#pragma once

#include "CoreMinimal.h"

class FMassifTerrainField;
struct FMassifFoliageSpecies;

namespace MassifFoliageScatter
{
	/** One placed plant, in absolute world meters; the manager converts to a component transform. */
	struct FInstance
	{
		FVector3d LocationMeters = FVector3d::ZeroVector;
		float YawDegrees = 0.0f;
		float Scale = 1.0f;
	};

	/**
	 * Every plant of one species on one tile, as a pure function of the field,
	 * the species, the seed and the tile coordinate. No global state and no
	 * dependence on neighbouring tiles, so a tile scattered alone is identical
	 * to the same tile scattered in any order alongside others — the same
	 * contract the terrain field holds to, and what lets this run on the thread
	 * pool and be tested headless.
	 *
	 * Candidates sit on a jittered grid rather than uniform random positions,
	 * so plants spread out instead of clumping, then altitude and slope weights
	 * thin them.
	 */
	void ScatterTile(
		const FMassifTerrainField& Field,
		const FMassifFoliageSpecies& Species,
		uint32 SpeciesSeed,
		int32 TileX,
		int32 TileY,
		TArray<FInstance>& OutInstances);

	/** Seed for one species, derived from the master seed and the species name. */
	uint32 MakeSpeciesSeed(uint32 MasterSeed, const TCHAR* SpeciesName);
}
