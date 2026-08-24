#include "MassifTreePass.h"

#include "Async/ParallelFor.h"
#include "MassifFoliageScatter.h"
#include "MassifFoliageSpecies.h"
#include "MassifHabitat.h"
#include "MassifObjectStore.h"
#include "MassifObjectTypeRegistry.h"
#include "MassifTerrainField.h"

namespace
{
	/** The square of tile coordinates that covers the world, in one species' tile size. */
	struct FTileRange
	{
		int32 FirstTile = 0;
		int32 SideCount = 0;

		int32 TileCount() const
		{
			return SideCount * SideCount;
		}
	};

	FTileRange MakeTileRange(double WorldSizeMeters, double TileSizeMeters)
	{
		const double HalfWorldMeters = 0.5 * WorldSizeMeters;

		FTileRange Range;
		Range.FirstTile = FMath::FloorToInt32(-HalfWorldMeters / TileSizeMeters);
		const int32 LastTile = FMath::CeilToInt32(HalfWorldMeters / TileSizeMeters) - 1;
		Range.SideCount = LastTile - Range.FirstTile + 1;
		return Range;
	}

	/**
	 * A tile's own square reaches past the world border, and a tree placed out there
	 * would be clamped into the edge chunk rather than dropped — which stacks a
	 * hedge along all four sides of the map.
	 */
	bool IsInsideWorld(const FVector3d& PositionMeters, double WorldSizeMeters)
	{
		const double HalfWorldMeters = 0.5 * WorldSizeMeters;
		return FMath::Abs(PositionMeters.X) <= HalfWorldMeters
			&& FMath::Abs(PositionMeters.Y) <= HalfWorldMeters;
	}

	FMassifObjectPlacement MakeTreePlacement(
		const FMassifFoliageSpecies& Species, const MassifFoliageScatter::FInstance& Instance)
	{
		FMassifObjectPlacement Placement;
		Placement.PositionMeters = Instance.LocationMeters;
		Placement.TypeId = Species.ObjectTypeId;
		Placement.RadiusMeters = Instance.Scale * Species.CrownRadiusMeters;
		Placement.Rotation = FRotator(0.0, Instance.YawDegrees, 0.0);

		// Everything a generated forest does not yet have an opinion about. The fields
		// exist because a simulated forest will: a tree that has stood since before the
		// world started, in full health, is what "no opinion" means here.
		Placement.BuriedFraction = 0.0;
		Placement.PlantedHalfMonths = 0;
		Placement.Vigour = 1.0;
		return Placement;
	}

	int32 DrainTile(
		const TArray<MassifFoliageScatter::FInstance>& Instances, const FMassifFoliageSpecies& Species,
		double WorldSizeMeters, const FMassifObjectTypeRegistry& Registry, FMassifObjectStore& Store)
	{
		int32 PlantedCount = 0;
		for (const MassifFoliageScatter::FInstance& Instance : Instances)
		{
			if (!IsInsideWorld(Instance.LocationMeters, WorldSizeMeters))
			{
				continue;
			}
			PlantedCount += static_cast<int32>(
				Store.AddPlacement(MakeTreePlacement(Species, Instance), Registry));
		}
		return PlantedCount;
	}

	/** Every tile of one species, scattered in parallel because no tile reads another. */
	TArray<TArray<MassifFoliageScatter::FInstance>> ScatterSpecies(
		const FMassifHabitatField& Habitat, const FMassifFoliageSpecies& Species,
		uint32 MasterSeed, const FTileRange& Range)
	{
		const uint32 SpeciesSeed = MassifFoliageScatter::MakeSpeciesSeed(MasterSeed, Species.Name);

		TArray<TArray<MassifFoliageScatter::FInstance>> TileInstances;
		TileInstances.SetNum(Range.TileCount());

		ParallelFor(Range.TileCount(), [&](int32 TileIndex)
		{
			const int32 TileX = Range.FirstTile + TileIndex % Range.SideCount;
			const int32 TileY = Range.FirstTile + TileIndex / Range.SideCount;
			MassifFoliageScatter::ScatterTile(
				Habitat, Species, SpeciesSeed, TileX, TileY, TileInstances[TileIndex]);
		});
		return TileInstances;
	}

	int32 PlantSpecies(
		const FMassifHabitatField& Habitat, const FMassifFoliageSpecies& Species,
		uint32 MasterSeed, const FMassifObjectTypeRegistry& Registry, FMassifObjectStore& Store)
	{
		const double WorldSizeMeters = Habitat.GetTerrainField().GetWorldSizeMeters();
		const FTileRange Range = MakeTileRange(WorldSizeMeters, Species.TileSizeMeters);
		const TArray<TArray<MassifFoliageScatter::FInstance>> TileInstances =
			ScatterSpecies(Habitat, Species, MasterSeed, Range);

		// Draining is serial: the store is written once and is not thread-safe, and
		// the scatter above is where the time actually goes.
		int32 PlantedCount = 0;
		for (const TArray<MassifFoliageScatter::FInstance>& Instances : TileInstances)
		{
			PlantedCount += DrainTile(Instances, Species, WorldSizeMeters, Registry, Store);
		}
		return PlantedCount;
	}
}

MassifTreePass::FResult MassifTreePass::PlantWorld(
	const FMassifHabitatField& Habitat, uint32 MasterSeed,
	const FMassifObjectTypeRegistry& Registry, FMassifObjectStore& Store)
{
	checkf(!Store.IsFinalized(), TEXT("Trees must be planted before the store closes to writes."));

	const double StartSeconds = FPlatformTime::Seconds();

	FResult Result;
	for (const FMassifFoliageSpecies& Species : MassifFoliage::Species)
	{
		if (Species.ObjectTypeId == MassifInvalidObjectTypeId)
		{
			continue;
		}
		Result.PlantedCount += PlantSpecies(Habitat, Species, MasterSeed, Registry, Store);
	}

	Result.Seconds = FPlatformTime::Seconds() - StartSeconds;
	return Result;
}
