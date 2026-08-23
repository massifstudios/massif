#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifPrecipitationParams.h"

/** Channels the precipitation solve produces. See MassifEnvironmentGraph.h for the water ones. */
namespace MassifEnvironmentChannels
{
	/** Millimetres falling on a point over a year, however it arrives. */
	MASSIFWORLDGEN_API const FName& AnnualPrecipitation();

	/**
	 * Millimetres falling in one half-month, over the 24 slices of a
	 * representative year.
	 *
	 * A mountain year is not flat: the annual total says how much water a slope
	 * gets and this says when it arrives, which is what decides whether it is
	 * lying on the ground in February or ran off in August.
	 */
	MASSIFWORLDGEN_API const FName& SeasonalPrecipitation();

	/**
	 * Share of one half-month's fall arriving as snow rather than rain, 0 to 1,
	 * over the 24 slices of a representative year.
	 *
	 * The split is the difference between water that runs off the day it lands and
	 * water that is stored until spring, which is most of what a ski season is.
	 * Half-monthly because it is a property of the day it fell on: the same slope
	 * takes rain in July and snow in January, and no annual figure can say that.
	 */
	MASSIFWORLDGEN_API const FName& SnowFraction();

	/**
	 * Millimetres a year the air takes back off the ground and out of what grows
	 * on it.
	 *
	 * The other end of the water balance. Everything else in the graph moves water
	 * around the massif; this is the only term that removes it, and it is what
	 * decides whether a warm slope with rain on it is wet ground or dry ground.
	 */
	MASSIFWORLDGEN_API const FName& Evapotranspiration();
}

namespace MassifEnvironmentProcesses
{
	MASSIFWORLDGEN_API const FName& Precipitation();
}

MASSIFWORLDGEN_API void MassifRegisterPrecipitationProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifPrecipitationParams& Params);
