#pragma once

#include "CoreMinimal.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentParams.h"
#include "MassifEnvironmentScheduler.h"

class FMassifRiverNetwork;

/**
 * Names of the channels the standard environment graph produces.
 *
 * Functions rather than constants: an FName built during static initialisation
 * runs before the name table exists.
 */
namespace MassifEnvironmentChannels
{
	/** The elevation source sampled onto the graph's own grid. */
	MASSIFWORLDGEN_API const FName& Altitude();

	/** Every hollow raised to the level it overflows at, so the surface descends without interruption. */
	MASSIFWORLDGEN_API const FName& FilledAltitude();

	MASSIFWORLDGEN_API const FName& StandingWaterDepth();
	MASSIFWORLDGEN_API const FName& FlowAccumulation();
	MASSIFWORLDGEN_API const FName& Wetness();
	MASSIFWORLDGEN_API const FName& SoilDepth();

	/** Share of the water arriving at a point that soaks in rather than running off, 0 to 1. */
	MASSIFWORLDGEN_API const FName& Infiltration();

	/**
	 * Catchment area's worth of water coming back to the surface, in square metres.
	 *
	 * What soaked in upslope re-emerges where the rock under it stops taking water,
	 * which is why a stream can start mid-slope with no catchment above it.
	 */
	MASSIFWORLDGEN_API const FName& SpringDischarge();

	/**
	 * Catchment area's worth of water moving through a point in one half-month.
	 *
	 * The catchment is a fixed figure; what runs through it is not. An alpine
	 * valley in May carries nothing like what it carries in February.
	 */
	MASSIFWORLDGEN_API const FName& SeasonalFlow();

	/**
	 * Water running through a point, in cubic metres per second, averaged over a year.
	 *
	 * The routed catchment says how much ground drains through a point; this says
	 * how much water that ground yields, which is the same figure only where the
	 * rain is even. Two catchments of one size on the wet and dry flanks of the
	 * same range carry rivers of different sizes, and this is the channel that
	 * knows it.
	 */
	MASSIFWORLDGEN_API const FName& Discharge();

	/** What the droplets left, before roots hold any of it in place. */
	MASSIFWORLDGEN_API const FName& SoilDeposition();
}

/** Names of the processes that produce them, for fidelity switching and the graph inspector. */
namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Altitude();
	MASSIFWORLDGEN_API const FName& DepressionFill();
	MASSIFWORLDGEN_API const FName& StandingWater();
	MASSIFWORLDGEN_API const FName& FlowRouting();
	MASSIFWORLDGEN_API const FName& SoilDepth();
	MASSIFWORLDGEN_API const FName& SoilDeposition();
	MASSIFWORLDGEN_API const FName& Wetness();
	MASSIFWORLDGEN_API const FName& Infiltration();
	MASSIFWORLDGEN_API const FName& Springs();
	MASSIFWORLDGEN_API const FName& SeasonalFlow();
	MASSIFWORLDGEN_API const FName& Discharge();
}

/** Adds every standard process to a scheduler. The scheduler orders them; this does not. */
MASSIFWORLDGEN_API void MassifRegisterEnvironmentProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifEnvironmentParams& Params);

/** The same with every system but the water left at its defaults. */
MASSIFWORLDGEN_API void MassifRegisterEnvironmentProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifEnvironmentBakeParams& WaterParams);

/**
 * Adds only the process that samples the elevation source onto the graph's grid.
 * Everything else reads that channel, so a graph exercising one system on its own
 * still needs it.
 */
MASSIFWORLDGEN_API void MassifRegisterAltitudeProcess(
	FMassifEnvironmentScheduler& Scheduler, const FMassifEnvironmentBakeParams& Params);

/**
 * Adds what becomes of the routed water: the share that soaks in, the springs it
 * returns through, and what moves through a point in a given half-month.
 *
 * Separable from the drainage itself because every input beyond the routed
 * catchment is optional. A graph with no soil, no rock and no weather in it
 * still solves these, which is what makes them testable without the rest.
 */
MASSIFWORLDGEN_API void MassifRegisterWaterBalanceProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifEnvironmentBakeParams& Params);

/**
 * Registers the standard processes and orders them, without solving.
 *
 * For a caller that has fidelities to set first: a level set after the solve
 * dirties the graph and runs the whole thing a second time. Null if the schedule
 * was refused.
 */
MASSIFWORLDGEN_API TUniquePtr<FMassifEnvironmentScheduler> MassifPrepareEnvironment(
	TSharedRef<const IMassifElevationSource> Elevation,
	const FMassifGeoreference& Georeference,
	uint32 MasterSeed,
	const FMassifEnvironmentBakeParams& Params,
	FString& OutError);

/** Registers the standard processes, schedules them and runs them. Null if the schedule was refused. */
MASSIFWORLDGEN_API TUniquePtr<FMassifEnvironmentScheduler> MassifBuildEnvironment(
	TSharedRef<const IMassifElevationSource> Elevation,
	const FMassifGeoreference& Georeference,
	uint32 MasterSeed,
	const FMassifEnvironmentBakeParams& Params,
	FString& OutError);

/**
 * Traces the channels the routed water cut, as polylines.
 *
 * Not a process: a process writes channels, and a river network is a set of
 * polylines. It reads the solved channels afterwards, which it can only do
 * because the altitude channels hold doubles — the drop the depression fill
 * adds to keep a flat descending is far below what a float or a quantised
 * channel would keep, and a river traced against a coarser surface would lose
 * its way across every lake bed in the world.
 */
MASSIFWORLDGEN_API TSharedRef<const FMassifRiverNetwork> MassifExtractRivers(
	const FMassifEnvironmentState& State, const FMassifRiverParams& Params);
