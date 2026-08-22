#pragma once

#include "CoreMinimal.h"

/**
 * What the wind solve is tuned by.
 *
 * The world is under one synoptic wind — a single bearing and a single speed —
 * and the terrain rewrites it locally. Everything here is a count, a reach or a
 * fixed strength, never a convergence test: the fetch is swept at a fixed number
 * of steps and the bearing drawn from the seeded stream, so the same world blows
 * the same way on every machine (ARCHITECTURE §4).
 */
struct FMassifWindParams
{
	/**
	 * Share of the wind a fully closed canopy takes out of the flow.
	 *
	 * Canopy slows the wind that redistributes the snow that decides where canopy
	 * grows, which is why snow under trees lies deep and undrifted. Reading zero -
	 * what an unwritten vegetation channel gives on the loop's first round - leaves
	 * the flow exactly as the terrain shaped it.
	 */
	double CanopyDragFraction = 0.55;

	/**
	 * Cells the wind channels are solved at. Coarse like the rest of the climate:
	 * the flow is turned by whole flanks and crests rather than by anything a few
	 * metres across, and the exposure sweep costs cells times steps.
	 */
	double CellSizeMeters = 64.0;

	/** Speed of the prevailing wind over open level ground. Every reading is measured off this. */
	double SynopticSpeedMetersPerSecond = 8.0;

	/**
	 * Compass bearing the prevailing wind blows from, the way a wind is always
	 * named: 0 is a northerly, 90 an easterly. Unset draws it from the seed, so
	 * each world gets its own prevailing wind; pinning it is what lets a test turn
	 * the wind against the terrain.
	 */
	TOptional<double> SynopticBearingDegrees;

	/**
	 * How far upwind the exposure sweep looks for something that blocks the flow.
	 * A crest shelters ground well past the foot of its own flank, and beyond a
	 * kilometre the shelter has mixed out again.
	 */
	double ExposureReachMeters = 1000.0;

	/** Samples along the upwind fetch, spread evenly over the reach above. */
	int32 ExposureStepCount = 24;

	/**
	 * Exposure lost per radian of upwind obstruction. At 1.5 a flank behind a
	 * 20-degree skyline keeps about half the wind of open ground.
	 */
	double ExposureSensitivity = 1.5;

	/**
	 * The most of the synoptic direction a valley axis may take over. Below 1, so
	 * the axis pulls the flow round without ever fully replacing what drives it.
	 */
	double ChannelingStrength = 0.7;

	/**
	 * Slope at which terrain confines the flow half as much as a vertical wall
	 * would. Confinement is read off the gradient, so an open plateau channels
	 * nothing and a deep trench channels nearly fully.
	 */
	double ChannelingReferenceSlope = 0.35;

	/**
	 * Downwind fall at which the flow has separated from the ground completely.
	 * A quarter is about 14 degrees; gentler ground than that keeps the flow
	 * attached and has no lee pocket behind it.
	 */
	double LeeSeparationSlope = 0.25;

	/**
	 * How much of the along-wind flow runs backwards where separation is complete.
	 * This is the recirculation that packs a lee slope with drifted snow rather
	 * than merely sheltering it.
	 */
	double LeeReversalFraction = 0.3;
};
