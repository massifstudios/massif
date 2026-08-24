#pragma once

#include "CoreMinimal.h"

/**
 * What comes off the cliffs, how far it runs, and what is left lying where it
 * stopped.
 *
 * Two outputs come out of one pass of falling blocks: the blocks themselves,
 * placed where each came to rest, and the density they left behind. The density
 * is what the rest of the graph and the debug views read; the blocks are what
 * gets drawn. Both are the same particles, so a scree cone cannot show a
 * different shape to the eye than it shows to the systems reading it.
 */
struct FMassifRockfallParams
{
	/** Cells the deposit density is reduced onto. Matches the hydrology grid. */
	double CellSizeMeters = 8.0;

	/** Side of one storage bucket of placed blocks, matching the resident tiles (§5.4). */
	double TileSizeMeters = 512.0;

	/**
	 * Candidate blocks per square kilometre of the whole world.
	 *
	 * Over the world rather than over the cliffs, so the figure does not move when
	 * the release threshold is retuned. Most candidates are rejected before they
	 * fall: what survives is set by how much of the massif is steep bare rock.
	 */
	double CandidatesPerSquareKilometre = 20000.0;

	/**
	 * Slope a face has to reach before it sheds anything, and the span over which
	 * it goes from shedding nothing to shedding all it can.
	 *
	 * Below the first figure the ground holds its own debris; a real talus slope
	 * stands near 35 degrees, so ground at or under that is where blocks stop
	 * rather than where they start.
	 */
	double MinReleaseSlopeDegrees = 42.0;
	double ReleaseSlopeSpanDegrees = 18.0;

	/** Soil deeper than this smothers a face: what is under it is not shedding blocks. */
	double ReleaseSoilDepthMeters = 0.25;

	/** How much of the release rate the softest rock keeps. Hard rock stands in cliffs and sheds blocks. */
	double SoftRockReleaseFraction = 0.35;

	/**
	 * Block radius band, and the power-law exponent that picks within it.
	 *
	 * Real rockfall block sizes are power-law distributed: many small, few large.
	 * A larger exponent is a finer-grained slope.
	 *
	 * The lower end is where a block stops being worth an object and starts being
	 * gravel the material layer draws. Everything simulated here is placed, so the
	 * block that came to rest and the boulder that gets drawn are the same block.
	 */
	double MinBlockRadiusMeters = 0.5;
	double MaxBlockRadiusMeters = 4.0;
	double BlockSizeExponent = 2.6;

	/** Metres a block advances per step, and the step ceiling that bounds one fall. */
	double StepLengthMeters = 4.0;
	int32 MaxFallSteps = 400;

	/**
	 * How much of its direction a block keeps against the slope under it.
	 *
	 * High, and this is the whole reason a rockfall pass is not a steepest-descent
	 * walk: a block that follows the gradient exactly ends up in the stream bed
	 * with the water, and boulders do not sit in stream beds. Momentum is what
	 * carries one across a bench and out onto the cone.
	 */
	double Inertia = 0.82;

	/** Gravity along the surface, and the speed no block exceeds, in the step's own units. */
	double Gravity = 9.81;
	double MaxSpeedMetersPerSecond = 45.0;

	/**
	 * The angle a slope of loose blocks stands at.
	 *
	 * Friction is its tangent, so a block gains energy on ground steeper than this
	 * and loses it on ground shallower — which is the whole runout model, and the
	 * reason blocks pile into a cone at the foot of a face rather than either
	 * stopping on it or running forever.
	 */
	double ReposeSlopeDegrees = 33.0;

	/** Speed below which a block is at rest. */
	double RestSpeedMetersPerSecond = 0.4;

	/**
	 * How far a block is knocked sideways per step, as a share of the step, at
	 * full speed. What turns a single fall line into a spreading cone.
	 */
	double LateralScatter = 0.32;

	/**
	 * How much further a big block runs than a small one.
	 *
	 * A larger block carries more energy against roughly the same surface
	 * roughness, which is why the toe of a talus cone is coarse and the apex is
	 * fine. Zero would sort the cone not at all.
	 */
	double SizeRunoutBias = 0.45;

	/**
	 * Share of a block's speed a fully closed canopy takes per step.
	 *
	 * Forest below a cliff catches what comes down it, which is why a mature stand
	 * on a rockfall slope has no boulders in its lower half.
	 */
	double CanopyDamping = 0.22;

	/** How deep into its own radius a resting block settles. */
	double BuriedFraction = 0.3;

	/** Candidates handed to one worker at a time. */
	int32 CandidatesPerBatch = 4096;
};
