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
