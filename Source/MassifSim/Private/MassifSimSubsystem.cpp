#include "MassifSimSubsystem.h"

#include "Engine/GameInstance.h"

void UMassifSimSubsystem::StartNewGame(int32 MasterSeed)
{
	MassifResetSimState(State, MasterSeed);
	PendingCommands.Reset();
	Clock = FMassifSimClock();
	bSimulationStarted = true;
}

void UMassifSimSubsystem::EnqueueCommand(const FMassifSimCommand& Command)
{
	PendingCommands.Add(Command);
}

void UMassifSimSubsystem::SetGameSpeed(EMassifGameSpeed Speed)
{
	Clock.Speed = Speed;
}

void UMassifSimSubsystem::AdvanceFrame(double DeltaSeconds)
{
	if (!bSimulationStarted)
	{
		return;
	}

	const int32 TickCount = Clock.ConsumeTicks(DeltaSeconds);
	for (int32 TickIndex = 0; TickIndex < TickCount; ++TickIndex)
	{
		AdvanceSimTick();
	}
}

void UMassifSimSubsystem::AdvanceSimTick()
{
	TArray<FMassifSimCommand> AppliedCommands = MoveTemp(PendingCommands);
	PendingCommands.Reset();

	MassifAdvanceSimTick(State, AppliedCommands);
}

void UMassifSimSubsystem::Tick(float DeltaTime)
{
	AdvanceFrame(DeltaTime);
}

TStatId UMassifSimSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMassifSimSubsystem, STATGROUP_Tickables);
}

UWorld* UMassifSimSubsystem::GetTickableGameObjectWorld() const
{
	const UGameInstance* Owner = GetGameInstance();
	if (Owner == nullptr)
	{
		return nullptr;
	}

	return Owner->GetWorld();
}
