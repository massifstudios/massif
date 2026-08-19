#pragma once

#include "CoreMinimal.h"
#include "WorldGenConfig.h"

/**
 * Runs hydraulic + thermal erosion compute passes over a heightfield.
 * Blocks the calling (game) thread until GPU results are read back.
 * Requires a rendering-capable RHI; callers must check CanRun() first.
 */
struct FMassifGpuErosion
{
	static bool CanRun();

	/** Erodes HeightMeters (Resolution x Resolution) in place. Returns false on failure; the input is left untouched. */
	static bool Run(const FWorldGenConfig& Config, TArray<float>& HeightMeters);
};
