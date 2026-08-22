#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifSunParams.h"

/** Channels the sunlight solve produces. See MassifEnvironmentGraph.h for the water ones. */
namespace MassifEnvironmentChannels
{
	/** Fraction of the sky hemisphere a point can see, 0 hemmed in to 1 open. */
	MASSIFWORLDGEN_API const FName& SkyViewFactor();

	/** A day's direct beam against what level open ground at the same latitude takes. */
	MASSIFWORLDGEN_API const FName& DirectInsolation();

	/** The same for skylight, which a tilted or hemmed-in surface sees less of. */
	MASSIFWORLDGEN_API const FName& DiffuseInsolation();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Insolation();
}

MASSIFWORLDGEN_API void MassifRegisterSunProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifSunParams& Params);
