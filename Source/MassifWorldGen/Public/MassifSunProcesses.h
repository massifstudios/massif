#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifSunParams.h"

/** Channels the sunlight solve produces. See MassifEnvironmentGraph.h for the water ones. */
namespace MassifEnvironmentChannels
{
	/**
	 * Fraction of the sky hemisphere a point can see, 0 hemmed in to 1 open.
	 *
	 * One slice: the skyline is a property of the terrain and has no date.
	 */
	MASSIFWORLDGEN_API const FName& SkyViewFactor();

	/**
	 * Half-monthly direct beam, against what level open ground at the same latitude
	 * takes on an average growing-season day.
	 *
	 * Sliced because the sun at 47 degrees north stands far lower in January than in
	 * July, and a north face takes almost none of it: which slopes hold their snow
	 * into spring is decided here rather than by altitude.
	 */
	MASSIFWORLDGEN_API const FName& DirectInsolation();

	/** The same for skylight, which a tilted or hemmed-in surface sees less of. */
	MASSIFWORLDGEN_API const FName& DiffuseInsolation();

	/**
	 * The growing-season mean of the direct beam, in one slice.
	 *
	 * What a treeline reads: a tree stands or does not stand by the season it has
	 * to grow in, and asking that question of a single half-month would answer it
	 * with whichever half-month was asked.
	 */
	MASSIFWORLDGEN_API const FName& AnnualInsolation();

	/**
	 * Degrees the skyline stands above level, per compass bearing.
	 *
	 * The sweep behind `SkyViewFactor`, kept instead of averaged away. Sky view
	 * answers how much sky a point has and cannot answer where it is: a cell under
	 * a ridge to its south and open to the north reports the same fraction as one
	 * shaded evenly all round, and only the first loses the winter sun.
	 *
	 * This is what makes time of day answerable. `DirectInsolation` is a whole
	 * day's total for a half-month, so it cannot say whether the sun is up *here*,
	 * now — which is the question behind morning refreeze, afternoon shade on a
	 * north slope, and the spring corn cycle. Comparing the sun's current altitude
	 * against the angle stored at its bearing does say.
	 *
	 * Slices are bearings, not dates: `MassifHorizonAzimuthCount` of them, slice 0
	 * due north and running clockwise. The slice axis is reused because a bearing
	 * indexes exactly the way a date does, and every consumer — sampling, the
	 * scrubber, the transect — already handles it.
	 */
	MASSIFWORLDGEN_API const FName& Horizon();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Insolation();
}

MASSIFWORLDGEN_API void MassifRegisterSunProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifSunParams& Params);
