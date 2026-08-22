#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifWindParams.h"

/** Channels the wind solve produces. See MassifEnvironmentGraph.h for the water ones. */
namespace MassifEnvironmentChannels
{
	/**
	 * Wind over a point against open level ground: 1 is neutral, above 1 a crest
	 * or a windward shoulder, below 1 a sheltered lee pocket or a valley floor.
	 */
	MASSIFWORLDGEN_API const FName& WindExposure();

	/**
	 * The wind itself, metres per second east and north. Speed is the vector's
	 * length, so one reading carries both where the air goes and how hard.
	 */
	MASSIFWORLDGEN_API const FName& WindField();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Wind();
}

MASSIFWORLDGEN_API void MassifRegisterWindProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifWindParams& Params);
