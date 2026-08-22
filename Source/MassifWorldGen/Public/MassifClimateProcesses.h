#pragma once

#include "CoreMinimal.h"
#include "MassifClimateParams.h"
#include "MassifEnvironmentScheduler.h"

/** Channels the temperature solve produces. */
namespace MassifEnvironmentChannels
{
	/** Mean growing-season air temperature at the ground, in degrees Celsius. */
	MASSIFWORLDGEN_API const FName& MeanTemperature();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Temperature();
}

MASSIFWORLDGEN_API void MassifRegisterClimateProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifClimateParams& Params);
