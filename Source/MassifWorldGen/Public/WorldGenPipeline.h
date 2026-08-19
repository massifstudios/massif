#pragma once

#include "CoreMinimal.h"
#include "WorldGenConfig.h"
#include "WorldGenResult.h"

class FMassifTerrainField;

/**
 * Builds the terrain field, then materialises one window of it into a plain
 * array.
 *
 * The array is scaffolding. The field has no resolution and the world has no
 * heightfield; this exists so the M1 chunk renderer and the debug export keep
 * working until M5 samples the field directly into clipmap rings.
 */
struct MASSIFWORLDGEN_API FWorldGenPipeline
{
	static FWorldGenResult Generate(const FWorldGenConfig& Config);

	/** Samples the window the config's preview settings describe, centred on the world origin. */
	static FWorldGenResult MaterialisePreviewWindow(const FMassifTerrainField& Field);
};
