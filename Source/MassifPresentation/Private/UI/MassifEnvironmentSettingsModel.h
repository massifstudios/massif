#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentProcess.h"

class FMassifEnvironmentScheduler;

/** One environment system as the settings screen lists it. */
struct FMassifFidelityRow
{
	FName ProcessName;
	FString DisplayName;

	/** What the graph is solving at now, and what an Apply would move it to. */
	EMassifFidelity Current = EMassifFidelity::Standard;
	EMassifFidelity Staged = EMassifFidelity::Standard;

	/** Levels this process implements, lowest first. The rest are drawn but not offered. */
	TArray<EMassifFidelity> SupportedLevels;

	bool IsChanged() const
	{
		return Staged != Current;
	}

	bool Supports(EMassifFidelity Fidelity) const
	{
		return SupportedLevels.Contains(Fidelity);
	}
};

/**
 * The settings screen's state, with no widget in it.
 *
 * Changes are staged rather than applied as they are picked. Three reasons, in
 * order of how much they matter: a solve can take seconds, so applying per click
 * would stall the game once per click; the graph re-solves everything downstream
 * of a change, so three changes applied separately re-solve the shared tail
 * three times; and staging is what lets the screen say what a change will cost
 * before it is paid.
 */
class FMassifEnvironmentSettingsModel
{
public:
	explicit FMassifEnvironmentSettingsModel(FMassifEnvironmentScheduler& InScheduler);

	const TArray<FMassifFidelityRow>& GetRows() const
	{
		return Rows;
	}

	/** Null where nothing carries the name. Rows are rebuilt on Apply, so never hold the pointer. */
	const FMassifFidelityRow* FindRow(FName ProcessName) const;

	/** Ignored where the process does not implement the level, as the scheduler would refuse it. */
	void Stage(FName ProcessName, EMassifFidelity Fidelity);

	/** Drops every staged change. */
	void Revert();

	bool HasChanges() const;

	/**
	 * Every system an Apply would re-solve, in the order it would run them.
	 *
	 * Read out of the graph rather than listed here: a change spreads to
	 * everything downstream of it, and where the graph found systems to be one
	 * feedback loop it spreads sideways to the rest of the loop as well. Nothing in
	 * this file knows which systems those are, which is what keeps the screen
	 * correct when a system is added.
	 */
	TArray<FString> GetAffectedDisplayNames() const;

	/** Moves the graph to the staged levels, re-solves once, and saves. Seconds the solve took. */
	double Apply();

private:
	void RefreshRows();

	FMassifEnvironmentScheduler& Scheduler;
	TArray<FMassifFidelityRow> Rows;
};
