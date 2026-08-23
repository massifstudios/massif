#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentChannel.h"
#include "MassifEnvironmentProcess.h"

struct FMassifRiverReach;

/**
 * Where the water stands at one station through a representative year.
 *
 * Half-monthly, on the same slicing the climate and the snowpack are solved on,
 * so a day of the year means one thing to all of them. The bed is not in here
 * and does not belong in here: a channel's shape is the work of every year it
 * has seen, and does not change because it is April. What changes is how much
 * of that shape has water in it.
 *
 * Solved once at world gen, because the alternative is re-solving a world's
 * worth of channel every time a slider moves.
 *
 * Altitudes are doubles and the rest are floats: a surface a kilometre above
 * the sea resolves to a tenth of a millimetre in a float, and a run of stations
 * each landing on a different side of that reads as a rippling sheet.
 */
struct FMassifStationSeason
{
	double WaterSurfaceMeters[MassifSeasonalSliceCount] = {};
	float DepthMeters[MassifSeasonalSliceCount] = {};
	float HalfWidthMeters[MassifSeasonalSliceCount] = {};
	float SpeedMetersPerSecond[MassifSeasonalSliceCount] = {};
};

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

/**
 * Solves the same surface again for each half-month of a representative year,
 * and leaves the annual answer standing.
 *
 * A season is a discharge, and nothing else here changes: the bed a station has
 * was cut by every year it has seen and is the same in February as in June. What
 * the year moves is how much water that bed is carrying, and everything visible
 * follows from the section it fills — a river runs deep and bank to bank on the
 * snowmelt and withdraws into its own thalweg when the pack has gone.
 *
 * SliceDischargeScale gives a station's share of its annual mean in one slice.
 * One means the annual mean, so a world with no weather solved for it returns
 * one everywhere and comes out with a flat year rather than with no year.
 *
 * Deterministic and thread-safe over reaches, by the same fixed counts the
 * annual solve uses.
 */
MASSIFWORLDGEN_API void MassifSolveSeasonalFlow(
	const FMassifChannelFlowParams& Params,
	TFunctionRef<double(FVector2d PositionMeters, int32 TimeSlice)> SliceDischargeScale,
	TArray<FMassifRiverReach>& InOutReaches);
