#include "MassifSimState.h"

#include "MassifSeed.h"
#include "MassifStateHash.h"

void MassifResetSimState(FMassifSimState& State, int32 MasterSeed)
{
	State = FMassifSimState();
	State.MasterSeed = MasterSeed;
	State.RandomStream =
		FMassifSeed(static_cast<uint32>(MasterSeed)).MakeChannelStream(MASSIF_SIM_CORE_SEED_CHANNEL);
}

namespace
{
	/** Winds the offset so the clock reads the given minute of the day, without moving the tick count. */
	void MassifSetWallClockMinute(FMassifSimState& State, int32 MinuteOfDay)
	{
		const int64 TicksPerDay = MassifTicksPerDay();
		const int64 TicksPerMinute = TicksPerDay / int64(MassifGameMinutesPerDay);

		const int64 Wrapped = ((int64(MinuteOfDay) % 1440) + 1440) % 1440;
		const int64 TargetTicksIntoDay = Wrapped * TicksPerMinute;
		const int64 NowTicksIntoDay =
			(((State.TickCount + State.ClockOffsetTicks) % TicksPerDay) + TicksPerDay) % TicksPerDay;

		State.ClockOffsetTicks += TargetTicksIntoDay - NowTicksIntoDay;
	}
}

void MassifApplySimCommand(FMassifSimState& State, const FMassifSimCommand& Command)
{
	switch (Command.Type)
	{
		case EMassifSimCommandType::AddToCounter:
			State.CommandCounter += Command.Argument;
			return;

		case EMassifSimCommandType::SetTimeOfDayMinutes:
			MassifSetWallClockMinute(State, Command.Argument);
			return;

		case EMassifSimCommandType::None:
			return;
	}
}

FMassifWallClock MassifWallClockOf(const FMassifSimState& State)
{
	return MassifWallClockAtTick(State.TickCount + State.ClockOffsetTicks);
}

void MassifAdvanceSimTick(FMassifSimState& State, TConstArrayView<FMassifSimCommand> Commands)
{
	for (const FMassifSimCommand& Command : Commands)
	{
		MassifApplySimCommand(State, Command);
	}

	++State.TickCount;
}

uint64 MassifComputeSimStateHash(const FMassifSimState& State)
{
	FMassifStateHash Hash;
	Hash.AppendInt(State.TickCount);
	Hash.AppendInt(State.MasterSeed);
	Hash.AppendInt(State.CommandCounter);
	Hash.AppendInt(State.ClockOffsetTicks);

	// Both seeds: the initial one says which sequence the state is on, and the
	// current one says how far along it is. A stream that had been drawn from a
	// different number of times is not the same state.
	Hash.AppendInt(State.RandomStream.GetInitialSeed());
	Hash.AppendInt(State.RandomStream.GetCurrentSeed());
	return Hash.Finalize();
}
