#include "Foliage/MassifFoliageScatter.h"
#include "Foliage/MassifFoliageSpecies.h"
#include "MassifHabitat.h"
#include "MassifTerrainField.h"
#include "MassifTestTerrain.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 ScatterTestSeed = 4711;

	/** Tiles spread over the massif, enough that some of them land on ground a species takes. */
	constexpr int32 ScannedTilesPerSide = 8;
	constexpr double ScatterTestSpacingMeters = 1.0;

	/**
	 * Tile coordinates spanning the whole test world rather than a corner of it.
	 * A species occupies one altitude band, and the world runs from valley floor
	 * to summit, so tiles clustered anywhere in particular meet at most a few
	 * species' conditions.
	 */
	TArray<FIntPoint> SpreadTiles(const FMassifHabitatField& Habitat, const FMassifFoliageSpecies& Species)
	{
		const double WorldSizeMeters = Habitat.GetTerrainField().GetWorldSizeMeters();
		const int32 HalfTileCount = FMath::FloorToInt32(0.5 * WorldSizeMeters / Species.TileSizeMeters);
		const int32 Stride = FMath::Max(2 * HalfTileCount / (ScannedTilesPerSide - 1), 1);

		TArray<FIntPoint> Tiles;
		Tiles.Reserve(ScannedTilesPerSide * ScannedTilesPerSide);
		for (int32 Index = 0; Index < ScannedTilesPerSide * ScannedTilesPerSide; ++Index)
		{
			Tiles.Emplace(
				-HalfTileCount + Stride * (Index % ScannedTilesPerSide),
				-HalfTileCount + Stride * (Index / ScannedTilesPerSide));
		}
		return Tiles;
	}

	/** Points the coverage test reads the habitat at, per side of the test world. */
	constexpr int32 CoverageSamplesPerSide = 220;

	const FMassifFoliageSpecies& FindSpecies(const TCHAR* Name)
	{
		for (const FMassifFoliageSpecies& Species : MassifFoliage::Species)
		{
			if (FCString::Strcmp(Species.Name, Name) == 0)
			{
				return Species;
			}
		}
		checkf(false, TEXT("No foliage species named %s"), Name);
		return MassifFoliage::Species[0];
	}

	/**
	 * The habitat over the whole test world, sampled once and reused for every
	 * species. Scattering all of them instead would cost millions of habitat
	 * samples to answer a question about the table, not about the scatter.
	 */
	TArray<FMassifHabitatSample> SampleWholeWorld(const FMassifHabitatField& Habitat)
	{
		const double WorldSizeMeters = Habitat.GetTerrainField().GetWorldSizeMeters();
		const double StepMeters = WorldSizeMeters / CoverageSamplesPerSide;

		TArray<FMassifHabitatSample> Samples;
		Samples.Reserve(CoverageSamplesPerSide * CoverageSamplesPerSide);
		for (int32 Index = 0; Index < CoverageSamplesPerSide * CoverageSamplesPerSide; ++Index)
		{
			const FVector2d PositionMeters(
				(Index % CoverageSamplesPerSide + 0.5) * StepMeters - 0.5 * WorldSizeMeters,
				(Index / CoverageSamplesPerSide + 0.5) * StepMeters - 0.5 * WorldSizeMeters);
			Samples.Add(Habitat.Sample(PositionMeters, ScatterTestSpacingMeters));
		}
		return Samples;
	}

	int32 CountSuitableGround(
		const TArray<FMassifHabitatSample>& Samples, const FMassifFoliageSpecies& Species)
	{
		int32 SuitableCount = 0;
		for (const FMassifHabitatSample& Sample : Samples)
		{
			SuitableCount += MassifFoliageScatter::HabitatWeight(Species, Sample) > 0.0 ? 1 : 0;
		}
		return SuitableCount;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageScatterTest,
	"Massif.Presentation.FoliageScatter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFoliageScatterTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHabitatField> Habitat = MassifTestTerrain::MakeHabitat(ScatterTestSeed);
	const FMassifFoliageSpecies& Species = FindSpecies(TEXT("PiceaAbies"));
	const uint32 SpeciesSeed = MassifFoliageScatter::MakeSpeciesSeed(ScatterTestSeed, Species.Name);

	int32 TotalInstanceCount = 0;
	int32 BusiestTileX = 0;
	int32 BusiestTileY = 0;
	int32 BusiestTileCount = 0;
	TArray<MassifFoliageScatter::FInstance> Instances;
	for (const FIntPoint& Tile : SpreadTiles(*Habitat, Species))
	{
		const int32 TileX = Tile.X;
		const int32 TileY = Tile.Y;
		MassifFoliageScatter::ScatterTile(*Habitat, Species, SpeciesSeed, TileX, TileY, Instances);
		TotalInstanceCount += Instances.Num();
		if (Instances.Num() > BusiestTileCount)
		{
			BusiestTileCount = Instances.Num();
			BusiestTileX = TileX;
			BusiestTileY = TileY;
		}

		// Every plant stands on ground its species accepts, inside its own tile.
		for (const MassifFoliageScatter::FInstance& Instance : Instances)
		{
			const FVector2d PositionMeters(Instance.LocationMeters.X, Instance.LocationMeters.Y);
			const bool bInsideTile =
				PositionMeters.X >= TileX * Species.TileSizeMeters
				&& PositionMeters.X < (TileX + 1) * Species.TileSizeMeters
				&& PositionMeters.Y >= TileY * Species.TileSizeMeters
				&& PositionMeters.Y < (TileY + 1) * Species.TileSizeMeters;
			if (!bInsideTile)
			{
				AddError(FString::Printf(TEXT("Instance at %s is outside tile %d,%d"),
					*PositionMeters.ToString(), TileX, TileY));
				return false;
			}

			const FMassifHabitatSample Ground = Habitat->Sample(PositionMeters, ScatterTestSpacingMeters);
			if (MassifFoliageScatter::HabitatWeight(Species, Ground) <= 0.0)
			{
				AddError(FString::Printf(
					TEXT("Instance at %.0f m stands on ground the species rejects: %.1f C, %.2f humidity, %.1f deg"),
					Ground.AltitudeMeters, Ground.TemperatureCelsius, Ground.Humidity, Ground.SlopeDegrees));
				return false;
			}
			if (!FMath::IsNearlyEqual(Instance.LocationMeters.Z, Ground.AltitudeMeters, 0.01))
			{
				AddError(FString::Printf(TEXT("Instance sits at %.2f m over %.2f m ground"),
					Instance.LocationMeters.Z, Ground.AltitudeMeters));
				return false;
			}
			if (Instance.Scale < Species.MinScale || Instance.Scale > Species.MaxScale)
			{
				AddError(FString::Printf(TEXT("Instance scale %.3f is outside %.2f-%.2f"),
					Instance.Scale, Species.MinScale, Species.MaxScale));
				return false;
			}
		}
	}

	TestTrue(FString::Printf(TEXT("Spruce grows somewhere on the scanned tiles (%d placed)"), TotalInstanceCount),
		TotalInstanceCount > 0);

	// The scatter is a pure function of seed, species and tile: the same tile
	// asked twice must come back identical, or the forest would reshuffle every
	// time the camera revisits it. Checked on the tile that grew the most, so a
	// bare patch of the test terrain cannot pass this by growing nothing twice.
	TArray<MassifFoliageScatter::FInstance> First;
	TArray<MassifFoliageScatter::FInstance> Second;
	MassifFoliageScatter::ScatterTile(*Habitat, Species, SpeciesSeed, BusiestTileX, BusiestTileY, First);
	MassifFoliageScatter::ScatterTile(*Habitat, Species, SpeciesSeed, BusiestTileX, BusiestTileY, Second);
	if (First.Num() != Second.Num())
	{
		AddError(FString::Printf(TEXT("Rescattering the same tile gave %d then %d instances"),
			First.Num(), Second.Num()));
		return false;
	}
	for (int32 Index = 0; Index < First.Num(); ++Index)
	{
		const bool bSame = First[Index].LocationMeters.Equals(Second[Index].LocationMeters, 0.0)
			&& First[Index].YawDegrees == Second[Index].YawDegrees
			&& First[Index].Scale == Second[Index].Scale;
		if (!bSame)
		{
			AddError(FString::Printf(TEXT("Instance %d differs between two scatters of the same tile"), Index));
			return false;
		}
	}

	// A different seed has to move the forest, or the seed is not reaching it.
	TArray<MassifFoliageScatter::FInstance> OtherSeed;
	MassifFoliageScatter::ScatterTile(
		*Habitat,
		Species,
		MassifFoliageScatter::MakeSpeciesSeed(ScatterTestSeed + 1, Species.Name),
		BusiestTileX,
		BusiestTileY,
		OtherSeed);
	const bool bMoved = OtherSeed.Num() != First.Num()
		|| (First.Num() > 0 && !OtherSeed[0].LocationMeters.Equals(First[0].LocationMeters, 0.001));
	TestTrue(TEXT("A different master seed scatters differently"), bMoved);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageSpeciesCoverageTest,
	"Massif.Presentation.FoliageSpeciesCoverage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFoliageSpeciesCoverageTest::RunTest(const FString& Parameters)
{
	// A species whose bands miss the massif it is planted on grows nothing at
	// all, which reads as a missing mesh rather than as a mistuned table.
	const TSharedRef<const FMassifHabitatField> Habitat = MassifTestTerrain::MakeHabitat(ScatterTestSeed);
	const TArray<FMassifHabitatSample> Samples = SampleWholeWorld(*Habitat);

	for (const FMassifFoliageSpecies& Species : MassifFoliage::Species)
	{
		const int32 SuitableCount = CountSuitableGround(Samples, Species);
		AddInfo(FString::Printf(TEXT("%s: %d of %d sampled points suit it"),
			Species.Name, SuitableCount, Samples.Num()));
		TestTrue(FString::Printf(TEXT("%s has ground it can grow on"), Species.Name), SuitableCount > 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageHabitatWeightTest,
	"Massif.Presentation.FoliageHabitatWeight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFoliageHabitatWeightTest::RunTest(const FString& Parameters)
{
	const FMassifFoliageSpecies& Spruce = FindSpecies(TEXT("PiceaAbies"));

	FMassifHabitatSample Ideal;
	Ideal.AltitudeMeters = 1400.0;
	Ideal.SlopeDegrees = 12.0;
	Ideal.TemperatureCelsius = 0.5 * (Spruce.MinTemperatureCelsius + Spruce.MaxTemperatureCelsius);
	Ideal.Humidity = Spruce.MinHumidity + 2.0 * Spruce.HumidityFade;
	Ideal.GroundType = EMassifGroundType::Soil;
	TestEqual(TEXT("Ground in the middle of every band is fully stocked"),
		MassifFoliageScatter::HabitatWeight(Spruce, Ideal), 1.0);

	// Each condition alone has to be able to empty the ground: nothing grows
	// where it is too cold, and being damp and gentle does not make up for it.
	FMassifHabitatSample TooCold = Ideal;
	TooCold.TemperatureCelsius = Spruce.MinTemperatureCelsius - 0.1;
	TestEqual(TEXT("Nothing grows below the species' temperature"),
		MassifFoliageScatter::HabitatWeight(Spruce, TooCold), 0.0);

	FMassifHabitatSample TooDry = Ideal;
	TooDry.Humidity = Spruce.MinHumidity - 0.01;
	TestEqual(TEXT("Nothing grows on ground drier than the species takes"),
		MassifFoliageScatter::HabitatWeight(Spruce, TooDry), 0.0);

	FMassifHabitatSample TooSteep = Ideal;
	TooSteep.SlopeDegrees = Spruce.MaxSlopeDegrees + 0.1;
	TestEqual(TEXT("Nothing grows on ground steeper than the species holds"),
		MassifFoliageScatter::HabitatWeight(Spruce, TooSteep), 0.0);

	FMassifHabitatSample WrongGround = Ideal;
	WrongGround.GroundType = EMassifGroundType::Rock;
	TestEqual(TEXT("Trees do not root in rock however good the climate"),
		MassifFoliageScatter::HabitatWeight(Spruce, WrongGround), 0.0);

	// Between the edge and the middle of a band, the species thins rather than stopping.
	FMassifHabitatSample NearTreeline = Ideal;
	NearTreeline.TemperatureCelsius = Spruce.MinTemperatureCelsius + 0.5 * Spruce.TemperatureFadeCelsius;
	const double ThinnedWeight = MassifFoliageScatter::HabitatWeight(Spruce, NearTreeline);
	TestTrue(FString::Printf(TEXT("The treeline thins out instead of stopping dead (%.2f)"), ThinnedWeight),
		ThinnedWeight > 0.0 && ThinnedWeight < 1.0);

	// A damp slope suits the spruce; the same ground is too wet for the pine.
	const FMassifFoliageSpecies& Pine = FindSpecies(TEXT("PinusSylvestris"));
	FMassifHabitatSample DampHollow = Ideal;
	DampHollow.Humidity = 0.85;
	TestTrue(TEXT("Spruce and pine split the same slope between them"),
		MassifFoliageScatter::HabitatWeight(Spruce, DampHollow)
			> MassifFoliageScatter::HabitatWeight(Pine, DampHollow));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAspectWeightTest,
	"Massif.Presentation.FoliageAspectWeight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFoliageAspectWeightTest::RunTest(const FString& Parameters)
{
	// Alpenrose picks the north flank for the snow cover, not for the cold, so
	// the preference has to survive the two slopes being the same temperature.
	const FMassifFoliageSpecies& Alpenrose = FindSpecies(TEXT("RhododendronFerrugineum"));

	FMassifHabitatSample NorthFacing;
	NorthFacing.SlopeDegrees = 25.0;
	NorthFacing.AspectDegrees = 0.0;
	NorthFacing.TemperatureCelsius =
		0.5 * (Alpenrose.MinTemperatureCelsius + Alpenrose.MaxTemperatureCelsius);
	NorthFacing.Humidity = Alpenrose.MinHumidity + 2.0 * Alpenrose.HumidityFade;
	NorthFacing.GroundType = EMassifGroundType::Soil;

	FMassifHabitatSample SouthFacing = NorthFacing;
	SouthFacing.AspectDegrees = 180.0;

	const double NorthWeight = MassifFoliageScatter::HabitatWeight(Alpenrose, NorthFacing);
	const double SouthWeight = MassifFoliageScatter::HabitatWeight(Alpenrose, SouthFacing);
	TestEqual(TEXT("The favoured bearing is not penalised at all"), NorthWeight, 1.0);
	TestTrue(FString::Printf(TEXT("The opposite bearing is thinned to %.2f"), SouthWeight),
		SouthWeight < NorthWeight && SouthWeight > 0.0);

	// Flat ground faces nowhere, so a bearing read off an almost level normal
	// must not be allowed to reject it.
	FMassifHabitatSample Level = SouthFacing;
	Level.SlopeDegrees = 0.0;
	TestEqual(TEXT("Flat ground is not judged on the bearing it nominally faces"),
		MassifFoliageScatter::HabitatWeight(Alpenrose, Level), 1.0);

	// A species with no preference sees the same ground the same way whichever
	// way it faces.
	const FMassifFoliageSpecies& Spruce = FindSpecies(TEXT("PiceaAbies"));
	FMassifHabitatSample Neutral = NorthFacing;
	Neutral.TemperatureCelsius = 0.5 * (Spruce.MinTemperatureCelsius + Spruce.MaxTemperatureCelsius);
	Neutral.Humidity = Spruce.MinHumidity + 2.0 * Spruce.HumidityFade;
	FMassifHabitatSample NeutralSouth = Neutral;
	NeutralSouth.AspectDegrees = 180.0;
	TestEqual(TEXT("A species without a preference ignores aspect"),
		MassifFoliageScatter::HabitatWeight(Spruce, Neutral),
		MassifFoliageScatter::HabitatWeight(Spruce, NeutralSouth));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliagePatchingTest,
	"Massif.Presentation.FoliagePatching",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFoliagePatchingTest::RunTest(const FString& Parameters)
{
	// Aspen spreads by suckers, so it has to come out in stands with gaps
	// between them rather than as an even sprinkle over everything it tolerates.
	const TSharedRef<const FMassifHabitatField> Habitat = MassifTestTerrain::MakeHabitat(ScatterTestSeed);
	const FMassifFoliageSpecies& Aspen = FindSpecies(TEXT("PopulusTremula"));
	const uint32 SpeciesSeed = MassifFoliageScatter::MakeSpeciesSeed(ScatterTestSeed, Aspen.Name);

	int32 PlantedCount = 0;
	int32 SuitableCount = 0;
	TArray<MassifFoliageScatter::FInstance> Instances;
	for (const FIntPoint& Tile : SpreadTiles(*Habitat, Aspen))
	{
		MassifFoliageScatter::ScatterTile(*Habitat, Aspen, SpeciesSeed, Tile.X, Tile.Y, Instances);
		PlantedCount += Instances.Num();
		for (const MassifFoliageScatter::FInstance& Instance : Instances)
		{
			const FVector2d PositionMeters(Instance.LocationMeters.X, Instance.LocationMeters.Y);
			SuitableCount +=
				MassifFoliageScatter::HabitatWeight(Aspen, Habitat->Sample(PositionMeters, 1.0)) > 0.0 ? 1 : 0;
		}
	}

	TestTrue(FString::Printf(TEXT("Aspen still grows where it belongs (%d placed)"), PlantedCount),
		PlantedCount > 0);
	TestEqual(TEXT("Patching only removes plants, it never moves them onto ground the species rejects"),
		SuitableCount, PlantedCount);

	return true;
}
