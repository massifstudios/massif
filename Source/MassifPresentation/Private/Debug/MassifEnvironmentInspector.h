#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentProcess.h"

class FMassifEnvironmentScheduler;

/** One process, as the inspector draws it. */
struct FMassifInspectorProcessRow
{
	FName Name;
	EMassifFidelity Fidelity = EMassifFidelity::Standard;
	bool bDirty = false;

	/** Set where the process shares a relaxation group with others, so it ran more than once. */
	bool bInFeedbackGroup = false;

	double LastExecutionSeconds = 0.0;
};

/** One channel, as the inspector draws it. */
struct FMassifInspectorChannelRow
{
	FName Name;
	FString Unit;
	double Minimum = 0.0;
	double Maximum = 0.0;
	double CellSizeMeters = 0.0;
	bool bWritten = false;
};

/** The graph as text: what runs, in what order, at what level, and what it cost. */
struct FMassifInspectorSnapshot
{
	TArray<FMassifInspectorProcessRow> Processes;
	TArray<FMassifInspectorChannelRow> Channels;
	double TotalSolveSeconds = 0.0;
	int64 ResidentByteCount = 0;

	bool IsEmpty() const
	{
		return Processes.IsEmpty();
	}
};

/**
 * Reads the process graph for the debug panel, and switches fidelities on it.
 *
 * Every one of these processes is a simulation whose output is otherwise only
 * visible as a second-order effect on where trees ended up, so the graph needs
 * to be legible on its own: what ran, in what order, at what level, how long it
 * took, and what range came out the other end (ARCHITECTURE §5.7).
 */
namespace MassifEnvironmentInspector
{
	/** Whether the panel is drawn. Toggled by massif.Env.Inspector. */
	bool IsPanelVisible();

	FMassifInspectorSnapshot Snapshot(const FMassifEnvironmentScheduler& Scheduler);

	/** Parses a fidelity by name, case-insensitively. Unset where the name is not one. */
	TOptional<EMassifFidelity> ParseFidelity(const FString& Text);

	/**
	 * Applies a fidelity and re-runs what it dirtied, on the calling thread.
	 *
	 * The consumers share the state object the solve writes into, so a re-solve is
	 * visible to them the moment it returns and nothing has to be rebuilt — which
	 * is also why this has to stay on the game thread rather than being handed to a
	 * task while something is sampling.
	 */
	bool SetFidelityAndResolve(
		FMassifEnvironmentScheduler& Scheduler, FName ProcessName, EMassifFidelity Fidelity);

	/** The same for every process that implements the level, skipping those that do not. */
	int32 SetPresetAndResolve(FMassifEnvironmentScheduler& Scheduler, EMassifFidelity Fidelity);
}
