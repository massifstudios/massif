#include "MassifRockManager.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "MassifObjectRecord.h"
#include "MassifObjectStore.h"
#include "MassifObjectTypeRegistry.h"
#include "MassifUnits.h"
#include "Materials/MaterialInterface.h"

using MassifUnits::MetersToCentimeters;

namespace
{
	uint64 PackChunkKey(int32 ChunkColumn, int32 ChunkRow)
	{
		return (static_cast<uint64>(static_cast<uint32>(ChunkRow)) << 32)
			| static_cast<uint64>(static_cast<uint32>(ChunkColumn));
	}

	/** Whether this reader is the one that draws the type, the store holding every tier at once. */
	bool IsBoulder(uint16 TypeId)
	{
		const FMassifObjectTypeInfo* TypeInfo = FMassifObjectTypeRegistry::Get().FindTypeInfo(TypeId);
		return TypeInfo != nullptr && TypeInfo->Category == EMassifObjectCategory::Boulder;
	}
}

FTransform MassifRockPlacement::MakeBlockTransform(const FMassifObjectPlacement& Block)
{
	// Half the drawn height stands above the ground before burial takes any of it,
	// so a block at zero burial rests on the surface rather than half through it.
	const double HalfHeightMeters = Block.RadiusMeters * BlockFlattening;
	const double CentreMeters = HalfHeightMeters * (1.0 - 2.0 * Block.BuriedFraction);
	const FVector LocationCentimeters =
		FVector(Block.PositionMeters + FVector3d(0.0, 0.0, CentreMeters)) * MetersToCentimeters;

	const double Scale = Block.RadiusMeters * MetersToCentimeters / PlaceholderRadiusCentimeters;
	return FTransform(Block.Rotation, LocationCentimeters, FVector(Scale, Scale, Scale * BlockFlattening));
}

AMassifRockManager::AMassifRockManager()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AMassifRockManager::Initialize(TSharedPtr<const FMassifObjectStore> InObjectStore)
{
	LoadRockMesh();
	ResetObjectStore(InObjectStore);
}

void AMassifRockManager::ResetObjectStore(TSharedPtr<const FMassifObjectStore> InObjectStore)
{
	check(InObjectStore.IsValid());
	DropAllChunks();
	ObjectStore = InObjectStore;
}

void AMassifRockManager::LoadRockMesh()
{
	// The mesh belongs to the type, not to the drawing code: replacing the
	// placeholder is a registry edit rather than a change here.
	const FMassifObjectTypeInfo* TypeInfo =
		FMassifObjectTypeRegistry::Get().FindTypeInfo(MassifObjectTypes::GraniteBoulder);
	check(TypeInfo != nullptr);

	const FSoftObjectPath MeshPath = TypeInfo->MeshPath.IsValid() ? TypeInfo->MeshPath : TypeInfo->PlaceholderMeshPath;
	RockMesh = Cast<UStaticMesh>(MeshPath.TryLoad());
	if (RockMesh == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Rocks: no mesh at %s, boulders are not drawn"), *MeshPath.ToString());
		return;
	}
	RockMaterial = RockMesh->GetMaterial(0);
}

void AMassifRockManager::DropAllChunks()
{
	for (TPair<uint64, FDrawnChunk>& Entry : DrawnChunks)
	{
		ReleaseComponent(Entry.Value);
	}
	DrawnChunks.Reset();
	ChunkRequests.Reset();
}

void AMassifRockManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!ObjectStore.IsValid() || RockMesh == nullptr)
	{
		return;
	}

	const FVector ViewMeters = GetViewLocationMeters();
	ChunkRequests.Reset();
	DropDistantChunks(ViewMeters);
	GatherChunks(ViewMeters);
	BuildRequestedChunks();
}

FVector AMassifRockManager::GetViewLocationMeters() const
{
	const APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	if (PlayerController == nullptr || PlayerController->PlayerCameraManager == nullptr)
	{
		return FVector::ZeroVector;
	}
	return PlayerController->PlayerCameraManager->GetCameraLocation() / MetersToCentimeters;
}

double AMassifRockManager::DistanceToChunkMeters(int32 ChunkColumn, int32 ChunkRow, const FVector& ViewMeters) const
{
	const FVector2d Centre = ObjectStore->GetChunkGrid().ChunkCentreMeters(ChunkColumn, ChunkRow);
	return FVector2d(Centre.X - ViewMeters.X, Centre.Y - ViewMeters.Y).Length();
}

void AMassifRockManager::DropDistantChunks(const FVector& ViewMeters)
{
	// Hysteresis, as for the foliage: drawn inside the radius, dropped a tenth
	// beyond it, so a camera on the boundary does not rebuild a chunk every frame.
	TArray<uint64> Departed;
	for (TPair<uint64, FDrawnChunk>& Entry : DrawnChunks)
	{
		const double DistanceMeters =
			DistanceToChunkMeters(Entry.Value.ChunkColumn, Entry.Value.ChunkRow, ViewMeters);
		if (DistanceMeters > VisibleRadiusMeters * 1.1)
		{
			ReleaseComponent(Entry.Value);
			Departed.Add(Entry.Key);
		}
	}
	for (const uint64 Key : Departed)
	{
		DrawnChunks.Remove(Key);
	}
}

void AMassifRockManager::GatherChunks(const FVector& ViewMeters)
{
	const FMassifObjectChunkGrid& ChunkGrid = ObjectStore->GetChunkGrid();
	const FIntPoint Centre = ChunkGrid.ContainingChunk(FVector2d(ViewMeters.X, ViewMeters.Y));
	const int32 ChunkReach = FMath::CeilToInt(VisibleRadiusMeters / ChunkGrid.ChunkSizeMeters);

	const int32 SideChunkCount = 2 * ChunkReach + 1;
	for (int32 Index = 0; Index < SideChunkCount * SideChunkCount; ++Index)
	{
		const int32 ChunkColumn = Centre.X - ChunkReach + Index % SideChunkCount;
		const int32 ChunkRow = Centre.Y - ChunkReach + Index / SideChunkCount;
		if (ChunkGrid.ChunkIndex(ChunkColumn, ChunkRow) == INDEX_NONE)
		{
			continue;
		}

		const double DistanceMeters = DistanceToChunkMeters(ChunkColumn, ChunkRow, ViewMeters);
		const bool bWanted = DistanceMeters <= VisibleRadiusMeters
			&& !DrawnChunks.Contains(PackChunkKey(ChunkColumn, ChunkRow));
		if (bWanted)
		{
			ChunkRequests.Add({ ChunkColumn, ChunkRow, DistanceMeters });
		}
	}
}

void AMassifRockManager::BuildRequestedChunks()
{
	ChunkRequests.Sort([](const FChunkRequest& Left, const FChunkRequest& Right)
	{
		return Left.DistanceMeters < Right.DistanceMeters;
	});

	const int32 BuildCount = FMath::Min(ChunkRequests.Num(), MaxChunkBuildsPerFrame);
	for (int32 RequestIndex = 0; RequestIndex < BuildCount; ++RequestIndex)
	{
		DrawChunk(ChunkRequests[RequestIndex]);
	}
}

void AMassifRockManager::DrawChunk(const FChunkRequest& Request)
{
	const FMassifObjectTypeRegistry& Registry = FMassifObjectTypeRegistry::Get();
	const TConstArrayView<FMassifObjectRecord> Records =
		ObjectStore->GetChunkRecords(Request.ChunkColumn, Request.ChunkRow);

	TArray<FTransform> Transforms;
	Transforms.Reserve(Records.Num());
	for (const FMassifObjectRecord& Record : Records)
	{
		FMassifObjectPlacement Block;
		if (IsBoulder(Record.TypeId) && MassifObjectRecordCodec::Decode(Record, Registry, Block))
		{
			Transforms.Add(MassifRockPlacement::MakeBlockTransform(Block));
		}
	}

	FDrawnChunk Chunk;
	Chunk.ChunkColumn = Request.ChunkColumn;
	Chunk.ChunkRow = Request.ChunkRow;

	// Chunks no block reached are still recorded, or they would be re-read every frame.
	if (!Transforms.IsEmpty())
	{
		Chunk.ComponentIndex = AcquireComponent();
		ComponentPool[Chunk.ComponentIndex]->AddInstances(Transforms, false, true);
	}
	DrawnChunks.Add(PackChunkKey(Request.ChunkColumn, Request.ChunkRow), Chunk);
}

int32 AMassifRockManager::AcquireComponent()
{
	if (FreeComponentIndices.Num() > 0)
	{
		return FreeComponentIndices.Pop();
	}

	UHierarchicalInstancedStaticMeshComponent* Component =
		NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
	Component->SetupAttachment(RootComponent);
	Component->RegisterComponent();
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetStaticMesh(RockMesh);
	Component->SetMaterial(0, RockMaterial);
	Component->SetCullDistances(0, FMath::CeilToInt(VisibleRadiusMeters * MetersToCentimeters));
	return ComponentPool.Add(Component);
}

void AMassifRockManager::ReleaseComponent(FDrawnChunk& Chunk)
{
	if (Chunk.ComponentIndex == INDEX_NONE)
	{
		return;
	}
	ComponentPool[Chunk.ComponentIndex]->ClearInstances();
	FreeComponentIndices.Add(Chunk.ComponentIndex);
	Chunk.ComponentIndex = INDEX_NONE;
}
