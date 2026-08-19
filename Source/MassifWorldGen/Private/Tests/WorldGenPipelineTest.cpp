#include "GpuErosion.h"
#include "Misc/AutomationTest.h"
#include "WorldGenPipeline.h"

namespace
{
	FWorldGenConfig MakeTestConfig(int32 MasterSeed)
	{
		FWorldGenConfig Config;
		Config.MasterSeed = MasterSeed;
		Config.Resolution = 257;
		Config.WorldSizeMeters = 2048.0f;
		Config.HydraulicIterationCount = 0;
		Config.ThermalIterationCount = 0;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldGenDeterminismTest,
	"Massif.WorldGen.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWorldGenDeterminismTest::RunTest(const FString& Parameters)
{
	const FWorldGenResult FirstRun = FWorldGenPipeline::Generate(MakeTestConfig(1337));
	const FWorldGenResult SecondRun = FWorldGenPipeline::Generate(MakeTestConfig(1337));
	const FWorldGenResult OtherSeedRun = FWorldGenPipeline::Generate(MakeTestConfig(99));

	TestTrue(TEXT("Result is valid"), FirstRun.IsValid());
	TestEqual(TEXT("Same seed produces identical heightfield hash"), FirstRun.ComputeHash(), SecondRun.ComputeHash());
	TestNotEqual(TEXT("Different seed produces different heightfield hash"), FirstRun.ComputeHash(), OtherSeedRun.ComputeHash());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldGenErosionTest,
	"Massif.WorldGen.Erosion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWorldGenErosionTest::RunTest(const FString& Parameters)
{
	if (!FMassifGpuErosion::CanRun())
	{
		AddInfo(TEXT("No rendering RHI; GPU erosion test skipped"));
		return true;
	}

	FWorldGenConfig Config = MakeTestConfig(1337);
	Config.HydraulicIterationCount = 1500;
	Config.ThermalIterationCount = 500;

	const FWorldGenResult Uneroded = FWorldGenPipeline::Generate(MakeTestConfig(1337));
	const FWorldGenResult FirstRun = FWorldGenPipeline::Generate(Config);
	const FWorldGenResult SecondRun = FWorldGenPipeline::Generate(Config);

	TestNotEqual(TEXT("Erosion changes the heightfield"), FirstRun.ComputeHash(), Uneroded.ComputeHash());
	TestEqual(TEXT("Erosion is reproducible on the same machine"), FirstRun.ComputeHash(), SecondRun.ComputeHash());

	// Full iteration counts must stay numerically stable: finite, inside a
	// sane band around the configured altitude range, and without needle
	// spikes (bounded cell-to-cell delta).
	const float LowBound = Config.MinAltitudeMeters - 100.0f;
	const float HighBound = Config.MaxAltitudeMeters + 100.0f;
	float MaxNeighborDelta = 0.0f;
	for (int32 Index = 0; Index < FirstRun.HeightMeters.Num(); ++Index)
	{
		const float Height = FirstRun.HeightMeters[Index];
		if (!FMath::IsFinite(Height) || Height < LowBound || Height > HighBound)
		{
			AddError(FString::Printf(TEXT("Unstable height %f at index %d"), Height, Index));
			return false;
		}
		const bool bHasRightNeighbor = (Index % Config.Resolution) + 1 < Config.Resolution;
		if (bHasRightNeighbor)
		{
			MaxNeighborDelta = FMath::Max(MaxNeighborDelta, FMath::Abs(FirstRun.HeightMeters[Index + 1] - Height));
		}
	}
	TestTrue(FString::Printf(TEXT("No needle spikes (max neighbor delta %.1f m)"), MaxNeighborDelta),
		MaxNeighborDelta < 60.0f);

	// Erosion must cut channels, not pepper the map with pits. Uneroded noise
	// sits near zero, so anything far above it means cells are digging holes.
	const float ErodedPitFraction = static_cast<float>(FirstRun.CountLocalMinima()) / static_cast<float>(FirstRun.HeightMeters.Num());
	const float UnerodedPitFraction = static_cast<float>(Uneroded.CountLocalMinima()) / static_cast<float>(Uneroded.HeightMeters.Num());
	AddInfo(FString::Printf(TEXT("Pits: %.3f%% eroded, %.3f%% uneroded"), 100.0f * ErodedPitFraction, 100.0f * UnerodedPitFraction));
	TestTrue(FString::Printf(TEXT("Erosion leaves few pits (%.3f%% of cells)"), 100.0f * ErodedPitFraction),
		ErodedPitFraction < 0.01f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldGenQueryTest,
	"Massif.WorldGen.Queries",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWorldGenQueryTest::RunTest(const FString& Parameters)
{
	const FWorldGenConfig Config = MakeTestConfig(1337);
	const FWorldGenResult Result = FWorldGenPipeline::Generate(Config);

	const float HalfSize = Config.WorldSizeMeters * 0.5f;
	for (float WorldY = -HalfSize; WorldY <= HalfSize; WorldY += 256.0f)
	{
		for (float WorldX = -HalfSize; WorldX <= HalfSize; WorldX += 256.0f)
		{
			const float Height = Result.SampleHeightMeters(WorldX, WorldY);
			TestTrue(TEXT("Sampled height within configured altitude range"),
				Height >= Config.MinAltitudeMeters && Height <= Config.MaxAltitudeMeters);
		}
	}

	const float CenterHeight = Result.SampleHeightMeters(0.0f, 0.0f);
	const float PixelHeight = Result.GetHeightAtPixel(Config.Resolution / 2, Config.Resolution / 2);
	TestEqual(TEXT("Center sample matches center pixel"), CenterHeight, PixelHeight, 0.01f);

	const float Slope = Result.SampleSlopeDegrees(100.0f, 100.0f);
	TestTrue(TEXT("Slope in [0, 90] degrees"), Slope >= 0.0f && Slope <= 90.0f);

	const float NormalZ = Result.SampleNormal(100.0f, 100.0f).Z;
	TestTrue(TEXT("Normal points upward"), NormalZ > 0.0f);

	const float OutsideHeight = Result.SampleHeightMeters(Config.WorldSizeMeters * 2.0f, 0.0f);
	TestTrue(TEXT("Out-of-bounds sample clamps to border and stays in range"),
		OutsideHeight >= Config.MinAltitudeMeters && OutsideHeight <= Config.MaxAltitudeMeters);

	return true;
}
