#include "MassifEnvironmentInspector.h"

#include "HAL/IConsoleManager.h"
#include "Engine/World.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifGameMode.h"
#include "MassifEnvironmentState.h"

namespace
{
	TAutoConsoleVariable<int32> CVarEnvironmentInspector(
		TEXT("massif.Env.Inspector"), 0,
		TEXT("Draw the environment process graph: order, fidelity, dirt and solve time."),
		ECVF_Cheat);

	FMassifInspectorProcessRow MakeProcessRow(const FMassifEnvironmentScheduler& Scheduler, FName ProcessName)
	{
		FMassifInspectorProcessRow Row;
		Row.Name = ProcessName;
		Row.Fidelity = Scheduler.GetFidelity(ProcessName);
		Row.bDirty = Scheduler.IsProcessDirty(ProcessName);
		Row.bInFeedbackGroup = Scheduler.IsProcessInFeedbackGroup(ProcessName);
		Row.LastExecutionSeconds = Scheduler.GetLastExecutionSeconds(ProcessName);
		return Row;
	}

	FMassifInspectorChannelRow MakeChannelRow(
		const FMassifEnvironmentState& State, FMassifChannelHandle Channel)
	{
		const FMassifChannelDescriptor& Descriptor = State.GetDescriptor(Channel);

		FMassifInspectorChannelRow Row;
		Row.Name = Descriptor.Name;
		Row.Unit = Descriptor.Unit;
		Row.CellSizeMeters = Descriptor.CellSizeMeters;
		Row.bWritten = State.IsChannelWritten(Channel);
		Row.Minimum = State.GetChannelMinimum(Channel);
		Row.Maximum = State.GetChannelMaximum(Channel);
		return Row;
	}
}

bool MassifEnvironmentInspector::IsPanelVisible()
{
	return CVarEnvironmentInspector.GetValueOnGameThread() != 0;
}

FMassifInspectorSnapshot MassifEnvironmentInspector::Snapshot(const FMassifEnvironmentScheduler& Scheduler)
{
	FMassifInspectorSnapshot Result;
	for (FName ProcessName : Scheduler.GetExecutionOrder())
	{
		Result.Processes.Add(MakeProcessRow(Scheduler, ProcessName));
		Result.TotalSolveSeconds += Scheduler.GetLastExecutionSeconds(ProcessName);
	}

	const FMassifEnvironmentState& State = Scheduler.GetState();
	for (FMassifChannelHandle Channel : State.GetChannelsByName())
	{
		Result.Channels.Add(MakeChannelRow(State, Channel));
	}

	Result.ResidentByteCount = State.GetResidentByteCount();
	return Result;
}

TOptional<EMassifFidelity> MassifEnvironmentInspector::ParseFidelity(const FString& Text)
{
	const EMassifFidelity Levels[] = {
		EMassifFidelity::Off, EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};
	for (EMassifFidelity Level : Levels)
	{
		if (Text.Equals(MassifFidelityName(Level), ESearchCase::IgnoreCase))
		{
			return Level;
		}
	}
	return {};
}

bool MassifEnvironmentInspector::SetFidelityAndResolve(
	FMassifEnvironmentScheduler& Scheduler, FName ProcessName, EMassifFidelity Fidelity)
{
	if (!Scheduler.SetFidelity(ProcessName, Fidelity))
	{
		return false;
	}

	Scheduler.Execute();
	return true;
}

int32 MassifEnvironmentInspector::SetPresetAndResolve(
	FMassifEnvironmentScheduler& Scheduler, EMassifFidelity Fidelity)
{
	int32 ChangedCount = 0;
	for (FName ProcessName : Scheduler.GetExecutionOrder())
	{
		ChangedCount += static_cast<int32>(Scheduler.SetFidelity(ProcessName, Fidelity));
	}

	Scheduler.Execute();
	return ChangedCount;
}

namespace
{
	FMassifEnvironmentScheduler* FindScheduler(UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}

		const AMassifGameMode* GameMode = World->GetAuthGameMode<AMassifGameMode>();
		if (GameMode == nullptr)
		{
			return nullptr;
		}
		return GameMode->GetEnvironmentScheduler().Get();
	}

	void ListProcesses(const FMassifEnvironmentScheduler& Scheduler)
	{
		for (const FMassifInspectorProcessRow& Row : MassifEnvironmentInspector::Snapshot(Scheduler).Processes)
		{
			UE_LOG(LogTemp, Display, TEXT("  %-28s %-10s %6.0f ms"),
				*Row.Name.ToString(), MassifFidelityName(Row.Fidelity), Row.LastExecutionSeconds * 1000.0);
		}
	}

	void HandleFidelityCommand(const TArray<FString>& Arguments, UWorld* World)
	{
		FMassifEnvironmentScheduler* Scheduler = FindScheduler(World);
		if (Scheduler == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("massif.Env.Fidelity: no environment has been solved yet"));
			return;
		}
		if (Arguments.Num() < 2)
		{
			UE_LOG(LogTemp, Display, TEXT("massif.Env.Fidelity <Process> <Off|Cheap|Standard|Reference>"));
			ListProcesses(*Scheduler);
			return;
		}

		const TOptional<EMassifFidelity> Fidelity = MassifEnvironmentInspector::ParseFidelity(Arguments[1]);
		if (!Fidelity.IsSet())
		{
			UE_LOG(LogTemp, Warning, TEXT("massif.Env.Fidelity: %s is not a level"), *Arguments[1]);
			return;
		}

		const FName ProcessName(*Arguments[0]);
		const double StartedSeconds = FPlatformTime::Seconds();
		if (!MassifEnvironmentInspector::SetFidelityAndResolve(*Scheduler, ProcessName, *Fidelity))
		{
			UE_LOG(LogTemp, Warning, TEXT("massif.Env.Fidelity: %s does not implement %s"),
				*Arguments[0], MassifFidelityName(*Fidelity));
			return;
		}

		UE_LOG(LogTemp, Display, TEXT("%s at %s, re-solved in %.0f ms"),
			*Arguments[0], MassifFidelityName(*Fidelity), (FPlatformTime::Seconds() - StartedSeconds) * 1000.0);
	}

	void HandlePresetCommand(const TArray<FString>& Arguments, UWorld* World)
	{
		FMassifEnvironmentScheduler* Scheduler = FindScheduler(World);
		if (Scheduler == nullptr || Arguments.IsEmpty())
		{
			UE_LOG(LogTemp, Display, TEXT("massif.Env.Preset <Off|Cheap|Standard|Reference>"));
			return;
		}

		const TOptional<EMassifFidelity> Fidelity = MassifEnvironmentInspector::ParseFidelity(Arguments[0]);
		if (!Fidelity.IsSet())
		{
			UE_LOG(LogTemp, Warning, TEXT("massif.Env.Preset: %s is not a level"), *Arguments[0]);
			return;
		}

		const double StartedSeconds = FPlatformTime::Seconds();
		const int32 ChangedCount = MassifEnvironmentInspector::SetPresetAndResolve(*Scheduler, *Fidelity);
		UE_LOG(LogTemp, Display, TEXT("%d processes moved to %s, re-solved in %.0f ms"),
			ChangedCount, MassifFidelityName(*Fidelity), (FPlatformTime::Seconds() - StartedSeconds) * 1000.0);
	}

	FAutoConsoleCommandWithWorldAndArgs GEnvironmentFidelityCommand(
		TEXT("massif.Env.Fidelity"),
		TEXT("Set one environment process's fidelity and re-solve what it dirtied."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HandleFidelityCommand));

	FAutoConsoleCommandWithWorldAndArgs GEnvironmentPresetCommand(
		TEXT("massif.Env.Preset"),
		TEXT("Move every environment process that implements a level to it, and re-solve."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HandlePresetCommand));
}
