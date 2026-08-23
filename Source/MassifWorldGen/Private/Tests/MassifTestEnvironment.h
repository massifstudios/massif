#pragma once

#include "MassifElevationSource.h"
#include "MassifEnvironmentBakeGrid.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifHeightmap.h"

/**
 * Discharge from an even depth of runoff over the whole world, which is what the
 * graph's own discharge process produces with no precipitation solve to read.
 *
 * For tests whose subject is the channel the water cut rather than the weather
 * that filled it: an even year is reproducible in one line here, and matching the
 * graph's solved discharge would mean re-solving the weather beside it.
 */
inline void MassifRouteEvenRunoff(
	const FMassifBakeGrid& Grid,
	const FMassifEnvironmentBakeParams& Params,
	TArrayView<const double> Filled,
	TArray<float>& OutDischargeCubicMetersPerSecond)
{
	const double CellCubicMetersPerSecond = 0.001 * Params.FallbackRunoffMillimetersPerYear
		* Grid.CellAreaSquareMeters() / MassifSecondsPerYear;

	TArray<double> Inflow;
	Inflow.Init(CellCubicMetersPerSecond, Grid.CellCount());
	MassifBakeCpu::RouteInflow(Grid, Params, Filled, Inflow, OutDischargeCubicMetersPerSecond);
}

/**
 * Solves the environment over a heightmap, for tests that want the finished
 * channels rather than the individual stages.
 *
 * Asserts rather than returning an error: a refused schedule is a broken graph,
 * not a case a test should carry on past.
 */
inline TUniquePtr<FMassifEnvironmentScheduler> MassifSolveTestEnvironment(
	TSharedRef<const FMassifHeightmap> Heightmap, uint32 MasterSeed, const FMassifEnvironmentBakeParams& Params)
{
	FString Error;
	TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifBuildEnvironment(
		MakeShared<FMassifHeightmapElevationSource>(MoveTemp(Heightmap)), FMassifGeoreference(), MasterSeed,
		Params, Error);
	checkf(Scheduler.IsValid(), TEXT("%s"), *Error);
	return Scheduler;
}

/** The solved channels alone, for tests that hand them to something that outlives the call. */
inline TSharedRef<const FMassifEnvironmentState> MassifSolveTestEnvironmentState(
	TSharedRef<const FMassifHeightmap> Heightmap, uint32 MasterSeed, const FMassifEnvironmentBakeParams& Params)
{
	return MassifSolveTestEnvironment(MoveTemp(Heightmap), MasterSeed, Params)->GetSharedState();
}
