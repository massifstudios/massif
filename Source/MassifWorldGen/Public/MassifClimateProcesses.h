#pragma once

#include "CoreMinimal.h"
#include "MassifClimateParams.h"
#include "MassifEnvironmentScheduler.h"

/** Channels the temperature solve produces. */
namespace MassifEnvironmentChannels
{
	/** Mean growing-season air temperature at the ground, in degrees Celsius. */
	MASSIFWORLDGEN_API const FName& MeanTemperature();

	/** The same carried round a representative year, half-monthly, in degrees Celsius. */
	MASSIFWORLDGEN_API const FName& SeasonalTemperature();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Temperature();
	MASSIFWORLDGEN_API const FName& SeasonalClimate();
}

MASSIFWORLDGEN_API void MassifRegisterClimateProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifClimateParams& Params);
