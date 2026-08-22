#pragma once

#include "CoreMinimal.h"
#include "MassifClimateParams.h"
#include "MassifEnvironmentBakeParams.h"
#include "MassifPrecipitationParams.h"
#include "MassifSnowParams.h"
#include "MassifSunParams.h"
#include "MassifVegetationParams.h"
#include "MassifWindParams.h"

/**
 * Every system's own parameters, gathered for the call that registers them all.
 *
 * One block per process rather than one block for the graph: a process owns and
 * versions what tunes it, and adding a system adds a member here instead of
 * widening a struct everything else already reads (ARCHITECTURE §5.6.3).
 */
struct FMassifEnvironmentParams
{
	FMassifEnvironmentBakeParams Water;
	FMassifSunParams Sun;
	FMassifClimateParams Climate;
	FMassifWindParams Wind;
	FMassifPrecipitationParams Precipitation;
	FMassifSnowParams Snow;
	FMassifVegetationParams Vegetation;
};
