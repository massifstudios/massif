#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifLithologyParams.h"

/** Channels the rock under the world produces. */
namespace MassifEnvironmentChannels
{
	/** How well the rock resists being taken apart: 0 crumbles away, 1 stands as cliff. */
	MASSIFWORLDGEN_API const FName& RockHardness();

	/** How readily it weathers into soil at all: 0 a thin rendzina, 1 a deep loam. */
	MASSIFWORLDGEN_API const FName& SoilYield();

	/** Loose debris shed by frost shattering, in metres per thousand years. */
	MASSIFWORLDGEN_API const FName& ScreeProduction();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Lithology();
}

/**
 * Compass bearing the bands run along, drawn from the master seed.
 *
 * The one thing about the pattern that cannot be read back off a channel, and
 * the property that separates bedding from noise.
 */
MASSIFWORLDGEN_API double MassifLithologyStrikeBearingDegrees(uint32 MasterSeed);

MASSIFWORLDGEN_API void MassifRegisterLithologyProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifLithologyParams& Params);
