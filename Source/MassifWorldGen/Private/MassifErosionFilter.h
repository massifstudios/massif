#pragma once

#include "CoreMinimal.h"

/** The macro surface under one point: how it leans, and how far up the local relief it sits. */
struct FMassifErosionInput
{
	FVector2d PositionMeters = FVector2d::ZeroVector;

	/** Gradient of the macro surface here, meters of rise per meter travelled. Sets how deep the gullies cut. */
	FVector2d SlopeMetersPerMeter = FVector2d::ZeroVector;

	/** 0 on a valley floor, 1 at the top of the local relief. Decides where gullies fade out. */
	float ReliefFraction = 0.0f;
};

struct FMassifErosionSettings
{
	int32 OctaveCount = 6;

	/**
	 * Weight on the finest octave included. A caller sampling coarsely drops the
	 * octaves it cannot resolve, and fades the last one it keeps rather than
	 * switching it off, so two samplings of the same point at different spacings
	 * agree instead of stepping at the boundary between them.
	 */
	float FinestOctaveWeight = 1.0f;

	/** Spacing of the widest gullies; each octave divides it by WavelengthGain's reciprocal. */
	double LargestGullyWavelengthMeters = 220.0;
	double LargestGullyDepthMeters = 18.0;
	double WavelengthGain = 0.5;
	double DepthGain = 0.5;

	/**
	 * Pivot lattice spacing as a multiple of the wavelength. This is what sets
	 * how long a gully runs before the phase shifts and it dies out, so at 1 a
	 * gully is about as long as it is wide and the slope comes out dimpled
	 * rather than grooved.
	 */
	double CellsPerWavelength = 3.0;

	/** Surface gradient at which gullies reach full depth; gentler ground gets less. */
	float SlopeReference = 0.35f;

	/** Above 1, later octaves keep cutting on ground where the earlier ones had already faded. */
	float DetailPersistence = 1.0f;

	/**
	 * How far a gully drags the finer gullies sideways, as a fraction of its own
	 * wavelength. This is what makes them branch off it instead of running
	 * parallel to it.
	 */
	float BranchStrength = 0.25f;

	/** 1 keeps the cosine profile, 0 pinches that end of the gully into a straight-sided edge. */
	float RidgeRounding = 0.6f;
	float CreaseRounding = 0.15f;

	uint32 Seed = 0;
};

/** Macro surface gradient anywhere, which the filter needs at each of its stripe pivots. */
using FMassifMacroSlopeSampler = TFunctionRef<FVector2d(FVector2d PositionMeters)>;

/**
 * Analytic erosion filter after Rune Skovbo Johansen, "Fast and Gorgeous
 * Erosion Filter" (2026).
 *
 * Instead of moving water and sediment over a grid, it lays cosine stripes
 * across the local flow direction, so every trough is a gully running downhill.
 * Each octave then drags the next, finer one sideways, which is what makes
 * small gullies branch off large ones the way a drainage network does.
 *
 * Stripes are oriented per pivot cell rather than per point. A point reads its
 * phase from a pivot up to a cell away, so orienting per point levers any turn
 * in the flow direction into a large phase shift and tears the pattern into
 * cliffs; per cell the phase is exactly linear in position.
 *
 * Every point is evaluated from its own inputs alone: no grid, no iteration, no
 * neighbours. That is what lets terrain be sampled in tiles at any resolution
 * without generating anything around them.
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
	double EvaluateDisplacementMeters(
		const FMassifErosionInput& Input,
		const FMassifErosionSettings& Settings,
		FMassifMacroSlopeSampler SampleMacroSlope);
}
