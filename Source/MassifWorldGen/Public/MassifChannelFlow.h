#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentProcess.h"

struct FMassifRiverReach;

/** How the water is put back on top of the bed the carve left. */
struct FMassifChannelFlowParams
{
	/**
	 * `Off` keeps the surface the elevation model measured, which along a channel
	 * is the top of the water and so is already an answer. `Cheap` puts each
	 * station at its own normal depth, so the surface follows the section rather
	 * than the ground. `Standard` adds the backwater: a pool fills upstream of a
	 * constriction instead of the surface stepping down into it.
	 */
	EMassifFidelity Fidelity = EMassifFidelity::Standard;

	/** Bisection steps for the level at one station. A count, not a tolerance (ARCHITECTURE §4). */
	int32 SurfaceIterations = 28;

	/**
	 * Passes of the whole reach.
	 *
	 * The slope a station's depth is solved against is a property of the surface,
	 * and the surface is what is being solved: a second pass measures the slope over
	 * the answer the first one gave instead of over the ground.
	 */
	int32 PassCount = 2;

	/** Stations either side the energy slope is measured over, matching the normal-depth solve. */
	int32 SlopeWindowNodes = 4;

	/** Floor on the energy slope, so a reach left exactly level still carries its water. */
	double MinEnergySlope = 1.0e-3;

	/** Shallowest water drawn, matching the normal-depth solve's own floor. */
	double MinDepthMeters = 0.30;
};

/**
 * Solves where the water's surface sits over every station of every reach.
 *
 * The surface stops being the ground's altitude handed down as a running minimum
 * and becomes what the section under it can carry: it falls steeply through a
 * narrow reach, flattens into a pool where the channel opens out, and rises above
 * normal depth where what is downstream cannot take the water as fast as it
 * arrives. None of that is available to a solve that only knows the discharge.
 *
 * The surface never rises going downstream, and never goes over the lower bank of
 * its own section — past that the water is on a floodplain, and a floodplain is
 * not what a channel's cross-section can answer for.
 *
 * Each reach's outlet is put at its own normal depth rather than at the surface of
 * the reach it flows into: reaches are not linked to one another, and a
 * tributary's last station and its trunk's are the same point solved twice.
 *
 * Deterministic and thread-safe over reaches, by fixed pass and step counts.
 */
MASSIFWORLDGEN_API void MassifSolveChannelFlow(
	const FMassifChannelFlowParams& Params, TArray<FMassifRiverReach>& InOutReaches);
