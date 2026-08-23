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

void MassifApplySimCommand(FMassifSimState& State, const FMassifSimCommand& Command)
{
	switch (Command.Type)
	{
		case EMassifSimCommandType::AddToCounter:
			State.CommandCounter += Command.Argument;
			return;

		case EMassifSimCommandType::None:
			return;
	}
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

	// Both seeds: the initial one says which sequence the state is on, and the
	// current one says how far along it is. A stream that had been drawn from a
	// different number of times is not the same state.
	Hash.AppendInt(State.RandomStream.GetInitialSeed());
	Hash.AppendInt(State.RandomStream.GetCurrentSeed());
	return Hash.Finalize();
}
