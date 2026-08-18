#pragma once

#include "CoreMinimal.h"

/**
 * Deterministic hash-based gradient noise. Pure integer/float math with no
 * tables or global state, so results are identical on every platform and
 * independent of evaluation order.
 */
namespace MassifNoise
{
	/** Perlin-style gradient noise in [-1, 1]. */
	float GradientNoise(float X, float Y, uint32 Seed);

	/** Standard fractional Brownian motion, roughly [-1, 1]. */
	float Fractal(float X, float Y, uint32 Seed, int32 OctaveCount, float Lacunarity, float Gain);

	/** Ridged multifractal in [0, 1]; high values form sharp ridge lines. */
	float RidgedFractal(float X, float Y, uint32 Seed, int32 OctaveCount, float Lacunarity, float Gain);
}
