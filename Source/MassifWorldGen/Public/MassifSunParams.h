#pragma once

#include "CoreMinimal.h"

/**
 * What the insolation solve is tuned by.
 *
 * Everything here is a count or a fixed sample, never a convergence test: the
 * sun's path is integrated at a fixed number of steps and the horizon swept at a
 * fixed number of azimuths, so the same world lights the same way on every
 * machine (ARCHITECTURE §4).
 */
struct FMassifSunParams
{
	/**
	 * Cells the insolation channels are solved at. Coarser than hydrology: a
	 * horizon is cast by ridges kilometres away and does not turn over within a
	 * few metres, and the sweep costs cells times azimuths times steps.
	 */
	double CellSizeMeters = 32.0;

	/** Compass directions the horizon is swept in. */
	int32 HorizonAzimuthCount = 16;

	/** Samples along each sweep, spread out to the reach below. */
	int32 HorizonStepCount = 48;

	/**
	 * How far a ridge is allowed to shade from. Beyond a few kilometres the earth's
	 * curvature and haze matter more than the terrain does, and the sweep cost is
	 * linear in this.
	 */
	double HorizonReachMeters = 4000.0;

	/**
	 * Day of the year the representative sun path is taken on. Day 196 is mid
	 * July: the middle of the growing season, which is what decides where a tree
	 * can stand rather than what the ski season looks like.
	 */
	int32 RepresentativeDayOfYear = 196;

	/** Steps the sun path is integrated at, between sunrise and sunset. */
	int32 SunPathStepCount = 24;
};
