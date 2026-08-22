#pragma once

#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifHeightmap.h"

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
