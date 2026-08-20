#include "Foliage/MassifFoliageScatter.h"
#include "Foliage/MassifFoliageSpecies.h"
#include "MassifHabitat.h"
#include "MassifTerrainField.h"
#include "MassifTestTerrain.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 ScatterTestSeed = 4711;

	/** Tiles over a strip of the massif, enough that some of them land on ground a species takes. */
	constexpr int32 ScannedTilesPerSide = 6;
	constexpr double GroundSampleSpacingMeters = 1.0;

	struct FSpeciesCoverage
	{
		int32 InstanceCount = 0;
		int32 OccupiedTileCount = 0;
	};

	FSpeciesCoverage MeasureCoverage(const FMassifHabitatField& Habitat, const FMassifFoliageSpecies& Species)
	{
		const uint32 SpeciesSeed = MassifFoliageScatter::MakeSpeciesSeed(ScatterTestSeed, Species.Name);
		TArray<MassifFoliageScatter::FInstance> Instances;
		FSpeciesCoverage Coverage;
		for (int32 TileIndex = 0; TileIndex < ScannedTilesPerSide * ScannedTilesPerSide; ++TileIndex)
		{
			MassifFoliageScatter::ScatterTile(
				Habitat, Species, SpeciesSeed,
				TileIndex % ScannedTilesPerSide, TileIndex / ScannedTilesPerSide, Instances);
			Coverage.InstanceCount += Instances.Num();
			Coverage.OccupiedTileCount += FMath::Min(Instances.Num(), 1);
		}
		return Coverage;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageScatterTest,
	"Massif.Presentation.FoliageScatter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFoliageScatterTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHabitatField> Habitat = MassifTestTerrain::MakeHabitat(ScatterTestSeed);
	const FMassifFoliageSpecies& Species = MassifFoliage::Species[0];
	const uint32 SpeciesSeed = MassifFoliageScatter::MakeSpeciesSeed(ScatterTestSeed, Species.Name);

	int32 TotalInstanceCount = 0;
	int32 BusiestTileX = 0;
	int32 BusiestTileY = 0;
	int32 BusiestTileCount = 0;
	TArray<MassifFoliageScatter::FInstance> Instances;
	for (int32 TileIndex = 0; TileIndex < ScannedTilesPerSide * ScannedTilesPerSide; ++TileIndex)
	{
		const int32 TileX = TileIndex % ScannedTilesPerSide;
		const int32 TileY = TileIndex / ScannedTilesPerSide;
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

			const FMassifHabitatSample Ground = Habitat->Sample(PositionMeters, GroundSampleSpacingMeters);
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

	TestTrue(FString::Printf(TEXT("Conifers grow somewhere on the scanned strip (%d placed)"), TotalInstanceCount),
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

	for (const FMassifFoliageSpecies& Species : MassifFoliage::Species)
	{
		const FSpeciesCoverage Coverage = MeasureCoverage(*Habitat, Species);
		AddInfo(FString::Printf(TEXT("%s: %d instances over %d of %d tiles"),
			Species.Name, Coverage.InstanceCount, Coverage.OccupiedTileCount,
			ScannedTilesPerSide * ScannedTilesPerSide));
		TestTrue(FString::Printf(TEXT("%s grows somewhere on the test terrain"), Species.Name),
			Coverage.InstanceCount > 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageHabitatWeightTest,
	"Massif.Presentation.FoliageHabitatWeight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFoliageHabitatWeightTest::RunTest(const FString& Parameters)
{
	const FMassifFoliageSpecies& Fir = MassifFoliage::Species[0];

	FMassifHabitatSample Ideal;
	Ideal.AltitudeMeters = 1400.0;
	Ideal.SlopeDegrees = 12.0;
	Ideal.TemperatureCelsius = 0.5 * (Fir.MinTemperatureCelsius + Fir.MaxTemperatureCelsius);
	Ideal.Humidity = Fir.MinHumidity + 2.0 * Fir.HumidityFade;
	Ideal.GroundType = EMassifGroundType::Soil;
	TestEqual(TEXT("Ground in the middle of every band is fully stocked"),
		MassifFoliageScatter::HabitatWeight(Fir, Ideal), 1.0);

	// Each condition alone has to be able to empty the ground: nothing grows
	// where it is too cold, and being damp and gentle does not make up for it.
	FMassifHabitatSample TooCold = Ideal;
	TooCold.TemperatureCelsius = Fir.MinTemperatureCelsius - 0.1;
	TestEqual(TEXT("Nothing grows below the species' temperature"),
		MassifFoliageScatter::HabitatWeight(Fir, TooCold), 0.0);

	FMassifHabitatSample TooDry = Ideal;
	TooDry.Humidity = Fir.MinHumidity - 0.01;
	TestEqual(TEXT("Nothing grows on ground drier than the species takes"),
		MassifFoliageScatter::HabitatWeight(Fir, TooDry), 0.0);

	FMassifHabitatSample TooSteep = Ideal;
	TooSteep.SlopeDegrees = Fir.MaxSlopeDegrees + 0.1;
	TestEqual(TEXT("Nothing grows on ground steeper than the species holds"),
		MassifFoliageScatter::HabitatWeight(Fir, TooSteep), 0.0);

	FMassifHabitatSample WrongGround = Ideal;
	WrongGround.GroundType = EMassifGroundType::Rock;
	TestEqual(TEXT("Trees do not root in rock however good the climate"),
		MassifFoliageScatter::HabitatWeight(Fir, WrongGround), 0.0);

	// Between the edge and the middle of a band, the species thins rather than stopping.
	FMassifHabitatSample NearTreeline = Ideal;
	NearTreeline.TemperatureCelsius = Fir.MinTemperatureCelsius + 0.5 * Fir.TemperatureFadeCelsius;
	const double ThinnedWeight = MassifFoliageScatter::HabitatWeight(Fir, NearTreeline);
	TestTrue(FString::Printf(TEXT("The treeline thins out instead of stopping dead (%.2f)"), ThinnedWeight),
		ThinnedWeight > 0.0 && ThinnedWeight < 1.0);

	// A damp shaded slope suits the fir; the same ground is too wet for the pine.
	const FMassifFoliageSpecies& Pine = MassifFoliage::Species[1];
	FMassifHabitatSample DampHollow = Ideal;
	DampHollow.Humidity = 0.85;
	TestTrue(TEXT("Fir and pine split the same slope between them"),
		MassifFoliageScatter::HabitatWeight(Fir, DampHollow) > MassifFoliageScatter::HabitatWeight(Pine, DampHollow));
	return true;
}
