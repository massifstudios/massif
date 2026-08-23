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

	/**
	 * How strongly cold air drains through a point on a still night, 0 to 1.
	 *
	 * Nothing to do with the prevailing wind: this is the flow the ground itself
	 * drives once it has radiated its heat away. Near zero on a ridge nothing
	 * drains into, high on a floor that collects a large slope area and is flat
	 * enough to let what arrives sit. Where the frost hollows are.
	 */
	MASSIFWORLDGEN_API const FName& KatabaticStrength();

	/**
	 * Degrees a descending lee flow adds, in Celsius. Zero on windward ground.
	 *
	 * Air forced over a range leaves its water on the windward side and comes down
	 * the far side dry, warming at the dry adiabatic rate over the whole descent —
	 * far more than it cooled climbing. It is why a lee valley runs warm and why
	 * its snow goes first.
	 */
	MASSIFWORLDGEN_API const FName& FoehnWarming();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Wind();
}

MASSIFWORLDGEN_API void MassifRegisterWindProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifWindParams& Params);
