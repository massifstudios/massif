#pragma once

#include "CoreMinimal.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentProcess.h"
#include "MassifEnvironmentState.h"

/**
 * Processes the scheduler runs as one unit.
 *
 * Most groups hold a single process. A group holds several where they read each
 * other's channels — vegetation holding the soil that vegetation needs, canopy
 * slowing the wind that redistributes the snow that decides where canopy grows.
 * A cycle is not an error in an environment; it is what an environment is. The
 * group is solved by running its members in a fixed order a **fixed number of
 * times**, never until a convergence test is satisfied, because a solve that
 * stops when it has converged stops at a different iteration on a different
 * machine (ARCHITECTURE §4).
 *
 * Members must tolerate reading a channel no one has written yet: on the first
 * iteration, the loop has no previous round to read.
 */
struct FMassifRelaxationGroup
{
	/** Members, sorted by process name, so the order does not depend on registration. */
	TArray<int32> ProcessIndices;

	int32 IterationCount = 1;

	bool IsRelaxed() const
	{
		return IterationCount > 1 || ProcessIndices.Num() > 1;
	}
};

/**
 * Orders the environment processes and runs them (ARCHITECTURE §5.6.3).
 *
 * Order comes from what the processes declare they read and write, not from the
 * order they were registered in: the schedule of a given set of processes is the
 * same however they were added, which is what allows a system to be dropped in
 * without anyone deciding where in a pipeline it belongs. Cycles in those
 * declarations become relaxation groups rather than failures.
 *
 * Changing a parameter or a fidelity marks a process dirty, and the next run
 * covers that process and everything transitively downstream of it and nothing
 * else — which is what keeps interactive fidelity switching affordable when the
 * whole graph takes seconds to solve.
 */
class MASSIFWORLDGEN_API FMassifEnvironmentScheduler
{
public:
	FMassifEnvironmentScheduler(
		TSharedRef<const IMassifElevationSource> InElevation,
		const FMassifGeoreference& InGeoreference,
		uint32 InMasterSeed);

	FMassifEnvironmentScheduler(const FMassifEnvironmentScheduler&) = delete;
	FMassifEnvironmentScheduler& operator=(const FMassifEnvironmentScheduler&) = delete;

	/** Adds a process. The schedule must be rebuilt before the next run. */
	void RegisterProcess(
		TSharedRef<const IMassifEnvironmentProcess> Process,
		EMassifFidelity InitialFidelity = EMassifFidelity::Standard);

	/**
	 * Resolves the declarations into an execution order and registers every
	 * declared output channel with the state. Returns false and fills OutError
	 * where a channel has no producer or two producers, where a process asks for
	 * a fidelity it does not implement, or where a cycle was not declared as
	 * feedback.
	 */
	bool BuildSchedule(FString& OutError);

	/** False where the process does not implement the level; the current one is kept. */
	bool SetFidelity(FName ProcessName, EMassifFidelity Fidelity);
	EMassifFidelity GetFidelity(FName ProcessName) const;

	/** How many times a relaxation group runs its members. Rebuilding the schedule applies it. */
	void SetRelaxationIterationCount(int32 IterationCount);

	/** Whether processes may take their compute path. Off is the reference path and headless. */
	void SetGpuEnabled(bool bEnabled);

	/** Marks the process and everything transitively downstream of it for the next run. */
	void MarkDirty(FName ProcessName);
	void MarkAllDirty();

	/** Runs every dirty group in schedule order. */
	void Execute();

	const FMassifEnvironmentState& GetState() const
	{
		return *State;
	}

	const IMassifElevationSource& GetElevation() const
	{
		return Elevation.Get();
	}

	const FMassifGeoreference& GetGeoreference() const
	{
		return Georeference;
	}

	/** Process names in the order they run. Empty until the schedule is built. */
	TArray<FName> GetExecutionOrder() const;

	const TArray<FMassifRelaxationGroup>& GetGroups() const
	{
		return Groups;
	}

	/** The groups holding more than one process: the feedback loops the graph found. */
	TArray<TArray<FName>> GetFeedbackGroups() const;

	bool IsProcessDirty(FName ProcessName) const;

	int32 GetProcessCount() const
	{
		return Nodes.Num();
	}

private:
	struct FProcessNode
	{
		TSharedRef<const IMassifEnvironmentProcess> Process;
		FMassifProcessDeclaration Declaration;
		EMassifFidelity Fidelity = EMassifFidelity::Standard;

		/** Group the node was placed in, and the direct dependents used to spread dirt. */
		int32 GroupIndex = INDEX_NONE;
	};

	bool ValidateNames(FString& OutError) const;
	bool MapProducers(TMap<FName, int32>& OutProducerOfChannel, FString& OutError) const;
	bool ValidateFidelities(FString& OutError) const;
	bool BuildDependencyEdges(const TMap<FName, int32>& ProducerOfChannel, FString& OutError);
	bool BuildGroups(FString& OutError);
	void RegisterOutputChannels();

	bool IsCyclicComponent(const TArray<int32>& Members) const;
	bool ValidateFeedbackDeclarations(const TArray<TArray<int32>>& ComponentMembers, FString& OutError) const;
	int32 FindUndeclaredFeedbackMember(const TArray<int32>& Members) const;

	void StoreGroups(
		const TArray<int32>& Order,
		const TArray<TArray<int32>>& ComponentMembers,
		const TArray<TSet<int32>>& ComponentEdges);
	void StoreGroup(
		int32 GroupIndex,
		int32 Component,
		const TArray<TArray<int32>>& ComponentMembers,
		const TArray<TSet<int32>>& ComponentEdges,
		const TArray<int32>& GroupOfComponent);

	static int32 CountComponents(const TArray<int32>& ComponentOfNode);

	int32 FindProcessIndex(FName ProcessName) const;
	TArray<FName> NamesOfGroup(const FMassifRelaxationGroup& Group) const;
	void RunGroup(const FMassifRelaxationGroup& Group);
	void RunProcess(const FProcessNode& Node);

	TSharedRef<const IMassifElevationSource> Elevation;
	FMassifGeoreference Georeference;
	uint32 MasterSeed = 0;

	TUniquePtr<FMassifEnvironmentState> State;

	TArray<FProcessNode> Nodes;

	/** Nodes that must run after a node, one entry per node. */
	TArray<TArray<int32>> DependentsOfNode;

	TArray<FMassifRelaxationGroup> Groups;

	/** Groups that must run after a group, one entry per group. */
	TArray<TSet<int32>> DependentsOfGroup;

	/** Dirt is tracked per group: a relaxed group's members all change when any of them runs. */
	TArray<bool> GroupIsDirty;

	int32 RelaxationIterationCount = 4;
	bool bGpuEnabled = true;
	bool bScheduleBuilt = false;
};
