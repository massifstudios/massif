#include "MassifNoise.h"
#include "MassifTerrainField.h"
#include "Misc/AutomationTest.h"

namespace
{
	/** A world small enough to build in a test, keeping the scale relationships of the real one. */
	FWorldGenConfig MakeFieldTestConfig(int32 MasterSeed)
	{
		FWorldGenConfig Config;
		Config.MasterSeed = MasterSeed;
		Config.WorldSizeMeters = 24000.0f;
		Config.RangeSpacingMeters = 9000.0f;
		Config.RangeWarpWavelengthMeters = 12000.0f;
		Config.RangeWarpMeters = 2500.0f;
		Config.BakeCellSizeMeters = 64.0f;
		Config.BakeMaxIterationCount = 30;
		Config.RiverDrainageAreaThresholdSquareMeters = 1.0e6f;
		return Config;
	}

	double TotalGullyDepthMeters(const FWorldGenConfig& Config)
	{
		double TotalMeters = 0.0;
		double DepthMeters = Config.LargestGullyDepthMeters;
		for (int32 Octave = 0; Octave < Config.GullyOctaveCount; ++Octave)
		{
			TotalMeters += DepthMeters;
			DepthMeters *= Config.GullyDepthGain;
		}
		return TotalMeters;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainFieldChunkIndependenceTest,
	"Massif.WorldGen.TerrainField.ChunkIndependence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTerrainFieldChunkIndependenceTest::RunTest(const FString& Parameters)
{
	const FMassifTerrainField Field(MakeFieldTestConfig(1337));
	const double Spacing = 4.0;
	const FVector2d TileOrigin(-3200.0, 1100.0);

	// The property the whole streaming scheme rests on: a tile evaluated on its
	// own has to come out bit-identical to the same tile evaluated as part of
	// something larger, and in any order. Anything cached between samples that
	// depends on what was sampled before shows up here and nowhere else.
	TArray<double> Alone;
	Alone.Reserve(16 * 16);
	for (int32 StepY = 0; StepY < 16; ++StepY)
	{
		for (int32 StepX = 0; StepX < 16; ++StepX)
		{
			Alone.Add(Field.SampleHeightMeters(TileOrigin + FVector2d(StepX * Spacing, StepY * Spacing), Spacing));
		}
	}

	// Same points, walked backwards, with far-away samples interleaved.
	for (int32 Index = Alone.Num() - 1; Index >= 0; --Index)
	{
		const int32 StepX = Index % 16;
		const int32 StepY = Index / 16;
		Field.SampleHeightMeters(FVector2d(7000.0 - Index * 13.0, -5400.0 + Index * 7.0), 32.0);

		const double Interleaved = Field.SampleHeightMeters(TileOrigin + FVector2d(StepX * Spacing, StepY * Spacing), Spacing);
		if (Interleaved != Alone[Index])
		{
			AddError(FString::Printf(TEXT("Sample at tile cell (%d, %d) changed with evaluation order: %.9f then %.9f"),
				StepX, StepY, Alone[Index], Interleaved));
			return false;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainFieldLodAgreementTest,
	"Massif.WorldGen.TerrainField.LodAgreement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTerrainFieldLodAgreementTest::RunTest(const FString& Parameters)
{
	const FWorldGenConfig Config = MakeFieldTestConfig(1337);
	const FMassifTerrainField Field(Config);
	const double AllowedMeters = TotalGullyDepthMeters(Config);

	// A coarse sampling drops the octaves it cannot resolve, so it may differ
	// from a fine one, but only by what those octaves could have carried. A
	// bigger gap than that means the two are describing different ground, and
	// clipmap rings meeting at a boundary would show a step.
	double WorstDifference = 0.0;
	for (int32 Step = 0; Step < 400; ++Step)
	{
		const FVector2d Position(-4000.0 + Step * 17.0, 2200.0 - Step * 11.0);
		const double Fine = Field.SampleHeightMeters(Position, 1.0);
		const double Coarse = Field.SampleHeightMeters(Position, 64.0);
		WorstDifference = FMath::Max(WorstDifference, FMath::Abs(Fine - Coarse));
	}
	TestTrue(FString::Printf(TEXT("Coarse and fine samplings agree within the dropped octaves (%.1f m of %.1f m)"),
		WorstDifference, AllowedMeters), WorstDifference <= AllowedMeters);

	// Octave count must fall as spacing grows, and the finest kept must fade
	// rather than switch, or the rings step where they meet.
	float FinestWeight = 1.0f;
	const int32 FineOctaves = Field.OctavesForSpacing(1.0, FinestWeight);
	const int32 CoarseOctaves = Field.OctavesForSpacing(64.0, FinestWeight);
	TestTrue(FString::Printf(TEXT("Coarse sampling drops octaves (%d at 1 m, %d at 64 m)"), FineOctaves, CoarseOctaves),
		CoarseOctaves < FineOctaves);

	double PreviousWeightedCount = -1.0;
	for (double Spacing = 1.0; Spacing < 200.0; Spacing *= 1.15)
	{
		float Weight = 1.0f;
		const double WeightedCount = Field.OctavesForSpacing(Spacing, Weight) - 1.0 + Weight;
		if (PreviousWeightedCount >= 0.0 && WeightedCount > PreviousWeightedCount + 0.001)
		{
			AddError(FString::Printf(TEXT("Detail rose with spacing at %.2f m: %.3f octaves after %.3f"),
				Spacing, WeightedCount, PreviousWeightedCount));
			return false;
		}
		PreviousWeightedCount = WeightedCount;
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainFieldSurfaceTest,
	"Massif.WorldGen.TerrainField.Surface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTerrainFieldSurfaceTest::RunTest(const FString& Parameters)
{
	const FWorldGenConfig Config = MakeFieldTestConfig(1337);
	const FMassifTerrainField Field(Config);

	// Tearing and pitting are what every failed version of the erosion filter
	// produced, and both are invisible in a single sample. Sweeping a patch at
	// the spacing the ground will actually be built at catches them.
	const double Spacing = 2.0;
	const int32 Side = 200;
	TArray<double> Heights;
	Heights.SetNumUninitialized(Side * Side);
	for (int32 StepY = 0; StepY < Side; ++StepY)
	{
		for (int32 StepX = 0; StepX < Side; ++StepX)
		{
			Heights[StepY * Side + StepX] = Field.SampleHeightMeters(
				FVector2d(-1000.0 + StepX * Spacing, 600.0 + StepY * Spacing), Spacing);
		}
	}

	// A pit only counts when it is deep enough to be a hollow the ground cannot
	// drain, not a centimetre dimple the stripe filter leaves on smooth slopes.
	const double PitDepthFloorMeters = 0.25;
	double MaxNeighborDelta = 0.0;
	int32 PitCount = 0;
	for (int32 StepY = 1; StepY < Side - 1; ++StepY)
	{
		for (int32 StepX = 1; StepX < Side - 1; ++StepX)
		{
			const int32 Index = StepY * Side + StepX;
			const double Height = Heights[Index];
			MaxNeighborDelta = FMath::Max(MaxNeighborDelta, FMath::Abs(Heights[Index + 1] - Height));
			MaxNeighborDelta = FMath::Max(MaxNeighborDelta, FMath::Abs(Heights[Index + Side] - Height));
			const double LowestNeighbor = FMath::Min(
				FMath::Min(Heights[Index - 1], Heights[Index + 1]),
				FMath::Min(Heights[Index - Side], Heights[Index + Side]));
			PitCount += LowestNeighbor - Height > PitDepthFloorMeters;
		}
	}

	const double PitFraction = static_cast<double>(PitCount) / static_cast<double>(Side * Side);
	AddInfo(FString::Printf(TEXT("Max step %.1f m over %.0f m, %.3f%% pits"), MaxNeighborDelta, Spacing, 100.0 * PitFraction));
	TestTrue(FString::Printf(TEXT("No cliffs between adjacent samples (%.1f m over %.0f m)"), MaxNeighborDelta, Spacing),
		MaxNeighborDelta < 12.0);
	TestTrue(FString::Printf(TEXT("Surface is not pitted (%.3f%%)"), 100.0 * PitFraction), PitFraction < 0.005);

	for (const double Height : Heights)
	{
		if (!FMath::IsFinite(Height) || Height < Config.MinAltitudeMeters || Height > Config.MaxAltitudeMeters)
		{
			AddError(FString::Printf(TEXT("Height %f outside the configured altitude range"), Height));
			return false;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainFieldPrecisionTest,
	"Massif.WorldGen.TerrainField.Precision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTerrainFieldPrecisionTest::RunTest(const FString& Parameters)
{
	// A float lattice coordinate 75 km out resolves only a fraction of a fine
	// noise cell, so detail would quietly flatten with distance from the origin.
	// Same noise, same wavelength, near the origin and far from it: the fine
	// structure has to be as strong in both.
	const double Wavelength = 2.0;
	auto FineDetailRange = [Wavelength](double OffsetMeters)
	{
		float Lowest = TNumericLimits<float>::Max();
		float Highest = TNumericLimits<float>::Lowest();
		for (int32 Step = 0; Step < 512; ++Step)
		{
			const double Position = OffsetMeters + Step * 0.25;
			const float Value = MassifNoise::GradientNoise(Position / Wavelength, 0.5, 12345u);
			Lowest = FMath::Min(Lowest, Value);
			Highest = FMath::Max(Highest, Value);
		}
		return Highest - Lowest;
	};

	const float NearOrigin = FineDetailRange(0.0);
	const float FarOut = FineDetailRange(75000.0);
	TestTrue(FString::Printf(TEXT("Detail survives 75 km from the origin (%.3f near, %.3f far)"), NearOrigin, FarOut),
		FarOut > NearOrigin * 0.9f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainFieldSharedBakeTest,
	"Massif.WorldGen.TerrainField.SharedBake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTerrainFieldSharedBakeTest::RunTest(const FString& Parameters)
{
	const FWorldGenConfig Config = MakeFieldTestConfig(1337);
	const FMassifTerrainField Fresh(Config);

	// Adopting a solved bake is only usable for live tuning if it changes
	// nothing: with identical config, a field around the donor's bake has to be
	// bit-identical to one that ran the solve itself.
	const FMassifTerrainField SharedSameConfig(Config, Fresh.GetFluvialBakeShared());

	// And a gully-knob change on top of the shared bake must reach the ground:
	// same macro surface, different heights where slopes hold gullies.
	FWorldGenConfig DeeperGullies = Config;
	DeeperGullies.LargestGullyDepthMeters *= 2.0f;
	const FMassifTerrainField SharedDeeperGullies(DeeperGullies, Fresh.GetFluvialBakeShared());

	bool bGullyChangeReachedGround = false;
	for (int32 Step = 0; Step < 64; ++Step)
	{
		const FVector2d Position(-2000.0 + Step * 61.0, 900.0 - Step * 37.0);
		const double FreshHeight = Fresh.SampleHeightMeters(Position, 2.0);
		if (SharedSameConfig.SampleHeightMeters(Position, 2.0) != FreshHeight)
		{
			AddError(FString::Printf(TEXT("Shared bake changed the ground at (%.0f, %.0f)"), Position.X, Position.Y));
			return false;
		}
		bGullyChangeReachedGround |= SharedDeeperGullies.SampleHeightMeters(Position, 2.0) != FreshHeight;
	}
	TestTrue(TEXT("Deeper gullies on the shared bake make different ground"), bGullyChangeReachedGround);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainFieldDeterminismTest,
	"Massif.WorldGen.TerrainField.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTerrainFieldDeterminismTest::RunTest(const FString& Parameters)
{
	const FMassifTerrainField FirstRun(MakeFieldTestConfig(1337));
	const FMassifTerrainField SecondRun(MakeFieldTestConfig(1337));
	const FMassifTerrainField OtherSeed(MakeFieldTestConfig(99));

	bool bDifferedFromOtherSeed = false;
	for (int32 Step = 0; Step < 64; ++Step)
	{
		const FVector2d Position(-2000.0 + Step * 61.0, 900.0 - Step * 37.0);
		const double First = FirstRun.SampleHeightMeters(Position, 2.0);
		if (First != SecondRun.SampleHeightMeters(Position, 2.0))
		{
			AddError(FString::Printf(TEXT("Same seed gave different ground at (%.0f, %.0f)"), Position.X, Position.Y));
			return false;
		}
		bDifferedFromOtherSeed |= First != OtherSeed.SampleHeightMeters(Position, 2.0);
	}
	TestTrue(TEXT("A different seed makes different ground"), bDifferedFromOtherSeed);

	return true;
}
