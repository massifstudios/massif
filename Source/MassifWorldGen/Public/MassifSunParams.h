#pragma once

#include "CoreMinimal.h"

/**
 * Bearings the horizon is *stored* at, evenly spaced from north.
 *
 * Fixed, and deliberately not the same number as the sweep: `Reference` sweeps
 * twice as finely, and a channel's slice count is settled when the process
 * declares itself, before any fidelity is known. The stored bearings are a
 * subset of the swept ones at every supported fidelity, so refining the sweep
 * sharpens each stored angle without moving where they sit.
 */
constexpr int32 MassifHorizonAzimuthCount = 16;

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

	/** Steps the sun path is integrated at, between sunrise and sunset. */
	int32 SunPathStepCount = 24;

	/**
	 * First and last half-monthly slice of the growing season, inclusive.
	 *
	 * Two things hang off this. It is the span the annual channel averages, which
	 * is what a treeline reads: what decides whether a tree can stand somewhere is
	 * the season it has to grow in, not the average of that season and the winter
	 * it sleeps through. It is also the span every slice is measured against, so
	 * that one is a growing-season day of level open ground and the winter slices
	 * come out as the fractions of it they are.
	 *
	 * Slice 6 is the first half of April and slice 17 the second half of September.
	 */
	int32 GrowingSeasonFirstSlice = 6;
	int32 GrowingSeasonLastSlice = 17;
};
