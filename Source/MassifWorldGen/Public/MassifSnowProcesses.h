#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifSnowParams.h"

/** Channels the snowpack solve produces. */
namespace MassifEnvironmentChannels
{
	/** Snow lying on the ground in metres, half-monthly through a representative year. */
	MASSIFWORLDGEN_API const FName& SnowDepth();

	/** Deepest that pack ever gets over the year, in metres. */
	MASSIFWORLDGEN_API const FName& PeakSnowDepth();

	/** Share of the year with snow lying: 0 bare throughout, 1 never bare. */
	MASSIFWORLDGEN_API const FName& SnowCoverDuration();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Snowpack();
}

MASSIFWORLDGEN_API void MassifRegisterSnowProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifSnowParams& Params);
