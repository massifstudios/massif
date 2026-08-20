#pragma once

#include "CoreMinimal.h"

class FMassifHabitatField;
struct FMassifFoliageSpecies;
struct FMassifHabitatSample;

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
	 * How well one species suits one patch of ground, 0 (never grows there) to 1
	 * (as good as it gets). The ground type gates it outright — nothing roots in
	 * a cliff or a snowfield it cannot tolerate — and temperature, humidity,
	 * steepness and which way the slope faces each thin it towards the edge of
	 * what the species takes.
	 *
	 * Multiplied rather than averaged: a plant that cannot stand the cold is not
	 * rescued by the ground being damp.
	 *
	 * Position-dependent thinning — the stands a clonal species grows in — is not
	 * here, because it is a property of where the point is rather than of what
	 * the ground is like. ScatterTile applies it on top.
	 */
	double HabitatWeight(const FMassifFoliageSpecies& Species, const FMassifHabitatSample& Habitat);

	/**
	 * Every plant of one species on one tile, as a pure function of the habitat,
	 * the species, the seed and the tile coordinate. No global state and no
	 * dependence on neighbouring tiles, so a tile scattered alone is identical
	 * to the same tile scattered in any order alongside others — the same
	 * contract the terrain field holds to, and what lets this run on the thread
	 * pool and be tested headless.
	 *
	 * Candidates sit on a jittered grid rather than uniform random positions,
	 * so plants spread out instead of clumping, then the habitat weight thins
	 * them: the same species is dense where the ground suits it and scattered
	 * where it barely holds on. A species that grows in stands is thinned again
	 * by a noise field read from the world position, which crosses tile edges
	 * without seeing them.
	 */
	void ScatterTile(
		const FMassifHabitatField& Habitat,
		const FMassifFoliageSpecies& Species,
		uint32 SpeciesSeed,
		int32 TileX,
		int32 TileY,
		TArray<FInstance>& OutInstances);

	/** Seed for one species, derived from the master seed and the species name. */
	uint32 MakeSpeciesSeed(uint32 MasterSeed, const TCHAR* SpeciesName);
}
