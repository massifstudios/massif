#include "MassifSimClock.h"

double MassifGameSpeedMultiplier(EMassifGameSpeed Speed)
{
	switch (Speed)
	{
		case EMassifGameSpeed::Paused:
			return 0.0;

		case EMassifGameSpeed::Normal:
			return 1.0;

		case EMassifGameSpeed::Fast:
			return 4.0;
	}

	return 1.0;
}

FMassifWallClock MassifWallClockAtTick(int64 TickCount)
{
	// Ticks per day is exact — a day is 1440 minutes and a tick is an eighth of
	// one — so the day boundary is found in integers and only the position inside
	// the day is a fraction. Deriving both from a running double would drift a
	// little further off midnight every day.
	constexpr int64 TicksPerDay =
		static_cast<int64>(MassifGameMinutesPerDay / MassifGameMinutesPerSimTick);

	const int64 TicksIntoDay = ((TickCount % TicksPerDay) + TicksPerDay) % TicksPerDay;

	FMassifWallClock Clock;
	Clock.Day = FMath::DivideAndRoundDown(TickCount, TicksPerDay);
	Clock.DayFraction = double(TicksIntoDay) / double(TicksPerDay);
	return Clock;
}

int32 FMassifSimClock::ConsumeTicks(double DeltaSeconds)
{
	AccumulatorSeconds += DeltaSeconds * MassifGameSpeedMultiplier(Speed);

	const int32 OwedTicks = FMath::FloorToInt32(AccumulatorSeconds / MassifSimTickSeconds);
	if (OwedTicks <= 0)
	{
		return 0;
	}

	if (OwedTicks > MassifMaxSimTicksPerFrame)
	{
		// Drop the real time that could not be simulated rather than carrying a debt
		// no later frame is able to pay off.
		AccumulatorSeconds = 0.0;
		return MassifMaxSimTicksPerFrame;
	}

	AccumulatorSeconds -= OwedTicks * MassifSimTickSeconds;
	return OwedTicks;
}
