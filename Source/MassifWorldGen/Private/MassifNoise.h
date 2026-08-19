#pragma once

#include "CoreMinimal.h"

/**
 * Deterministic hash-based gradient noise. Pure integer/float math with no
 * tables or global state, so results are identical on every platform and
 * independent of evaluation order.
 *
 * Coordinates are doubles. The world is 150 km across and wants centimeter
 * detail, and a float lattice coordinate that far out resolves only a
 * fraction of a cell: the noise would visibly coarsen with distance from the
 * origin. Every entry point splits the coordinate into an integer cell and a
 * fraction within it before any float math, so precision is the same
 * everywhere.
 */
namespace MassifNoise
{
	/** Perlin-style gradient noise in [-1, 1]. */
	float GradientNoise(double X, double Y, uint32 Seed);

	/** Standard fractional Brownian motion, roughly [-1, 1]. */
	float Fractal(double X, double Y, uint32 Seed, int32 OctaveCount, double Lacunarity, float Gain);

	/**
	 * Ridged multifractal in [0, 1]; high values form ridge lines.
	 * SharpnessExponent shapes each octave's ridge profile: 1.0 gives soft
	 * rounded crests, 2.0 gives knife edges.
	 */
	float RidgedFractal(double X, double Y, uint32 Seed, int32 OctaveCount, double Lacunarity, float Gain, float SharpnessExponent);

	/** Deterministic point in [0, 1)^2 for one lattice cell, as Worley-style noise uses. */
	FVector2f CellPoint(int64 CellX, int64 CellY, uint32 Seed);
}
