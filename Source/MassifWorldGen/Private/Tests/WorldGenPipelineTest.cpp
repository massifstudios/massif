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
