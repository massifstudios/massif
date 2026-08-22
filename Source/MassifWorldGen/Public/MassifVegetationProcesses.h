#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifVegetationParams.h"

/** Channels the vegetation solve produces. */
namespace MassifEnvironmentChannels
{
	/** Share of the ground carrying anything living at all, 0 bare to 1 closed. */
	MASSIFWORLDGEN_API const FName& VegetationCover();

	/** Share of it that is closed tree canopy, which is what shades and slows the wind. */
	MASSIFWORLDGEN_API const FName& VegetationCanopy();

	/** Height of that canopy in metres. */
	MASSIFWORLDGEN_API const FName& VegetationHeight();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Vegetation();
}

MASSIFWORLDGEN_API void MassifRegisterVegetationProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifVegetationParams& Params);
