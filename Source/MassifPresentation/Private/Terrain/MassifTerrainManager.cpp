#include "MassifTerrainManager.h"

#include "Async/Async.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Kismet/GameplayStatics.h"
#include "MassifTerrainField.h"
#include "MassifTerrainTileMesh.h"

using UE::Geometry::FDynamicMesh3;

namespace
{
	constexpr double MetersToCentimeters = 100.0;

	uint64 PackNodeKey(int32 Depth, int32 GridX, int32 GridY)
	{
		return (static_cast<uint64>(Depth) << 42) | (static_cast<uint64>(GridY) << 21) | static_cast<uint64>(GridX);
	}
}

AMassifTerrainManager::AMassifTerrainManager()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AMassifTerrainManager::Initialize(TSharedPtr<const FMassifTerrainField> InField, UMaterialInterface* InTerrainMaterial)
{
	check(InField.IsValid());
	Field = InField;
	TerrainMaterial = InTerrainMaterial;
	FinishedBuilds = MakeShared<FFinishedBuildQueue>();

	// Depth at which one more split would put vertices closer than the finest
	// spacing worth rendering. 150 km with 64-quad tiles bottoms out around
	// depth 14, or 14 cm between vertices.
	const double WorldSizeMeters = Field->GetConfig().WorldSizeMeters;
	MaxTreeDepth = 0;
	double VertexSpacingMeters = WorldSizeMeters / TileQuadCount;
	while (VertexSpacingMeters > FinestVertexSpacingMeters && MaxTreeDepth < 24)
	{
		VertexSpacingMeters *= 0.5;
		++MaxTreeDepth;
	}

	RootNode = MakeNode(0, 0, 0);
	RootNode->bRendered = true;
}

TUniquePtr<AMassifTerrainManager::FQuadNode> AMassifTerrainManager::MakeNode(int32 Depth, int32 GridX, int32 GridY)
{
	const double WorldSizeMeters = Field->GetConfig().WorldSizeMeters;
	const double SizeMeters = WorldSizeMeters / static_cast<double>(1 << Depth);

	TUniquePtr<FQuadNode> Node = MakeUnique<FQuadNode>();
	Node->Key = PackNodeKey(Depth, GridX, GridY);
	Node->Depth = Depth;
	Node->SizeMeters = SizeMeters;
	Node->CenterMeters = FVector2d(
		-WorldSizeMeters * 0.5 + (GridX + 0.5) * SizeMeters,
		-WorldSizeMeters * 0.5 + (GridY + 0.5) * SizeMeters);
	Node->CenterAltitudeMeters = Field->SampleHeightMeters(Node->CenterMeters, SizeMeters / TileQuadCount);
	LiveNodes.Add(Node->Key, Node.Get());
	return Node;
}

void AMassifTerrainManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Field.IsValid())
	{
		return;
	}

	ApplyFinishedBuilds();

	BuildRequests.Reset();
	UpdateNode(*RootNode, GetViewLocationMeters());
	LaunchBuilds();
}

FVector AMassifTerrainManager::GetViewLocationMeters() const
{
	const APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	if (PlayerController == nullptr || PlayerController->PlayerCameraManager == nullptr)
	{
		return FVector::ZeroVector;
	}
	return PlayerController->PlayerCameraManager->GetCameraLocation() / MetersToCentimeters;
}

double AMassifTerrainManager::DistanceToNodeMeters(const FQuadNode& Node, const FVector& ViewMeters) const
{
	const double HalfSize = Node.SizeMeters * 0.5;
	const double OutsideX = FMath::Max(FMath::Abs(ViewMeters.X - Node.CenterMeters.X) - HalfSize, 0.0);
	const double OutsideY = FMath::Max(FMath::Abs(ViewMeters.Y - Node.CenterMeters.Y) - HalfSize, 0.0);
	const double OutsideZ = ViewMeters.Z - Node.CenterAltitudeMeters;
	return FMath::Sqrt(OutsideX * OutsideX + OutsideY * OutsideY + OutsideZ * OutsideZ);
}

void AMassifTerrainManager::EnsureChildren(FQuadNode& Node)
{
	if (Node.Children[0].IsValid())
	{
		return;
	}
	const int32 ChildBaseX = 2 * static_cast<int32>(Node.Key & 0x1FFFFF);
	const int32 ChildBaseY = 2 * static_cast<int32>((Node.Key >> 21) & 0x1FFFFF);
	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		Node.Children[ChildIndex] = MakeNode(Node.Depth + 1, ChildBaseX + (ChildIndex & 1), ChildBaseY + (ChildIndex >> 1));
	}
}

bool AMassifTerrainManager::AreAllChildrenReady(const FQuadNode& Node) const
{
	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		if (!Node.Children[ChildIndex].IsValid() || !Node.Children[ChildIndex]->bMeshReady)
		{
			return false;
		}
	}
	return true;
}

void AMassifTerrainManager::ActivateChildren(FQuadNode& Node)
{
	Node.bChildrenActive = true;
	Node.bRendered = false;
	SetNodeVisibility(Node, false);
	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		Node.Children[ChildIndex]->bRendered = true;
		SetNodeVisibility(*Node.Children[ChildIndex], true);
	}
}

void AMassifTerrainManager::CollapseChildren(FQuadNode& Node)
{
	Node.bChildrenActive = false;
	Node.bRendered = true;
	SetNodeVisibility(Node, true);
	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		DestroySubtree(*Node.Children[ChildIndex]);
		Node.Children[ChildIndex].Reset();
	}
}

void AMassifTerrainManager::DestroySubtree(FQuadNode& Node)
{
	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		if (Node.Children[ChildIndex].IsValid())
		{
			DestroySubtree(*Node.Children[ChildIndex]);
			Node.Children[ChildIndex].Reset();
		}
	}
	ReleaseComponent(Node);
	LiveNodes.Remove(Node.Key);
}

void AMassifTerrainManager::UpdateNode(FQuadNode& Node, const FVector& ViewMeters)
{
	const double DistanceMeters = DistanceToNodeMeters(Node, ViewMeters);
	const bool bWantSplit = Node.Depth < MaxTreeDepth && DistanceMeters < Node.SizeMeters * SplitDistanceFactor;

	if (bWantSplit && !Node.bChildrenActive)
	{
		EnsureChildren(Node);
		if (AreAllChildrenReady(Node))
		{
			ActivateChildren(Node);
		}
	}

	if (Node.bChildrenActive)
	{
		const bool bWantMerge = DistanceMeters > Node.SizeMeters * MergeDistanceFactor;
		if (bWantMerge && Node.bMeshReady)
		{
			CollapseChildren(Node);
			return;
		}
		for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
		{
			UpdateNode(*Node.Children[ChildIndex], ViewMeters);
		}
		if (bWantMerge)
		{
			RequestBuild(Node, DistanceMeters);
		}
		return;
	}

	// This node draws its area. Its own mesh may still be pending, and children
	// created for a split build theirs until all four are ready. Children of a
	// split that never activated are kept through the hysteresis band and
	// dropped once the camera is clearly gone.
	RequestBuild(Node, DistanceMeters);
	if (!Node.Children[0].IsValid())
	{
		return;
	}
	if (DistanceMeters > Node.SizeMeters * MergeDistanceFactor)
	{
		for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
		{
			DestroySubtree(*Node.Children[ChildIndex]);
			Node.Children[ChildIndex].Reset();
		}
		return;
	}
	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		RequestBuild(*Node.Children[ChildIndex], DistanceMeters);
	}
}

void AMassifTerrainManager::RequestBuild(FQuadNode& Node, double DistanceMeters)
{
	if (Node.bMeshReady || Node.bBuildInFlight)
	{
		return;
	}
	BuildRequests.Add({ &Node, DistanceMeters });
}

void AMassifTerrainManager::LaunchBuilds()
{
	BuildRequests.Sort([](const FBuildRequest& Left, const FBuildRequest& Right)
	{
		return Left.DistanceMeters < Right.DistanceMeters;
	});

	for (const FBuildRequest& Request : BuildRequests)
	{
		if (InFlightBuildCount >= MaxInFlightBuilds)
		{
			return;
		}
		Request.Node->bBuildInFlight = true;
		++InFlightBuildCount;

		TSharedPtr<const FMassifTerrainField> FieldRef = Field;
		TSharedPtr<FFinishedBuildQueue> QueueRef = FinishedBuilds;
		const uint64 NodeKey = Request.Node->Key;
		const FVector2d CenterMeters = Request.Node->CenterMeters;
		const double SizeMeters = Request.Node->SizeMeters;
		Async(EAsyncExecution::ThreadPool, [FieldRef, QueueRef, NodeKey, CenterMeters, SizeMeters]()
		{
			FFinishedBuild Finished;
			Finished.NodeKey = NodeKey;
			Finished.Mesh = MassifTerrainTileMesh::BuildTileMesh(*FieldRef, CenterMeters, SizeMeters, TileQuadCount);
			QueueRef->Enqueue(MoveTemp(Finished));
		});
	}
}

void AMassifTerrainManager::ApplyFinishedBuilds()
{
	int32 AppliedCount = 0;
	FFinishedBuild Finished;
	while (AppliedCount < MaxMeshApplicationsPerFrame && FinishedBuilds->Dequeue(Finished))
	{
		--InFlightBuildCount;
		FQuadNode** NodePointer = LiveNodes.Find(Finished.NodeKey);
		if (NodePointer == nullptr)
		{
			// The subtree was collapsed while this build was in flight.
			continue;
		}

		FQuadNode& Node = **NodePointer;
		Node.bBuildInFlight = false;
		Node.bMeshReady = true;
		if (Node.ComponentIndex == INDEX_NONE)
		{
			Node.ComponentIndex = AcquireComponent();
		}
		UDynamicMeshComponent* Component = ComponentPool[Node.ComponentIndex];
		Component->SetWorldLocation(FVector(Node.CenterMeters.X, Node.CenterMeters.Y, 0.0) * MetersToCentimeters);
		Component->SetMesh(MoveTemp(*Finished.Mesh));
		Component->SetVisibility(Node.bRendered);
		++AppliedCount;
	}
}

int32 AMassifTerrainManager::AcquireComponent()
{
	if (FreeComponentIndices.Num() > 0)
	{
		return FreeComponentIndices.Pop();
	}

	UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(this);
	Component->SetupAttachment(RootComponent);
	Component->RegisterComponent();
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetMaterial(0, TerrainMaterial);
	Component->SetVisibility(false);
	return ComponentPool.Add(Component);
}

void AMassifTerrainManager::ReleaseComponent(FQuadNode& Node)
{
	if (Node.ComponentIndex == INDEX_NONE)
	{
		return;
	}
	ComponentPool[Node.ComponentIndex]->SetVisibility(false);
	FreeComponentIndices.Add(Node.ComponentIndex);
	Node.ComponentIndex = INDEX_NONE;
}

void AMassifTerrainManager::SetNodeVisibility(FQuadNode& Node, bool bVisible)
{
	if (Node.ComponentIndex != INDEX_NONE)
	{
		ComponentPool[Node.ComponentIndex]->SetVisibility(bVisible);
	}
}
