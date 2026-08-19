#include "MassifFoliageManager.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "MassifFoliageScatter.h"
#include "MassifFoliageSpecies.h"
#include "MassifTerrainField.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	constexpr double MetersToCentimeters = 100.0;

	uint64 PackTileKey(int32 SpeciesIndex, int32 TileX, int32 TileY)
	{
		return (static_cast<uint64>(SpeciesIndex) << 48)
			| (static_cast<uint64>(static_cast<uint32>(TileY)) << 24)
			| static_cast<uint64>(static_cast<uint32>(TileX) & 0xFFFFFF);
	}

	double DistanceToTileMeters(const FMassifFoliageSpecies& Species, int32 TileX, int32 TileY, const FVector& ViewMeters)
	{
		const double CenterX = (TileX + 0.5) * Species.TileSizeMeters;
		const double CenterY = (TileY + 0.5) * Species.TileSizeMeters;
		return FVector2d(CenterX - ViewMeters.X, CenterY - ViewMeters.Y).Length();
	}

	TArray<FTransform> MakeInstanceTransforms(const TArray<MassifFoliageScatter::FInstance>& Instances)
	{
		TArray<FTransform> Transforms;
		Transforms.Reserve(Instances.Num());
		for (const MassifFoliageScatter::FInstance& Instance : Instances)
		{
			Transforms.Emplace(
				FRotator(0.0f, Instance.YawDegrees, 0.0f),
				FVector(Instance.LocationMeters) * MetersToCentimeters,
				FVector(Instance.Scale));
		}
		return Transforms;
	}
}

AMassifFoliageManager::AMassifFoliageManager()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AMassifFoliageManager::Initialize(TSharedPtr<const FMassifTerrainField> InField, uint32 InMasterSeed)
{
	LoadSpeciesMeshes();
	ResetField(InField, InMasterSeed);
}

void AMassifFoliageManager::ResetField(TSharedPtr<const FMassifTerrainField> InField, uint32 InMasterSeed)
{
	check(InField.IsValid());
	DropAllTiles();
	Field = InField;
	MasterSeed = InMasterSeed;
}

void AMassifFoliageManager::LoadSpeciesMeshes()
{
	SpeciesMeshes.Reset();
	for (const FMassifFoliageSpecies& Species : MassifFoliage::Species)
	{
		const FSoftObjectPath MeshPath(Species.MeshAssetPath);
		UStaticMesh* Mesh = Cast<UStaticMesh>(MeshPath.TryLoad());
		if (Mesh == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("Foliage species %s has no mesh at %s, not planted"),
				Species.Name, Species.MeshAssetPath);
		}
		SpeciesMeshes.Add(Mesh);
	}
}

void AMassifFoliageManager::DropAllTiles()
{
	for (TPair<uint64, FPlantedTile>& Entry : PlantedTiles)
	{
		ReleaseComponent(Entry.Value);
	}
	PlantedTiles.Reset();
	TileRequests.Reset();
}

void AMassifFoliageManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Field.IsValid())
	{
		return;
	}

	const FVector ViewMeters = GetViewLocationMeters();
	TileRequests.Reset();
	DropDistantTiles(ViewMeters);
	const int32 SpeciesCount = UE_ARRAY_COUNT(MassifFoliage::Species);
	for (int32 SpeciesIndex = 0; SpeciesIndex < SpeciesCount; ++SpeciesIndex)
	{
		GatherSpecies(SpeciesIndex, ViewMeters);
	}
	BuildRequestedTiles();
}

FVector AMassifFoliageManager::GetViewLocationMeters() const
{
	const APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	if (PlayerController == nullptr || PlayerController->PlayerCameraManager == nullptr)
	{
		return FVector::ZeroVector;
	}
	return PlayerController->PlayerCameraManager->GetCameraLocation() / MetersToCentimeters;
}

void AMassifFoliageManager::DropDistantTiles(const FVector& ViewMeters)
{
	// Hysteresis: tiles are planted inside the radius and dropped past a tenth
	// beyond it, so a camera hovering on the boundary does not replant a tile
	// every frame.
	TArray<uint64> Departed;
	for (TPair<uint64, FPlantedTile>& Entry : PlantedTiles)
	{
		const FMassifFoliageSpecies& Species = MassifFoliage::Species[Entry.Value.SpeciesIndex];
		const double DistanceMeters = DistanceToTileMeters(Species, Entry.Value.TileX, Entry.Value.TileY, ViewMeters);
		if (DistanceMeters > Species.VisibleRadiusMeters * 1.1)
		{
			ReleaseComponent(Entry.Value);
			Departed.Add(Entry.Key);
		}
	}
	for (const uint64 Key : Departed)
	{
		PlantedTiles.Remove(Key);
	}
}

void AMassifFoliageManager::GatherSpecies(int32 SpeciesIndex, const FVector& ViewMeters)
{
	if (SpeciesMeshes[SpeciesIndex] == nullptr)
	{
		return;
	}

	const FMassifFoliageSpecies& Species = MassifFoliage::Species[SpeciesIndex];
	const int32 TileReach = FMath::CeilToInt(Species.VisibleRadiusMeters / Species.TileSizeMeters);
	const int32 CenterTileX = FMath::FloorToInt(ViewMeters.X / Species.TileSizeMeters);
	const int32 CenterTileY = FMath::FloorToInt(ViewMeters.Y / Species.TileSizeMeters);

	const int32 SideTileCount = 2 * TileReach + 1;
	for (int32 TileIndex = 0; TileIndex < SideTileCount * SideTileCount; ++TileIndex)
	{
		const int32 TileX = CenterTileX - TileReach + TileIndex % SideTileCount;
		const int32 TileY = CenterTileY - TileReach + TileIndex / SideTileCount;
		const double DistanceMeters = DistanceToTileMeters(Species, TileX, TileY, ViewMeters);
		const bool bWanted =
			DistanceMeters <= Species.VisibleRadiusMeters && !PlantedTiles.Contains(PackTileKey(SpeciesIndex, TileX, TileY));
		if (bWanted)
		{
			TileRequests.Add({ SpeciesIndex, TileX, TileY, DistanceMeters });
		}
	}
}

void AMassifFoliageManager::BuildRequestedTiles()
{
	TileRequests.Sort([](const FTileRequest& Left, const FTileRequest& Right)
	{
		return Left.DistanceMeters < Right.DistanceMeters;
	});

	const int32 BuildCount = FMath::Min(TileRequests.Num(), MaxTileBuildsPerFrame);
	for (int32 RequestIndex = 0; RequestIndex < BuildCount; ++RequestIndex)
	{
		PlantTile(TileRequests[RequestIndex]);
	}
}

void AMassifFoliageManager::PlantTile(const FTileRequest& Request)
{
	const FMassifFoliageSpecies& Species = MassifFoliage::Species[Request.SpeciesIndex];
	const uint32 SpeciesSeed = MassifFoliageScatter::MakeSpeciesSeed(MasterSeed, Species.Name);

	TArray<MassifFoliageScatter::FInstance> Instances;
	MassifFoliageScatter::ScatterTile(*Field, Species, SpeciesSeed, Request.TileX, Request.TileY, Instances);

	FPlantedTile Tile;
	Tile.SpeciesIndex = Request.SpeciesIndex;
	Tile.TileX = Request.TileX;
	Tile.TileY = Request.TileY;
	// Ground that grew nothing is still recorded, or it would be rescattered every frame.
	if (!Instances.IsEmpty())
	{
		Tile.ComponentIndex = AcquireComponent(Request.SpeciesIndex);
		ComponentPool[Tile.ComponentIndex]->AddInstances(MakeInstanceTransforms(Instances), false, true);
	}
	PlantedTiles.Add(PackTileKey(Request.SpeciesIndex, Request.TileX, Request.TileY), Tile);
}

int32 AMassifFoliageManager::AcquireComponent(int32 SpeciesIndex)
{
	const FMassifFoliageSpecies& Species = MassifFoliage::Species[SpeciesIndex];
	if (FreeComponentIndices.Num() > 0)
	{
		const int32 Reused = FreeComponentIndices.Pop();
		ComponentPool[Reused]->SetStaticMesh(SpeciesMeshes[SpeciesIndex]);
		ComponentPool[Reused]->SetCullDistances(0, FMath::CeilToInt(Species.VisibleRadiusMeters * MetersToCentimeters));
		return Reused;
	}

	UHierarchicalInstancedStaticMeshComponent* Component =
		NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
	Component->SetupAttachment(RootComponent);
	Component->RegisterComponent();
	Component->SetStaticMesh(SpeciesMeshes[SpeciesIndex]);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetCullDistances(0, FMath::CeilToInt(Species.VisibleRadiusMeters * MetersToCentimeters));
	return ComponentPool.Add(Component);
}

void AMassifFoliageManager::ReleaseComponent(FPlantedTile& Tile)
{
	if (Tile.ComponentIndex == INDEX_NONE)
	{
		return;
	}
	ComponentPool[Tile.ComponentIndex]->ClearInstances();
	FreeComponentIndices.Add(Tile.ComponentIndex);
	Tile.ComponentIndex = INDEX_NONE;
}
