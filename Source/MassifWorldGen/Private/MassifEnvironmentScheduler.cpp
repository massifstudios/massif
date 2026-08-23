#include "MassifEnvironmentScheduler.h"

namespace
{
	void GatherReachable(const TArray<TArray<int32>>& DependentsOfNode, int32 StartIndex, TSet<int32>& OutReachable)
	{
		TArray<int32> Pending = DependentsOfNode[StartIndex];
		while (Pending.Num() > 0)
		{
			const int32 NodeIndex = Pending.Pop();
			bool bAlreadyReached = false;
			OutReachable.Add(NodeIndex, &bAlreadyReached);
			if (!bAlreadyReached)
			{
				Pending.Append(DependentsOfNode[NodeIndex]);
			}
		}
	}

	/**
	 * Everything downstream of each node. The graph is tens of nodes wide, so the
	 * plain closure is cheaper to read than an incremental cycle algorithm and its
	 * cost is invisible next to a single process running over the world.
	 */
	TArray<TSet<int32>> ComputeReachability(const TArray<TArray<int32>>& DependentsOfNode)
	{
		TArray<TSet<int32>> Reachable;
		Reachable.SetNum(DependentsOfNode.Num());
		for (int32 NodeIndex = 0; NodeIndex < DependentsOfNode.Num(); ++NodeIndex)
		{
			GatherReachable(DependentsOfNode, NodeIndex, Reachable[NodeIndex]);
		}
		return Reachable;
	}

	void AssignComponent(
		const TArray<TSet<int32>>& Reachable, int32 NodeIndex, int32 Component, TArray<int32>& ComponentOfNode)
	{
		ComponentOfNode[NodeIndex] = Component;
		for (int32 OtherIndex : Reachable[NodeIndex])
		{
			const bool bMutuallyReachable = Reachable[OtherIndex].Contains(NodeIndex);
			if (bMutuallyReachable)
			{
				ComponentOfNode[OtherIndex] = Component;
			}
		}
	}

	/** Two nodes share a component when each can reach the other: the feedback loops. */
	TArray<int32> ComponentOfEachNode(const TArray<TSet<int32>>& Reachable)
	{
		TArray<int32> ComponentOfNode;
		ComponentOfNode.Init(INDEX_NONE, Reachable.Num());

		int32 NextComponent = 0;
		for (int32 NodeIndex = 0; NodeIndex < Reachable.Num(); ++NodeIndex)
		{
			if (ComponentOfNode[NodeIndex] == INDEX_NONE)
			{
				AssignComponent(Reachable, NodeIndex, NextComponent, ComponentOfNode);
				++NextComponent;
			}
		}
		return ComponentOfNode;
	}

	/** Members per component, each sorted by name so registration order cannot reach the schedule. */
	TArray<TArray<int32>> BuildComponentMembers(
		const TArray<int32>& ComponentOfNode, const TArray<FName>& NodeNames, int32 ComponentCount)
	{
		TArray<TArray<int32>> Members;
		Members.SetNum(ComponentCount);
		for (int32 NodeIndex = 0; NodeIndex < ComponentOfNode.Num(); ++NodeIndex)
		{
			Members[ComponentOfNode[NodeIndex]].Add(NodeIndex);
		}

		for (TArray<int32>& ComponentMembers : Members)
		{
			ComponentMembers.Sort([&NodeNames](int32 Left, int32 Right)
			{
				return NodeNames[Left].LexicalLess(NodeNames[Right]);
			});
		}
		return Members;
	}

	TArray<TSet<int32>> BuildComponentEdges(
		const TArray<TArray<int32>>& DependentsOfNode, const TArray<int32>& ComponentOfNode, int32 ComponentCount)
	{
		TArray<TSet<int32>> DependentsOfComponent;
		DependentsOfComponent.SetNum(ComponentCount);
		for (int32 NodeIndex = 0; NodeIndex < DependentsOfNode.Num(); ++NodeIndex)
		{
			for (int32 DependentIndex : DependentsOfNode[NodeIndex])
			{
				DependentsOfComponent[ComponentOfNode[NodeIndex]].Add(ComponentOfNode[DependentIndex]);
			}
		}

		for (int32 Component = 0; Component < ComponentCount; ++Component)
		{
			DependentsOfComponent[Component].Remove(Component);
		}
		return DependentsOfComponent;
	}

	/** Lowest member name, which orders components that could equally well run in either order. */
	TArray<FString> BuildComponentSortKeys(
		const TArray<TArray<int32>>& ComponentMembers, const TArray<FName>& NodeNames)
	{
		TArray<FString> SortKeys;
		SortKeys.Reserve(ComponentMembers.Num());
		for (const TArray<int32>& Members : ComponentMembers)
		{
			SortKeys.Add(NodeNames[Members[0]].ToString());
		}
		return SortKeys;
	}

	constexpr int32 TakenComponent = -1;

	void ReleaseDependentsOf(const TSet<int32>& Dependents, TArray<int32>& PrerequisiteCounts)
	{
		for (int32 Dependent : Dependents)
		{
			--PrerequisiteCounts[Dependent];
		}
	}

	int32 TakeReadyComponent(TArray<int32>& PrerequisiteCounts, const TArray<FString>& SortKeys)
	{
		int32 Chosen = INDEX_NONE;
		for (int32 Component = 0; Component < PrerequisiteCounts.Num(); ++Component)
		{
			const bool bReady = PrerequisiteCounts[Component] == 0;
			const bool bBetter = Chosen == INDEX_NONE || SortKeys[Component] < SortKeys[Chosen];
			if (bReady && bBetter)
			{
				Chosen = Component;
			}
		}
		return Chosen;
	}

	/**
	 * Canonical execution order over the collapsed graph. Ties break on the lowest
	 * member name rather than on discovery, so the same set of processes schedules
	 * identically however it was assembled.
	 */
	TArray<int32> OrderComponents(const TArray<TSet<int32>>& DependentsOfComponent, const TArray<FString>& SortKeys)
	{
		TArray<int32> PrerequisiteCounts;
		PrerequisiteCounts.Init(0, SortKeys.Num());
		for (const TSet<int32>& Dependents : DependentsOfComponent)
		{
			for (int32 Dependent : Dependents)
			{
				++PrerequisiteCounts[Dependent];
			}
		}

		TArray<int32> Order;
		while (Order.Num() < SortKeys.Num())
		{
			const int32 Component = TakeReadyComponent(PrerequisiteCounts, SortKeys);
			checkf(Component != INDEX_NONE, TEXT("Collapsed process graph still holds a cycle"));
			Order.Add(Component);
			PrerequisiteCounts[Component] = TakenComponent;
			ReleaseDependentsOf(DependentsOfComponent[Component], PrerequisiteCounts);
		}
		return Order;
	}
}

FMassifEnvironmentScheduler::FMassifEnvironmentScheduler(
	TSharedRef<const IMassifElevationSource> InElevation,
	const FMassifGeoreference& InGeoreference,
	uint32 InMasterSeed)
	: Elevation(InElevation)
	, Georeference(InGeoreference)
	, MasterSeed(InMasterSeed)
	, State(MakeShared<FMassifEnvironmentState>(InElevation->GetWorldSizeMeters()))
{
}

void FMassifEnvironmentScheduler::RegisterProcess(
	TSharedRef<const IMassifEnvironmentProcess> Process, EMassifFidelity InitialFidelity)
{
	FProcessNode Node{Process, Process->Declare(), InitialFidelity};
	Nodes.Add(MoveTemp(Node));
	bScheduleBuilt = false;
}

int32 FMassifEnvironmentScheduler::FindProcessIndex(FName ProcessName) const
{
	return Nodes.IndexOfByPredicate([ProcessName](const FProcessNode& Node)
	{
		return Node.Declaration.Name == ProcessName;
	});
}

bool FMassifEnvironmentScheduler::ValidateNames(FString& OutError) const
{
	TSet<FName> SeenNames;
	for (const FProcessNode& Node : Nodes)
	{
		if (Node.Declaration.Name.IsNone())
		{
			OutError = TEXT("A process was registered without a name");
			return false;
		}

		bool bAlreadySeen = false;
		SeenNames.Add(Node.Declaration.Name, &bAlreadySeen);
		if (bAlreadySeen)
		{
			OutError = FString::Printf(TEXT("Two processes are named %s"), *Node.Declaration.Name.ToString());
			return false;
		}
	}
	return true;
}

bool FMassifEnvironmentScheduler::MapProducers(TMap<FName, int32>& OutProducerOfChannel, FString& OutError) const
{
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		for (const FMassifChannelDescriptor& Output : Nodes[NodeIndex].Declaration.OutputChannels)
		{
			const int32* ExistingProducer = OutProducerOfChannel.Find(Output.Name);
			if (ExistingProducer != nullptr)
			{
				OutError = FString::Printf(
					TEXT("Channel %s is produced by both %s and %s"), *Output.Name.ToString(),
					*Nodes[*ExistingProducer].Declaration.Name.ToString(),
					*Nodes[NodeIndex].Declaration.Name.ToString());
				return false;
			}
			OutProducerOfChannel.Add(Output.Name, NodeIndex);
		}
	}
	return true;
}

bool FMassifEnvironmentScheduler::ValidateFidelities(FString& OutError) const
{
	for (const FProcessNode& Node : Nodes)
	{
		if (!Node.Declaration.SupportsFidelity(Node.Fidelity))
		{
			OutError = FString::Printf(
				TEXT("Process %s does not implement fidelity %s"), *Node.Declaration.Name.ToString(),
				MassifFidelityName(Node.Fidelity));
			return false;
		}
	}
	return true;
}

bool FMassifEnvironmentScheduler::BuildDependencyEdges(
	const TMap<FName, int32>& ProducerOfChannel, FString& OutError)
{
	DependentsOfNode.Empty();
	DependentsOfNode.SetNum(Nodes.Num());

	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		for (FName InputName : Nodes[NodeIndex].Declaration.InputChannels)
		{
			const int32* ProducerIndex = ProducerOfChannel.Find(InputName);
			if (ProducerIndex == nullptr)
			{
				OutError = FString::Printf(
					TEXT("Process %s reads channel %s, which no process produces"),
					*Nodes[NodeIndex].Declaration.Name.ToString(), *InputName.ToString());
				return false;
			}
			DependentsOfNode[*ProducerIndex].AddUnique(NodeIndex);
		}

		AddOptionalDependencies(ProducerOfChannel, NodeIndex);
	}
	return true;
}

void FMassifEnvironmentScheduler::AddOptionalDependencies(
	const TMap<FName, int32>& ProducerOfChannel, int32 NodeIndex)
{
	for (FName InputName : Nodes[NodeIndex].Declaration.OptionalInputChannels)
	{
		const int32* ProducerIndex = ProducerOfChannel.Find(InputName);
		if (ProducerIndex != nullptr)
		{
			DependentsOfNode[*ProducerIndex].AddUnique(NodeIndex);
		}
	}
}

void FMassifEnvironmentScheduler::RegisterOutputChannels()
{
	for (const FProcessNode& Node : Nodes)
	{
		for (const FMassifChannelDescriptor& Output : Node.Declaration.OutputChannels)
		{
			State->RegisterChannel(Output);
		}
	}
}

bool FMassifEnvironmentScheduler::IsCyclicComponent(const TArray<int32>& Members) const
{
	if (Members.Num() > 1)
	{
		return true;
	}
	return DependentsOfNode[Members[0]].Contains(Members[0]);
}

/**
 * A cycle the declarations did not admit to is a mistake in the declarations, not
 * a loop to solve: relaxing it would quietly halve the value of every channel in
 * it for a fixed number of rounds and call that an answer.
 */
bool FMassifEnvironmentScheduler::ValidateFeedbackDeclarations(
	const TArray<TArray<int32>>& ComponentMembers, FString& OutError) const
{
	for (const TArray<int32>& Members : ComponentMembers)
	{
		if (!IsCyclicComponent(Members))
		{
			continue;
		}

		const int32 UndeclaredIndex = FindUndeclaredFeedbackMember(Members);
		if (UndeclaredIndex != INDEX_NONE)
		{
			OutError = FString::Printf(
				TEXT("Process %s is in a feedback cycle without declaring bParticipatesInFeedback"),
				*Nodes[UndeclaredIndex].Declaration.Name.ToString());
			return false;
		}
	}
	return true;
}

int32 FMassifEnvironmentScheduler::FindUndeclaredFeedbackMember(const TArray<int32>& Members) const
{
	for (int32 ProcessIndex : Members)
	{
		if (!Nodes[ProcessIndex].Declaration.bParticipatesInFeedback)
		{
			return ProcessIndex;
		}
	}
	return INDEX_NONE;
}

void FMassifEnvironmentScheduler::StoreGroup(
	int32 GroupIndex,
	int32 Component,
	const TArray<TArray<int32>>& ComponentMembers,
	const TArray<TSet<int32>>& ComponentEdges,
	const TArray<int32>& GroupOfComponent)
{
	FMassifRelaxationGroup Group;
	Group.ProcessIndices = ComponentMembers[Component];
	Group.IterationCount = 1;
	if (IsCyclicComponent(Group.ProcessIndices))
	{
		Group.IterationCount = RelaxationIterationCount;
	}

	for (int32 ProcessIndex : Group.ProcessIndices)
	{
		Nodes[ProcessIndex].GroupIndex = GroupIndex;
	}
	for (int32 DependentComponent : ComponentEdges[Component])
	{
		DependentsOfGroup[GroupIndex].Add(GroupOfComponent[DependentComponent]);
	}
	Groups.Add(MoveTemp(Group));
}

void FMassifEnvironmentScheduler::StoreGroups(
	const TArray<int32>& Order,
	const TArray<TArray<int32>>& ComponentMembers,
	const TArray<TSet<int32>>& ComponentEdges)
{
	TArray<int32> GroupOfComponent;
	GroupOfComponent.Init(INDEX_NONE, ComponentMembers.Num());
	for (int32 GroupIndex = 0; GroupIndex < Order.Num(); ++GroupIndex)
	{
		GroupOfComponent[Order[GroupIndex]] = GroupIndex;
	}

	Groups.Empty(Order.Num());
	DependentsOfGroup.Empty();
	DependentsOfGroup.SetNum(Order.Num());
	for (int32 GroupIndex = 0; GroupIndex < Order.Num(); ++GroupIndex)
	{
		StoreGroup(GroupIndex, Order[GroupIndex], ComponentMembers, ComponentEdges, GroupOfComponent);
	}
}

TArray<FName> FMassifEnvironmentScheduler::NamesOfGroup(const FMassifRelaxationGroup& Group) const
{
	TArray<FName> Names;
	Names.Reserve(Group.ProcessIndices.Num());
	for (int32 ProcessIndex : Group.ProcessIndices)
	{
		Names.Add(Nodes[ProcessIndex].Declaration.Name);
	}
	return Names;
}

int32 FMassifEnvironmentScheduler::CountComponents(const TArray<int32>& ComponentOfNode)
{
	if (ComponentOfNode.Num() == 0)
	{
		return 0;
	}
	return FMath::Max(ComponentOfNode) + 1;
}

bool FMassifEnvironmentScheduler::BuildGroups(FString& OutError)
{
	TArray<FName> NodeNames;
	NodeNames.Reserve(Nodes.Num());
	for (const FProcessNode& Node : Nodes)
	{
		NodeNames.Add(Node.Declaration.Name);
	}

	const TArray<TSet<int32>> Reachable = ComputeReachability(DependentsOfNode);
	const TArray<int32> ComponentOfNode = ComponentOfEachNode(Reachable);
	const int32 ComponentCount = CountComponents(ComponentOfNode);

	const TArray<TArray<int32>> ComponentMembers = BuildComponentMembers(ComponentOfNode, NodeNames, ComponentCount);
	if (!ValidateFeedbackDeclarations(ComponentMembers, OutError))
	{
		return false;
	}

	const TArray<TSet<int32>> ComponentEdges = BuildComponentEdges(DependentsOfNode, ComponentOfNode, ComponentCount);
	const TArray<int32> Order = OrderComponents(ComponentEdges, BuildComponentSortKeys(ComponentMembers, NodeNames));
	StoreGroups(Order, ComponentMembers, ComponentEdges);
	return true;
}

bool FMassifEnvironmentScheduler::BuildSchedule(FString& OutError)
{
	OutError.Empty();
	bScheduleBuilt = false;

	TMap<FName, int32> ProducerOfChannel;
	const bool bDeclarationsValid = ValidateNames(OutError) && MapProducers(ProducerOfChannel, OutError)
		&& ValidateFidelities(OutError) && BuildDependencyEdges(ProducerOfChannel, OutError);
	if (!bDeclarationsValid)
	{
		return false;
	}

	if (!BuildGroups(OutError))
	{
		return false;
	}

	RegisterOutputChannels();
	MarkAllDirty();
	bScheduleBuilt = true;
	return true;
}

bool FMassifEnvironmentScheduler::SetFidelity(FName ProcessName, EMassifFidelity Fidelity)
{
	const int32 NodeIndex = FindProcessIndex(ProcessName);
	if (NodeIndex == INDEX_NONE)
	{
		return false;
	}
	if (!Nodes[NodeIndex].Declaration.SupportsFidelity(Fidelity))
	{
		return false;
	}

	Nodes[NodeIndex].Fidelity = Fidelity;
	MarkDirty(ProcessName);
	return true;
}

EMassifFidelity FMassifEnvironmentScheduler::GetFidelity(FName ProcessName) const
{
	const int32 NodeIndex = FindProcessIndex(ProcessName);
	if (NodeIndex == INDEX_NONE)
	{
		return EMassifFidelity::Off;
	}
	return Nodes[NodeIndex].Fidelity;
}

void FMassifEnvironmentScheduler::SetRelaxationIterationCount(int32 IterationCount)
{
	RelaxationIterationCount = FMath::Max(1, IterationCount);
	bScheduleBuilt = false;
}

void FMassifEnvironmentScheduler::SetGpuEnabled(bool bEnabled)
{
	bGpuEnabled = bEnabled;
}

TSet<int32> FMassifEnvironmentScheduler::CollectAffectedGroups(FName ProcessName) const
{
	TSet<int32> Reached;
	const int32 NodeIndex = FindProcessIndex(ProcessName);
	if (NodeIndex == INDEX_NONE || Nodes[NodeIndex].GroupIndex == INDEX_NONE)
	{
		return Reached;
	}

	TArray<int32> Pending;
	Pending.Add(Nodes[NodeIndex].GroupIndex);
	while (Pending.Num() > 0)
	{
		const int32 GroupIndex = Pending.Pop();
		if (!Reached.Contains(GroupIndex))
		{
			Reached.Add(GroupIndex);
			Pending.Append(DependentsOfGroup[GroupIndex].Array());
		}
	}
	return Reached;
}

void FMassifEnvironmentScheduler::MarkDirty(FName ProcessName)
{
	for (int32 GroupIndex : CollectAffectedGroups(ProcessName))
	{
		GroupIsDirty[GroupIndex] = true;
	}
}

TArray<FName> FMassifEnvironmentScheduler::GetAffectedProcesses(FName ProcessName) const
{
	const TSet<int32> Affected = CollectAffectedGroups(ProcessName);

	TArray<FName> Names;
	for (FName Name : GetExecutionOrder())
	{
		const int32 NodeIndex = FindProcessIndex(Name);
		if (Affected.Contains(Nodes[NodeIndex].GroupIndex))
		{
			Names.Add(Name);
		}
	}
	return Names;
}

FString FMassifEnvironmentScheduler::GetDisplayName(FName ProcessName) const
{
	const int32 NodeIndex = FindProcessIndex(ProcessName);
	if (NodeIndex == INDEX_NONE)
	{
		return ProcessName.ToString();
	}
	if (Nodes[NodeIndex].Declaration.DisplayName.IsEmpty())
	{
		return ProcessName.ToString();
	}
	return Nodes[NodeIndex].Declaration.DisplayName;
}

void FMassifEnvironmentScheduler::MarkAllDirty()
{
	GroupIsDirty.Init(true, Groups.Num());
}

void FMassifEnvironmentScheduler::RunProcess(FProcessNode& Node)
{
	const double StartedSeconds = FPlatformTime::Seconds();

	FMassifEnvironmentContext Context(State.Get(), Elevation.Get(), Georeference, MasterSeed, Node.Declaration);
	const bool bRanOnGpu = bGpuEnabled && Node.Process->ExecuteOnGpu(Context, Node.Fidelity);
	if (!bRanOnGpu)
	{
		Node.Process->Execute(Context, Node.Fidelity);
	}

	Node.LastExecutionSeconds = FPlatformTime::Seconds() - StartedSeconds;
}

double FMassifEnvironmentScheduler::GetLastExecutionSeconds(FName ProcessName) const
{
	const int32 NodeIndex = FindProcessIndex(ProcessName);
	if (NodeIndex == INDEX_NONE)
	{
		return 0.0;
	}
	return Nodes[NodeIndex].LastExecutionSeconds;
}

bool FMassifEnvironmentScheduler::IsProcessInFeedbackGroup(FName ProcessName) const
{
	const int32 NodeIndex = FindProcessIndex(ProcessName);
	if (NodeIndex == INDEX_NONE || Nodes[NodeIndex].GroupIndex == INDEX_NONE)
	{
		return false;
	}
	return Groups[Nodes[NodeIndex].GroupIndex].ProcessIndices.Num() > 1;
}

TArray<EMassifFidelity> FMassifEnvironmentScheduler::GetSupportedFidelities(FName ProcessName) const
{
	const int32 NodeIndex = FindProcessIndex(ProcessName);
	if (NodeIndex == INDEX_NONE)
	{
		return {};
	}

	TArray<EMassifFidelity> Supported = {EMassifFidelity::Off};
	Supported.Append(Nodes[NodeIndex].Declaration.SupportedFidelities);
	return Supported;
}

void FMassifEnvironmentScheduler::RunGroup(const FMassifRelaxationGroup& Group)
{
	for (int32 Iteration = 0; Iteration < Group.IterationCount; ++Iteration)
	{
		for (int32 ProcessIndex : Group.ProcessIndices)
		{
			RunProcess(Nodes[ProcessIndex]);
		}
	}
}

void FMassifEnvironmentScheduler::Execute()
{
	checkf(bScheduleBuilt, TEXT("Execute called before BuildSchedule succeeded"));
	for (int32 GroupIndex = 0; GroupIndex < Groups.Num(); ++GroupIndex)
	{
		if (GroupIsDirty[GroupIndex])
		{
			RunGroup(Groups[GroupIndex]);
			GroupIsDirty[GroupIndex] = false;
		}
	}
}

TArray<FName> FMassifEnvironmentScheduler::GetExecutionOrder() const
{
	TArray<FName> Order;
	for (const FMassifRelaxationGroup& Group : Groups)
	{
		for (int32 ProcessIndex : Group.ProcessIndices)
		{
			Order.Add(Nodes[ProcessIndex].Declaration.Name);
		}
	}
	return Order;
}

TArray<TArray<FName>> FMassifEnvironmentScheduler::GetFeedbackGroups() const
{
	TArray<TArray<FName>> FeedbackGroups;
	for (const FMassifRelaxationGroup& Group : Groups)
	{
		if (Group.ProcessIndices.Num() > 1)
		{
			FeedbackGroups.Add(NamesOfGroup(Group));
		}
	}
	return FeedbackGroups;
}

bool FMassifEnvironmentScheduler::IsProcessDirty(FName ProcessName) const
{
	const int32 NodeIndex = FindProcessIndex(ProcessName);
	if (NodeIndex == INDEX_NONE || Nodes[NodeIndex].GroupIndex == INDEX_NONE)
	{
		return false;
	}
	return GroupIsDirty[Nodes[NodeIndex].GroupIndex];
}
