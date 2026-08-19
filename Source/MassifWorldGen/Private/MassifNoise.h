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

	/**
	 * Ridged multifractal in [0, 1]; high values form ridge lines.
	 * SharpnessExponent shapes each octave's ridge profile: 1.0 gives soft
	 * rounded crests, 2.0 gives knife edges.
	 */
	float RidgedFractal(float X, float Y, uint32 Seed, int32 OctaveCount, float Lacunarity, float Gain, float SharpnessExponent);

	/** Deterministic point in [0, 1)^2 for one lattice cell, as Worley-style noise uses. */
	FVector2f CellPoint(int32 CellX, int32 CellY, uint32 Seed);
}
