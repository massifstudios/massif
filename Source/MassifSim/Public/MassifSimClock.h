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

/** Simulated seconds one tick covers. Fixed, and not a tuning knob. */
constexpr double MassifSimTickSeconds = 0.1;

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
