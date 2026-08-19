#pragma once

#include "CoreMinimal.h"

/** The base surface under one point: how it leans, and how far up the local relief it sits. */
struct FMassifErosionInput
{
	FVector2d PositionMeters = FVector2d::ZeroVector;

	/** Gradient of the base surface here, meters of rise per meter travelled. Seeds the gully directions. */
	FVector2d SlopeMetersPerMeter = FVector2d::ZeroVector;

	/** 0 on a valley floor, 1 at the top of the local relief. Decides where gullies fade out. */
	float ReliefFraction = 0.0f;
};

struct FMassifErosionSettings
{
	int32 OctaveCount = 10;

	/**
	 * Weight on the finest octave included. A caller sampling coarsely drops the
	 * octaves it cannot resolve, and fades the last one it keeps rather than
	 * switching it off, so two samplings of the same point at different spacings
	 * agree instead of stepping at the boundary between them.
	 */
	float FinestOctaveWeight = 1.0f;

	/** Spacing of the widest gullies; each octave divides it by WavelengthGain's reciprocal. */
	double LargestGullyWavelengthMeters = 1800.0;
	double LargestGullyDepthMeters = 140.0;
	double WavelengthGain = 0.5;
	double DepthGain = 0.5;

	/**
	 * Stripes per pivot cell. The reference keeps this close to 1: much above it
	 * the phase interpolation between cells visibly distorts the stripes.
	 */
	double CellsPerWavelength = 1.0;

	/** Surface gradient at which gullies reach full depth; gentler ground gets less. */
	float SlopeReference = 0.35f;

	/** Above 1, later octaves keep cutting on ground where the earlier ones had already faded. */
	float DetailPersistence = 1.0f;

	/**
	 * Amplitude of the stripes relative to the fade base, and how strongly each
	 * octave's stripes steer the finer ones — the reference's gully weight.
	 * This is what makes gullies branch instead of running parallel.
	 */
	float BranchStrength = 1.0f;

	/** Mask smoothing at that end of the gully profile: 1 eases gullies in gently, 0 snaps them on. */
	float RidgeRounding = 0.6f;
	float CreaseRounding = 0.15f;

	uint32 Seed = 0;
};

/**
 * Analytic erosion filter: a C++ translation of Rune Skovbo Johansen's "Fast
 * and Gorgeous Erosion Filter" (2026) and its Phacelle Noise, via Luke
 * Mitchell's C# conversion. Original code under the Mozilla Public License
 * v2.0; see the notice in the .cpp.
 *
 * Instead of moving water and sediment over a grid, it lays cosine stripes
 * across the local downhill direction, so every trough is a gully running
 * down it. Each octave adds its stripes' steepest-slope contribution to the
 * gradient the finer octaves orient by, which is what makes small gullies
 * branch off large ones the way a drainage network does.
 *
 * Every point is evaluated from its own inputs alone: no grid, no iteration,
 * no neighbours. That is what lets terrain be sampled in tiles at any
 * resolution without generating anything around them.
 */
namespace MassifErosionFilter
{
	/**
	 * Meters to add to the base surface at this point, positive on the spurs
	 * between gullies and negative in them.
	 *
	 * Cutting downwards only would sink every valley floor by the full gully
	 * depth, so the cut is measured against the depth the same point would
	 * receive with no gullies at all. Ground too flat to hold gullies therefore
	 * comes out untouched, at both ends of the relief.
	 */
	double EvaluateDisplacementMeters(const FMassifErosionInput& Input, const FMassifErosionSettings& Settings);
}
