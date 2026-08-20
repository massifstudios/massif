#include "MassifRiverManager.h"

#include "Async/Async.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Kismet/GameplayStatics.h"
#include "MassifRiverField.h"
#include "MassifUnits.h"
#include "MassifWaterTileMesh.h"

using MassifUnits::MetersToCentimeters;
using MassifWaterTileMesh::TileSizeMeters;

namespace
{
	/** Half the key space each axis gets, so a signed tile coordinate packs unsigned. */
	constexpr int64 TileCoordinateBias = 1 << 20;
}

AMassifRiverManager::AMassifRiverManager()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

uint64 AMassifRiverManager::MakeTileKey(int32 TileX, int32 TileY)
{
	const uint64 KeyX = static_cast<uint64>(TileX + TileCoordinateBias);
	const uint64 KeyY = static_cast<uint64>(TileY + TileCoordinateBias);
	return (KeyX << 32) | (KeyY & 0xFFFFFFFF);
}

double AMassifRiverManager::DistanceToTileMeters(int32 TileX, int32 TileY, const FVector& ViewMeters)
{
	const FVector2d CentreMeters =
		MassifWaterTileMesh::TileOriginMeters(TileX, TileY) + 0.5 * FVector2d(TileSizeMeters, TileSizeMeters);
	return FVector2d(CentreMeters.X - ViewMeters.X, CentreMeters.Y - ViewMeters.Y).Length();
}

void AMassifRiverManager::Initialize(
	TSharedPtr<const FMassifRiverField> InRiverField, UMaterialInterface* InRiverMaterial)
{
	RiverField = InRiverField;
	RiverMaterial = InRiverMaterial;
	FinishedBuilds = MakeShared<FFinishedBuildQueue>();
}

void AMassifRiverManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!RiverField.IsValid())
	{
		return;
	}

	const FVector ViewMeters = GetViewLocationMeters();
	ApplyFinishedBuilds();
	DropDistantTiles(ViewMeters);

	BuildRequests.Reset();
	GatherTiles(ViewMeters);
	LaunchBuilds();
	LogWaterSummaryOnce();
}

void AMassifRiverManager::LogWaterSummaryOnce()
{
	const bool bSettled = InFlightBuildCount == 0 && BuildRequests.IsEmpty();
	if (bLoggedWaterSummary || !bSettled || ComponentPool.IsEmpty())
	{
		return;
	}

	int32 TriangleCount = 0;
	for (const TObjectPtr<UDynamicMeshComponent>& Component : ComponentPool)
	{
		TriangleCount += Component->GetMesh()->TriangleCount();
	}

	UE_LOG(LogTemp, Display, TEXT("Rivers: %d water tiles around the camera, %d of them wet, %d triangles"),
		ResidentTiles.Num(), ComponentPool.Num() - FreeComponentIndices.Num(), TriangleCount);
	bLoggedWaterSummary = true;
}

FVector AMassifRiverManager::GetViewLocationMeters() const
{
	const APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	if (PlayerController == nullptr || PlayerController->PlayerCameraManager == nullptr)
	{
		return FVector::ZeroVector;
	}
	return PlayerController->PlayerCameraManager->GetCameraLocation() / MetersToCentimeters;
}

void AMassifRiverManager::GatherTiles(const FVector& ViewMeters)
{
	const int32 TileReach = FMath::CeilToInt32(VisibleRadiusMeters / TileSizeMeters);
	const int32 CentreTileX = FMath::FloorToInt32(ViewMeters.X / TileSizeMeters);
	const int32 CentreTileY = FMath::FloorToInt32(ViewMeters.Y / TileSizeMeters);

	const int32 SideTileCount = 2 * TileReach + 1;
	for (int32 TileIndex = 0; TileIndex < SideTileCount * SideTileCount; ++TileIndex)
	{
		const int32 TileX = CentreTileX - TileReach + TileIndex % SideTileCount;
		const int32 TileY = CentreTileY - TileReach + TileIndex / SideTileCount;
		const double DistanceMeters = DistanceToTileMeters(TileX, TileY, ViewMeters);
		const bool bWanted =
			DistanceMeters <= VisibleRadiusMeters && !ResidentTiles.Contains(MakeTileKey(TileX, TileY));
		if (bWanted)
		{
			BuildRequests.Add({ TileX, TileY, DistanceMeters });
		}
	}
}

void AMassifRiverManager::DropDistantTiles(const FVector& ViewMeters)
{
	TArray<uint64> Departed;
	for (TPair<uint64, FWaterTile>& Entry : ResidentTiles)
	{
		const double DistanceMeters = DistanceToTileMeters(Entry.Value.TileX, Entry.Value.TileY, ViewMeters);
		// A tile whose build is still running is kept until the mesh comes back,
		// or the result would be applied to a tile that no longer exists.
		if (DistanceMeters > VisibleRadiusMeters * DropRadiusFactor && !Entry.Value.bBuildInFlight)
		{
			ReleaseComponent(Entry.Value);
			Departed.Add(Entry.Key);
		}
	}
	for (const uint64 Key : Departed)
	{
		ResidentTiles.Remove(Key);
	}
}

void AMassifRiverManager::LaunchBuilds()
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

		FWaterTile Tile;
		Tile.TileX = Request.TileX;
		Tile.TileY = Request.TileY;
		Tile.bBuildInFlight = true;
		const uint64 TileKey = MakeTileKey(Request.TileX, Request.TileY);
		ResidentTiles.Add(TileKey, Tile);
		++InFlightBuildCount;

		TSharedPtr<const FMassifRiverField> FieldRef = RiverField;
		TSharedPtr<FFinishedBuildQueue> QueueRef = FinishedBuilds;
		const int32 TileX = Request.TileX;
		const int32 TileY = Request.TileY;
		Async(EAsyncExecution::ThreadPool, [FieldRef, QueueRef, TileKey, TileX, TileY]()
		{
			FFinishedBuild Finished;
			Finished.TileKey = TileKey;
			Finished.Mesh = MassifWaterTileMesh::BuildWaterTileMesh(*FieldRef, TileX, TileY);
			QueueRef->Enqueue(MoveTemp(Finished));
		});
	}
}

void AMassifRiverManager::ApplyFinishedBuilds()
{
	for (int32 Applied = 0; Applied < MaxMeshApplicationsPerFrame; ++Applied)
	{
		FFinishedBuild Finished;
		if (!FinishedBuilds->Dequeue(Finished))
		{
			return;
		}

		--InFlightBuildCount;
		FWaterTile* Tile = ResidentTiles.Find(Finished.TileKey);
		if (Tile == nullptr)
		{
			continue;
		}

		Tile->bBuildInFlight = false;
		ApplyMesh(*Tile, MoveTemp(Finished.Mesh));
	}
}

/** Dry ground is recorded as a resident tile with no component, or it would be rebuilt every frame. */
void AMassifRiverManager::ApplyMesh(FWaterTile& Tile, TSharedPtr<UE::Geometry::FDynamicMesh3> Mesh)
{
	if (!Mesh.IsValid() || Mesh->TriangleCount() == 0)
	{
		return;
	}

	Tile.ComponentIndex = AcquireComponent(Tile.TileX, Tile.TileY);
	ComponentPool[Tile.ComponentIndex]->SetMesh(MoveTemp(*Mesh));
}

int32 AMassifRiverManager::AcquireComponent(int32 TileX, int32 TileY)
{
	const FVector2d OriginMeters = MassifWaterTileMesh::TileOriginMeters(TileX, TileY);
	const FVector LocationCentimeters = FVector(OriginMeters.X, OriginMeters.Y, 0.0) * MetersToCentimeters;
	if (FreeComponentIndices.Num() > 0)
	{
		const int32 Reused = FreeComponentIndices.Pop();
		ComponentPool[Reused]->SetWorldLocation(LocationCentimeters);
		ComponentPool[Reused]->SetVisibility(true);
		return Reused;
	}

	UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(this);
	Component->SetupAttachment(RootComponent);
	Component->RegisterComponent();
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// The surface lies in the trench cut for it, so it has nothing to cast a
	// shadow onto that would be seen.
	Component->SetCastShadow(false);
	Component->SetMaterial(0, RiverMaterial);
	Component->SetWorldLocation(LocationCentimeters);
	return ComponentPool.Add(Component);
}

void AMassifRiverManager::ReleaseComponent(FWaterTile& Tile)
{
	if (Tile.ComponentIndex == INDEX_NONE)
	{
		return;
	}

	ComponentPool[Tile.ComponentIndex]->SetVisibility(false);
	FreeComponentIndices.Add(Tile.ComponentIndex);
	Tile.ComponentIndex = INDEX_NONE;
}
