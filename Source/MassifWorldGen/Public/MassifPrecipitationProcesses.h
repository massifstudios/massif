#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifPrecipitationParams.h"

/** Channels the precipitation solve produces. See MassifEnvironmentGraph.h for the water ones. */
namespace MassifEnvironmentChannels
{
	/** Millimetres falling on a point over a year, however it arrives. */
	MASSIFWORLDGEN_API const FName& AnnualPrecipitation();

	/**
	 * Share of that arriving as snow rather than rain, 0 to 1.
	 *
	 * The split is the difference between water that runs off the day it lands and
	 * water that is stored until spring, which is most of what a ski season is.
	 */
	MASSIFWORLDGEN_API const FName& SnowFraction();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Precipitation();
}

MASSIFWORLDGEN_API void MassifRegisterPrecipitationProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifPrecipitationParams& Params);
