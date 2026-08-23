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

	/**
	 * Density of the lying pack in kilograms per cubic metre, half-monthly.
	 *
	 * What decides whether ground is groomable rather than how much of it there is:
	 * the same metre of depth is a fresh fall at a hundred and a spring pack at four
	 * hundred, and only one of the two carries a machine.
	 */
	MASSIFWORLDGEN_API const FName& SnowDensity();

	/**
	 * What the ground reflects, half-monthly: bare where nothing lies, near white
	 * under a fresh fall, and greyer as the pack settles.
	 *
	 * The temperature reads this back, which is what makes snow cool the ground it
	 * lies on and keeps a cold slope cold once it has gone white.
	 */
	MASSIFWORLDGEN_API const FName& SnowAlbedo();

	/** How readily ground lets a slab go: steep enough to slide, loaded enough to matter. */
	MASSIFWORLDGEN_API const FName& AvalancheRelease();

	/** Ground the release areas above it run over, which is what clears a stripe through a forest. */
	MASSIFWORLDGEN_API const FName& AvalanchePath();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Snowpack();
}

MASSIFWORLDGEN_API void MassifRegisterSnowProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifSnowParams& Params);
