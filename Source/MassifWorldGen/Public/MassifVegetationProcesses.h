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

	/** Share of the rainfall the canopy catches and returns to the air before it reaches the ground. */
	MASSIFWORLDGEN_API const FName& Interception();

	/** What is growing, as an EMassifVegetationClass index. Never interpolated. */
	MASSIFWORLDGEN_API const FName& VegetationClass();

	/** Share of what could grow there that has been knocked down, 0 intact to 1 cleared. */
	MASSIFWORLDGEN_API const FName& Disturbance();
}

/** What is growing at a point, coarsely enough that a scatter can key off it. */
enum class EMassifVegetationClass : uint8
{
	Bare,            // nothing holds
	Snowbed,         // cleared by snow lying too long
	AlpineMeadow,    // grass and herb above the treeline
	DwarfShrub,      // heath and krummholz in the wind
	ConiferForest,   // the main forest belt
	BroadleafForest, // warm damp ground low down
	Wetland,         // waterlogged
	Count,
};

MASSIFWORLDGEN_API const TCHAR* MassifVegetationClassName(EMassifVegetationClass Class);

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Vegetation();
}

MASSIFWORLDGEN_API void MassifRegisterVegetationProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifVegetationParams& Params);
