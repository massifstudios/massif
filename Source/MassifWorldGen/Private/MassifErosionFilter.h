#pragma once

#include "CoreMinimal.h"

/** The macro surface under one point: where it is, how it leans, how high up the local relief it sits. */
struct FMassifErosionInput
{
	FVector2f PositionMeters = FVector2f::ZeroVector;

	/** Gradient of the macro surface, meters of rise per meter travelled. Water runs against it. */
	FVector2f SlopeMetersPerMeter = FVector2f::ZeroVector;

	/** 0 on a valley floor, 1 at the top of the local relief. Decides where gullies fade out. */
	float ReliefFraction = 0.0f;
};

struct FMassifErosionSettings
{
	int32 OctaveCount = 5;

	/** Spacing of the widest gullies; each octave divides it by WavelengthGain's reciprocal. */
	float LargestGullyWavelengthMeters = 480.0f;
	float LargestGullyDepthMeters = 90.0f;
	float WavelengthGain = 0.5f;
	float DepthGain = 0.55f;

	/** Pivot lattice spacing as a multiple of the wavelength; below 1 the stripes lose coherence. */
	float CellsPerWavelength = 1.0f;

	/** Surface gradient at which gullies reach full depth; gentler ground gets less. */
	float SlopeReference = 0.35f;

	/** Above 1, later octaves keep cutting on ground where the earlier ones had already faded. */
	float DetailPersistence = 1.6f;

	/**
	 * How far a gully may bend the flow the next octave reads, as a fraction of
	 * the macro slope. This is what makes fine gullies branch off coarse ones,
	 * and the one dial that trades branching against a smooth surface: past
	 * roughly 0.3 the bending outruns the stripe pattern and tears it.
	 */
	float BranchStrength = 0.25f;

	/** 1 keeps the cosine profile, 0 pinches that end of the gully into a straight-sided edge. */
	float RidgeRounding = 0.6f;
	float CreaseRounding = 0.15f;

	uint32 Seed = 0;
};

/**
 * Analytic erosion filter after Rune Skovbo Johansen, "Fast and Gorgeous
 * Erosion Filter" (2026).
 *
 * Instead of moving water and sediment over a grid, it lays cosine stripes
 * across the local flow direction, so every trough is a gully running downhill.
 * Each octave folds its own gully slopes back into the flow direction before
 * the next, smaller octave is laid down, which is what makes the small gullies
 * branch off the large ones the way a drainage network does.
 *
 * Every point is evaluated from its own inputs alone: no grid, no iteration, no
 * neighbours, so the result is identical whatever order or chunking is used.
 */
namespace MassifErosionFilter
{
	/**
	 * Meters to add to the macro surface at this point, positive on the spurs
	 * between gullies and negative in them.
	 *
	 * Cutting downwards only would sink every valley floor by the full gully
	 * depth and drop the rivers below the elevations their graph gives them, so
	 * the cut is measured against the depth the same point would receive with no
	 * gullies at all. Ground too flat to hold gullies therefore comes out
	 * untouched, at both ends of the relief.
	 */
	float EvaluateDisplacementMeters(const FMassifErosionInput& Input, const FMassifErosionSettings& Settings);
}
