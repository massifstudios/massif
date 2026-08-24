#include "MassifEnvironmentSettingsModel.h"

#include "MassifEnvironmentScheduler.h"
#include "MassifEnvironmentSettings.h"
#include "MassifFidelityLabels.h"

namespace
{
	/** Supported levels in ladder order, whatever order the process declared them in. */
	TArray<EMassifFidelity> SortedSupportedLevels(
		const FMassifEnvironmentScheduler& Scheduler, FName ProcessName)
	{
		const TArray<EMassifFidelity> Declared = Scheduler.GetSupportedFidelities(ProcessName);

		TArray<EMassifFidelity> Sorted;
		for (EMassifFidelity Fidelity : MassifFidelityLabels::AllLevels())
		{
			if (Declared.Contains(Fidelity))
			{
				Sorted.Add(Fidelity);
			}
		}
		return Sorted;
	}
}

FMassifEnvironmentSettingsModel::FMassifEnvironmentSettingsModel(
	FMassifEnvironmentScheduler& InScheduler, EMassifSettingsApplyMode InApplyMode)
	: Scheduler(InScheduler)
	, ApplyMode(InApplyMode)
{
	RefreshRows();
}

void FMassifEnvironmentSettingsModel::RefreshRows()
{
	Rows.Reset();
	for (FName ProcessName : Scheduler.GetExecutionOrder())
	{
		FMassifFidelityRow Row;
		Row.ProcessName = ProcessName;
		Row.DisplayName = Scheduler.GetDisplayName(ProcessName);
		Row.Current = Scheduler.GetFidelity(ProcessName);
		Row.Staged = Row.Current;
		Row.SupportedLevels = SortedSupportedLevels(Scheduler, ProcessName);
		Rows.Add(MoveTemp(Row));
	}
}

const FMassifFidelityRow* FMassifEnvironmentSettingsModel::FindRow(FName ProcessName) const
{
	return Rows.FindByPredicate([ProcessName](const FMassifFidelityRow& Candidate)
	{
		return Candidate.ProcessName == ProcessName;
	});
}

void FMassifEnvironmentSettingsModel::Stage(FName ProcessName, EMassifFidelity Fidelity)
{
	FMassifFidelityRow* Row = const_cast<FMassifFidelityRow*>(FindRow(ProcessName));
	if (Row == nullptr || !Row->Supports(Fidelity))
	{
		return;
	}
	Row->Staged = Fidelity;
}

void FMassifEnvironmentSettingsModel::Revert()
{
	for (FMassifFidelityRow& Row : Rows)
	{
		Row.Staged = Row.Current;
	}
}

bool FMassifEnvironmentSettingsModel::HasChanges() const
{
	return Rows.ContainsByPredicate([](const FMassifFidelityRow& Row)
	{
		return Row.IsChanged();
	});
}

TArray<FString> FMassifEnvironmentSettingsModel::GetAffectedDisplayNames() const
{
	TSet<FName> Affected;
	for (const FMassifFidelityRow& Row : Rows)
	{
		if (Row.IsChanged())
		{
			Affected.Append(Scheduler.GetAffectedProcesses(Row.ProcessName));
		}
	}

	TArray<FString> Names;
	for (FName ProcessName : Scheduler.GetExecutionOrder())
	{
		if (Affected.Contains(ProcessName))
		{
			Names.Add(Scheduler.GetDisplayName(ProcessName));
		}
	}
	return Names;
}

double FMassifEnvironmentSettingsModel::Apply()
{
	for (const FMassifFidelityRow& Row : Rows)
	{
		Scheduler.SetFidelity(Row.ProcessName, Row.Staged);
	}

	MassifEnvironmentSettings::Save(Scheduler);
	if (ApplyMode == EMassifSettingsApplyMode::RecordForNextGeneration)
	{
		RefreshRows();
		return 0.0;
	}

	const double StartedSeconds = FPlatformTime::Seconds();
	Scheduler.Execute();
	const double SolveSeconds = FPlatformTime::Seconds() - StartedSeconds;

	RefreshRows();
	return SolveSeconds;
}
