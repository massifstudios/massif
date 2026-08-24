#include "MassifRockManager.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "MassifEnvironmentState.h"
#include "MassifRockfallProcesses.h"
#include "MassifUnits.h"
#include "Materials/MaterialInterface.h"
#include "UObject/SoftObjectPath.h"

using MassifUnits::MetersToCentimeters;

namespace
{
	/** Stand-in until rock models are generated: the engine's unit sphere, squashed per block. */
	const TCHAR* RockPlaceholderMeshPath = TEXT("/Engine/BasicShapes/Sphere");

	uint64 PackTileKey(int32 TileColumn, int32 TileRow)
	{
		return (static_cast<uint64>(static_cast<uint32>(TileRow)) << 32)
			| static_cast<uint64>(static_cast<uint32>(TileColumn));
	}
}

FTransform MassifRockPlacement::MakeBlockTransform(const FMassifEnvironmentPoint& Block)
{
	const FRotator Rotation(Block.RotationDegrees.Y, Block.RotationDegrees.X, Block.RotationDegrees.Z);

	// Half the drawn height stands above the ground before burial takes any of it,
	// so a block at zero burial rests on the surface rather than half through it.
	const double HalfHeightMeters = Block.RadiusMeters * BlockFlattening;
	const double CentreMeters = HalfHeightMeters * (1.0 - 2.0 * Block.BuriedFraction);
	const FVector LocationCentimeters =
		FVector(Block.PositionMeters + FVector3d(0.0, 0.0, CentreMeters)) * MetersToCentimeters;

	const double Scale = Block.RadiusMeters * MetersToCentimeters / PlaceholderRadiusCentimeters;
	return FTransform(Rotation, LocationCentimeters, FVector(Scale, Scale, Scale * BlockFlattening));
}

AMassifRockManager::AMassifRockManager()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AMassifRockManager::Initialize(TSharedPtr<const FMassifEnvironmentState> InEnvironment)
{
	LoadRockMesh();
	ResetEnvironment(InEnvironment);
}

void AMassifRockManager::ResetEnvironment(TSharedPtr<const FMassifEnvironmentState> InEnvironment)
{
	check(InEnvironment.IsValid());
	DropAllTiles();
	Environment = InEnvironment;
}

void AMassifRockManager::LoadRockMesh()
{
	RockMesh = Cast<UStaticMesh>(FSoftObjectPath(RockPlaceholderMeshPath).TryLoad());
	if (RockMesh == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Rocks: no mesh at %s, boulders are not drawn"), RockPlaceholderMeshPath);
		return;
	}
	RockMaterial = RockMesh->GetMaterial(0);
}

void AMassifRockManager::DropAllTiles()
{
	for (TPair<uint64, FDrawnTile>& Entry : DrawnTiles)
	{
		ReleaseComponent(Entry.Value);
	}
	DrawnTiles.Reset();
	TileRequests.Reset();
}

void AMassifRockManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Environment.IsValid() || RockMesh == nullptr)
	{
		return;
	}
	if (!Environment->FindPointSet(MassifEnvironmentPointSets::RockPlacements()).IsValid())
	{
		return;
	}

	const FVector ViewMeters = GetViewLocationMeters();
	TileRequests.Reset();
	DropDistantTiles(ViewMeters);
	GatherTiles(ViewMeters);
	BuildRequestedTiles();
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

double AMassifRockManager::DistanceToTileMeters(int32 TileColumn, int32 TileRow, const FVector& ViewMeters) const
{
	const FMassifChannelGrid& TileGrid = Environment->GetPointSetTileGrid(
		Environment->FindPointSet(MassifEnvironmentPointSets::RockPlacements()));
	const FVector2d Centre = TileGrid.CellCentreMeters(TileColumn, TileRow);
	return FVector2d(Centre.X - ViewMeters.X, Centre.Y - ViewMeters.Y).Length();
}

void AMassifRockManager::DropDistantTiles(const FVector& ViewMeters)
{
	// Hysteresis, as for the foliage: drawn inside the radius, dropped a tenth
	// beyond it, so a camera on the boundary does not rebuild a tile every frame.
	TArray<uint64> Departed;
	for (TPair<uint64, FDrawnTile>& Entry : DrawnTiles)
	{
		const double DistanceMeters =
			DistanceToTileMeters(Entry.Value.TileColumn, Entry.Value.TileRow, ViewMeters);
		if (DistanceMeters > VisibleRadiusMeters * 1.1)
		{
			ReleaseComponent(Entry.Value);
			Departed.Add(Entry.Key);
		}
	}
	for (const uint64 Key : Departed)
	{
		DrawnTiles.Remove(Key);
	}
}

void AMassifRockManager::GatherTiles(const FVector& ViewMeters)
{
	const FMassifChannelGrid& TileGrid = Environment->GetPointSetTileGrid(
		Environment->FindPointSet(MassifEnvironmentPointSets::RockPlacements()));
	const FIntPoint Centre = TileGrid.ContainingCell(FVector2d(ViewMeters.X, ViewMeters.Y));
	const int32 TileReach = FMath::CeilToInt(VisibleRadiusMeters / TileGrid.CellSizeMeters);

	const int32 SideTileCount = 2 * TileReach + 1;
	for (int32 TileIndex = 0; TileIndex < SideTileCount * SideTileCount; ++TileIndex)
	{
		const int32 TileColumn = Centre.X - TileReach + TileIndex % SideTileCount;
		const int32 TileRow = Centre.Y - TileReach + TileIndex / SideTileCount;
		if (TileGrid.CellIndex(TileColumn, TileRow) == INDEX_NONE)
		{
			continue;
		}

		const double DistanceMeters = DistanceToTileMeters(TileColumn, TileRow, ViewMeters);
		const bool bWanted = DistanceMeters <= VisibleRadiusMeters
			&& !DrawnTiles.Contains(PackTileKey(TileColumn, TileRow));
		if (bWanted)
		{
			TileRequests.Add({ TileColumn, TileRow, DistanceMeters });
		}
	}
}

void AMassifRockManager::BuildRequestedTiles()
{
	TileRequests.Sort([](const FTileRequest& Left, const FTileRequest& Right)
	{
		return Left.DistanceMeters < Right.DistanceMeters;
	});

	const int32 BuildCount = FMath::Min(TileRequests.Num(), MaxTileBuildsPerFrame);
	for (int32 RequestIndex = 0; RequestIndex < BuildCount; ++RequestIndex)
	{
		DrawTile(TileRequests[RequestIndex]);
	}
}

void AMassifRockManager::DrawTile(const FTileRequest& Request)
{
	TArray<FMassifEnvironmentPoint> Blocks;
	Environment->ReadTilePoints(
		Environment->FindPointSet(MassifEnvironmentPointSets::RockPlacements()),
		Request.TileColumn, Request.TileRow, Blocks);

	FDrawnTile Tile;
	Tile.TileColumn = Request.TileColumn;
	Tile.TileRow = Request.TileRow;

	// Tiles no block reached are still recorded, or they would be re-read every frame.
	if (!Blocks.IsEmpty())
	{
		TArray<FTransform> Transforms;
		Transforms.Reserve(Blocks.Num());
		for (const FMassifEnvironmentPoint& Block : Blocks)
		{
			Transforms.Add(MassifRockPlacement::MakeBlockTransform(Block));
		}

		Tile.ComponentIndex = AcquireComponent();
		ComponentPool[Tile.ComponentIndex]->AddInstances(Transforms, false, true);
	}
	DrawnTiles.Add(PackTileKey(Request.TileColumn, Request.TileRow), Tile);
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

void AMassifRockManager::ReleaseComponent(FDrawnTile& Tile)
{
	if (Tile.ComponentIndex == INDEX_NONE)
	{
		return;
	}
	ComponentPool[Tile.ComponentIndex]->ClearInstances();
	FreeComponentIndices.Add(Tile.ComponentIndex);
	Tile.ComponentIndex = INDEX_NONE;
}
