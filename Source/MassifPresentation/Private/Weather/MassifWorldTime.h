#pragma once

#include "CoreMinimal.h"
#include "MassifSimClock.h"
#include "MassifSunGeometry.h"

class UWorld;

/**
 * The one place presentation asks what time it is.
 *
 * Every system that depends on the hour — the fog, the sun and the sky, and
 * whatever reads the clock next — has to get the same answer within a frame, and
 * they do so by construction when they all read the simulation's wall clock
 * rather than each keeping an override of its own.
 */
namespace MassifWorldTime
{
	/**
	 * Days into the year the world is played on.
	 *
	 * Midwinter, because Phase 1 is a winter game. A fixed date until a calendar
	 * exists to advance one, at which point this comes off the wall clock's day.
	 */
	constexpr double DayOfYear = 20.0;

	/** Where the simulation says the clock stands. Midnight of day zero where it is not running. */
	FMassifWallClock GetWallClock(const UWorld* World);

	/** 0 at midnight, rising to 1. Zero where the simulation is not running. */
	double GetDayFraction(const UWorld* World);

	FMassifSunPosition GetSunPosition(const UWorld* World, double LatitudeDegrees);
}
