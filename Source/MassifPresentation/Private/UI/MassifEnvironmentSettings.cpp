#include "MassifEnvironmentSettings.h"

#include "MassifEnvironmentScheduler.h"
#include "MassifFidelityLabels.h"

UMassifEnvironmentSettings& UMassifEnvironmentSettings::Get()
{
	return *GetMutableDefault<UMassifEnvironmentSettings>();
}

TMap<FName, FString> MassifEnvironmentSettings::ReadSaved()
{
	return UMassifEnvironmentSettings::Get().ProcessFidelities;
}

int32 MassifEnvironmentSettings::Apply(
	FMassifEnvironmentScheduler& Scheduler, const TMap<FName, FString>& Fidelities)
{
	int32 MovedCount = 0;
	for (const TPair<FName, FString>& Saved : Fidelities)
	{
		const TOptional<EMassifFidelity> Fidelity = MassifFidelityLabels::Parse(Saved.Value);
		if (Fidelity.IsSet())
		{
			MovedCount += static_cast<int32>(Scheduler.SetFidelity(Saved.Key, *Fidelity));
		}
	}
	return MovedCount;
}

int32 MassifEnvironmentSettings::ApplySaved(FMassifEnvironmentScheduler& Scheduler)
{
	return Apply(Scheduler, ReadSaved());
}

void MassifEnvironmentSettings::Save(const FMassifEnvironmentScheduler& Scheduler)
{
	UMassifEnvironmentSettings& Settings = UMassifEnvironmentSettings::Get();
	Settings.ProcessFidelities.Reset();
	for (FName ProcessName : Scheduler.GetExecutionOrder())
	{
		Settings.ProcessFidelities.Add(
			ProcessName, MassifFidelityLabels::Label(Scheduler.GetFidelity(ProcessName)));
	}
	Settings.SaveConfig();
}
