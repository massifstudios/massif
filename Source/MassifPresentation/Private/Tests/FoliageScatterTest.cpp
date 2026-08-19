#include "Foliage/MassifFoliageScatter.h"
#include "Foliage/MassifFoliageSpecies.h"
#include "MassifTerrainField.h"
#include "MassifTestTerrain.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 ScatterTestSeed = 4711;

	/** Tiles over a strip of the massif, enough that some of them land inside a species' band. */
	constexpr int32 ScannedTilesPerSide = 6;
	constexpr double GroundSampleSpacingMeters = 1.0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageScatterTest,
	"Massif.Presentation.FoliageScatter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFoliageScatterTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifTerrainField> Field = MassifTestTerrain::MakeField();
	const FMassifFoliageSpecies& Species = MassifFoliage::Species[0];
	const uint32 SpeciesSeed = MassifFoliageScatter::MakeSpeciesSeed(ScatterTestSeed, Species.Name);

	int32 TotalInstanceCount = 0;
	TArray<MassifFoliageScatter::FInstance> Instances;
	for (int32 TileIndex = 0; TileIndex < ScannedTilesPerSide * ScannedTilesPerSide; ++TileIndex)
	{
		const int32 TileX = TileIndex % ScannedTilesPerSide;
		const int32 TileY = TileIndex / ScannedTilesPerSide;
		MassifFoliageScatter::ScatterTile(*Field, Species, SpeciesSeed, TileX, TileY, Instances);
		TotalInstanceCount += Instances.Num();

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
			if (Instance.LocationMeters.Z < Species.MinAltitudeMeters
				|| Instance.LocationMeters.Z > Species.MaxAltitudeMeters)
			{
				AddError(FString::Printf(TEXT("Instance at %.0f m is outside the %.0f-%.0f m band"),
					Instance.LocationMeters.Z, Species.MinAltitudeMeters, Species.MaxAltitudeMeters));
				return false;
			}
			const double SlopeDegrees = Field->SampleSlopeDegrees(PositionMeters, GroundSampleSpacingMeters);
			if (SlopeDegrees > Species.MaxSlopeDegrees)
			{
				AddError(FString::Printf(TEXT("Instance on %.1f deg ground exceeds the %.1f deg limit"),
					SlopeDegrees, Species.MaxSlopeDegrees));
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
	// time the camera revisits it.
	TArray<MassifFoliageScatter::FInstance> First;
	TArray<MassifFoliageScatter::FInstance> Second;
	MassifFoliageScatter::ScatterTile(*Field, Species, SpeciesSeed, 3, 2, First);
	MassifFoliageScatter::ScatterTile(*Field, Species, SpeciesSeed, 3, 2, Second);
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
		*Field, Species, MassifFoliageScatter::MakeSpeciesSeed(ScatterTestSeed + 1, Species.Name), 3, 2, OtherSeed);
	const bool bMoved = OtherSeed.Num() != First.Num()
		|| (First.Num() > 0 && !OtherSeed[0].LocationMeters.Equals(First[0].LocationMeters, 0.001));
	TestTrue(TEXT("A different master seed scatters differently"), bMoved);

	return true;
}
