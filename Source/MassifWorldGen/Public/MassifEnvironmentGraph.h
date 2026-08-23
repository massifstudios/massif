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
