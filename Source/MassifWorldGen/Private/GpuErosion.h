#pragma once

#include "CoreMinimal.h"
#include "WorldGenConfig.h"

/** Spring cell that keeps injecting water for the whole erosion run. */
struct FMassifRiverSource
{
	int32 CellX = 0;
	int32 CellY = 0;
	float RateMetersPerSecond = 0.0f;
};

/**
 * Runs hydraulic + thermal erosion compute passes over a heightfield.
 * Blocks the calling (game) thread until GPU results are read back.
 * Requires a rendering-capable RHI; callers must check CanRun() first.
 */
struct FMassifGpuErosion
{
	static bool CanRun();

	/** Erodes HeightMeters (Resolution x Resolution) in place. Returns false on failure; the input is left untouched. */
	static bool Run(const FWorldGenConfig& Config, TArray<float>& HeightMeters, TConstArrayView<FMassifRiverSource> RiverSources);
};
