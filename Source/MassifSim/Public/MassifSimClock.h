#pragma once

#include "CoreMinimal.h"
#include "MassifSimClock.generated.h"

/**
 * How fast the simulation runs against real time.
 *
 * These change how many fixed steps run per real second. None of them changes
 * how long a step is, which is the whole reason the sim is reproducible at any
 * speed and under any frame rate (ARCHITECTURE §3).
 */
UENUM()
enum class EMassifGameSpeed : uint8
{
	Paused,
	Normal,
	Fast,
};

/**
 * Seconds of *motion* one tick covers. Fixed, and not a tuning knob.
 *
 * Guests ski, chairs travel and snow settles against this, at real-world rates,
 * because a skier at ten times speed does not read as a skier.
 */
constexpr double MassifSimTickSeconds = 0.1;

/**
 * Minutes the *wall clock* advances per tick (ARCHITECTURE §6.1).
 *
 * A separate scale from the one above, on purpose. A day that took a real day to
 * pass would put every weather system out of reach of the player watching for
 * it, and a tick fast enough to fix that would make the guests absurd. So the
 * two run apart: this is what melt, settling, fog and fronts are denominated in,
 * and `MassifSimTickSeconds` is what speeds and distances are.
 *
 * An eighth rather than a tenth because the accumulator adds it up over
 * thousands of ticks, and an exact binary fraction cannot drift off a day
 * boundary the way a decimal one does. At 10 Hz it puts a full day in a little
 * over nineteen real minutes at normal speed.
 */
constexpr double MassifGameMinutesPerSimTick = 0.125;

constexpr double MassifGameMinutesPerDay = 24.0 * 60.0;

/**
 * Ticks one frame may run before the clock gives up on the rest.
 *
 * Without a cap, a frame that ran long owes ticks, and running them makes the
 * next frame run longer still. What the cap costs is real time the machine could
 * not simulate; what it does not cost is determinism, because the state after N
 * ticks is unchanged by how those ticks were spread across frames.
 */
constexpr int32 MassifMaxSimTicksPerFrame = 20;

MASSIFSIM_API double MassifGameSpeedMultiplier(EMassifGameSpeed Speed);

/**
 * Ticks in a day, which is exact: a day is 1440 minutes and a tick is an eighth
 * of one. Day boundaries are therefore found in integers rather than by
 * accumulating a double that would creep further off midnight every day.
 */
constexpr int64 MassifTicksPerDay()
{
	return static_cast<int64>(MassifGameMinutesPerDay / MassifGameMinutesPerSimTick);
}

/**
 * Where the wall clock stands, derived from the tick count and nothing else.
 *
 * Not stored: a second copy of the time would be one more thing a save could
 * disagree with itself about, and the tick count already says everything.
 */
struct MASSIFSIM_API FMassifWallClock
{
	/** Whole days since the world began, starting at zero. */
	int64 Day = 0;

	/** Where in the day it is, 0 at midnight and rising to 1. */
	double DayFraction = 0.0;

	int32 GetHour() const
	{
		return FMath::Clamp(FMath::FloorToInt32(DayFraction * 24.0), 0, 23);
	}

	int32 GetMinute() const
	{
		const double Minutes = DayFraction * MassifGameMinutesPerDay;
		return FMath::Clamp(FMath::FloorToInt32(Minutes) % 60, 0, 59);
	}
};

MASSIFSIM_API FMassifWallClock MassifWallClockAtTick(int64 TickCount);

/**
 * Turns frame time into a whole number of sim ticks.
 *
 * The one place real time is allowed to touch the simulation, and it touches it
 * only as a count. Nothing downstream of here sees a frame delta.
 */
struct MASSIFSIM_API FMassifSimClock
{
	EMassifGameSpeed Speed = EMassifGameSpeed::Normal;

	/** Ticks owed for this frame, draining the accumulator by exactly that many. */
	int32 ConsumeTicks(double DeltaSeconds);

	/** Real time carried over, always less than one tick once ticks have been taken. */
	double GetAccumulatorSeconds() const
	{
		return AccumulatorSeconds;
	}

private:
	double AccumulatorSeconds = 0.0;
};
