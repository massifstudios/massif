#include "MassifFoliageManager.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "MassifUnits.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MassifFoliageScatter.h"
#include "MassifFoliageSpecies.h"
#include "MassifEnvironmentState.h"
#include "MassifHabitat.h"
#include "MassifObjectStore.h"
#include "MassifObjectTypeRegistry.h"
#include "MassifTerrainField.h"
#include "UObject/SoftObjectPath.h"

using MassifUnits::MetersToCentimeters;

namespace
{
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

	/**
	 * Where the plant sits inside its own mesh: scanned models are modelled
	 * wherever they stood in the source scene, so the shape can be metres from
	 * the mesh origin and above it. Planting a mesh at a point without this
	 * would leave it beside that point and hovering over the ground.
	 */
	FVector ComputeBaseOffsetCentimeters(const UStaticMesh& Mesh)
	{
		const FBoxSphereBounds Bounds = Mesh.GetBounds();
		return FVector(-Bounds.Origin.X, -Bounds.Origin.Y, Bounds.BoxExtent.Z - Bounds.Origin.Z);
	}

	uint64 PackTreeChunkKey(int32 ChunkColumn, int32 ChunkRow)
	{
		return (static_cast<uint64>(static_cast<uint32>(ChunkRow)) << 32)
			| static_cast<uint64>(static_cast<uint32>(ChunkColumn));
	}

	double DistanceToTreeChunkMeters(
		const FMassifObjectChunkGrid& Grid, int32 Column, int32 Row, const FVector& ViewMeters)
	{
		const FVector2d CentreMeters = Grid.ChunkCentreMeters(Column, Row);
		return FVector2d(CentreMeters.X - ViewMeters.X, CentreMeters.Y - ViewMeters.Y).Length();
	}

	/** How far out trees are drawn: the furthest any stored species asks to be seen from. */
	double TreeVisibleRadiusMeters()
	{
		static const double RadiusMeters = []
		{
			double Furthest = 0.0;
			for (const FMassifFoliageSpecies& Species : MassifFoliage::Species)
			{
				if (Species.ObjectTypeId != MassifInvalidObjectTypeId)
				{
					Furthest = FMath::Max(Furthest, Species.VisibleRadiusMeters);
				}
			}
			return Furthest;
		}();
		return RadiusMeters;
	}

	/**
	 * Where one stored tree is drawn.
	 *
	 * The record keeps a crown radius rather than a mesh scale, because a scale is
	 * meaningless without the mesh it multiplies and the mesh is going to be
	 * replaced. The species' own radius at scale 1 is what turns it back.
	 */
	FTransform MakeTreeTransform(
		const FMassifObjectPlacement& Tree, const FMassifFoliageSpecies& Species,
		const FVector& BaseOffsetCentimeters)
	{
		const float Scale = static_cast<float>(Tree.RadiusMeters / Species.CrownRadiusMeters);
		const FRotator Rotation(0.0f, Tree.Rotation.Yaw, 0.0f);
		const FVector GroundCentimeters = FVector(Tree.PositionMeters) * MetersToCentimeters;
		const FVector OffsetCentimeters = Rotation.RotateVector(BaseOffsetCentimeters * Scale);
		return FTransform(Rotation, GroundCentimeters + OffsetCentimeters, FVector(Scale));
	}

	TArray<FTransform> MakeInstanceTransforms(
		const TArray<MassifFoliageScatter::FInstance>& Instances, const FVector& BaseOffsetCentimeters)
	{
		TArray<FTransform> Transforms;
		Transforms.Reserve(Instances.Num());
		for (const MassifFoliageScatter::FInstance& Instance : Instances)
		{
			const FRotator Rotation(0.0f, Instance.YawDegrees, 0.0f);
			const FVector GroundCentimeters = FVector(Instance.LocationMeters) * MetersToCentimeters;
			const FVector OffsetCentimeters = Rotation.RotateVector(BaseOffsetCentimeters * Instance.Scale);
			Transforms.Emplace(Rotation, GroundCentimeters + OffsetCentimeters, FVector(Instance.Scale));
		}
		return Transforms;
	}
}

AMassifFoliageManager::AMassifFoliageManager()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AMassifFoliageManager::Initialize(
	TSharedPtr<const FMassifTerrainField> InField,
	TSharedPtr<const FMassifEnvironmentState> InEnvironment,
	uint32 InMasterSeed,
	TSharedPtr<const FMassifObjectStore> InObjectStore)
{
	LoadSpeciesMeshes();
	MapStoredSpecies();
	ResetField(InField, InEnvironment, InMasterSeed, InObjectStore);
}

void AMassifFoliageManager::ResetField(
	TSharedPtr<const FMassifTerrainField> InField,
	TSharedPtr<const FMassifEnvironmentState> InEnvironment,
	uint32 InMasterSeed,
	TSharedPtr<const FMassifObjectStore> InObjectStore)
{
	check(InField.IsValid() && InEnvironment.IsValid() && InObjectStore.IsValid());
	DropAllTiles();
	DropAllTreeChunks();
	Habitat = MakeShared<const FMassifHabitatField>(
		InField.ToSharedRef(), InEnvironment.ToSharedRef(), InMasterSeed);
	ObjectStore = InObjectStore;
	MasterSeed = InMasterSeed;
}

void AMassifFoliageManager::MapStoredSpecies()
{
	SpeciesIndexByObjectTypeId.Reset();
	const int32 SpeciesCount = UE_ARRAY_COUNT(MassifFoliage::Species);
	for (int32 SpeciesIndex = 0; SpeciesIndex < SpeciesCount; ++SpeciesIndex)
	{
		const uint16 TypeId = MassifFoliage::Species[SpeciesIndex].ObjectTypeId;
		if (TypeId != MassifInvalidObjectTypeId)
		{
			SpeciesIndexByObjectTypeId.Add(TypeId, SpeciesIndex);
		}
	}
}

void AMassifFoliageManager::LoadSpeciesMeshes()
{
	SpeciesMeshes.Reset();
	SpeciesBaseOffsets.Reset();
	SpeciesMaterials.Reset();
	PlaceholderMaterial = Cast<UMaterialInterface>(
		FSoftObjectPath(MassifFoliagePlaceholder::MaterialAssetPath).TryLoad());

	int32 PlaceholderCount = 0;
	for (const FMassifFoliageSpecies& Species : MassifFoliage::Species)
	{
		PlaceholderCount += LoadOneSpecies(Species) ? 1 : 0;
	}
	if (PlaceholderCount > 0)
	{
		UE_LOG(LogTemp, Display, TEXT("Foliage: %d of %d species drawn as placeholders"),
			PlaceholderCount, static_cast<int32>(UE_ARRAY_COUNT(MassifFoliage::Species)));
	}
}

/** True when the species fell back to its placeholder. */
bool AMassifFoliageManager::LoadOneSpecies(const FMassifFoliageSpecies& Species)
{
	UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(Species.MeshAssetPath).TryLoad());
	if (Mesh != nullptr)
	{
		AddSpeciesMesh(*Mesh, nullptr);
		return false;
	}

	UStaticMesh* Placeholder = Cast<UStaticMesh>(FSoftObjectPath(Species.PlaceholderMeshAssetPath).TryLoad());
	if (Placeholder == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Foliage species %s has neither %s nor a placeholder at %s, not planted"),
			Species.Name, Species.MeshAssetPath, Species.PlaceholderMeshAssetPath);
		SpeciesMeshes.Add(nullptr);
		SpeciesBaseOffsets.Add(FVector::ZeroVector);
		SpeciesMaterials.Add(nullptr);
		return false;
	}

	AddSpeciesMesh(*Placeholder, MakePlaceholderMaterial(Species.PlaceholderTint));
	return true;
}

/**
 * Placeholders share one primitive per growth form, so the tint is what tells
 * two species drawn from the same shape apart on the hillside.
 */
UMaterialInstanceDynamic* AMassifFoliageManager::MakePlaceholderMaterial(const FLinearColor& Tint)
{
	if (PlaceholderMaterial == nullptr)
	{
		return nullptr;
	}

	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(PlaceholderMaterial, this);
	Material->SetVectorParameterValue(TEXT("Tint"), Tint);
	return Material;
}

void AMassifFoliageManager::AddSpeciesMesh(UStaticMesh& Mesh, UMaterialInstanceDynamic* Material)
{
	SpeciesMeshes.Add(&Mesh);
	SpeciesBaseOffsets.Add(ComputeBaseOffsetCentimeters(Mesh));
	SpeciesMaterials.Add(Material);
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
	if (!Habitat.IsValid())
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

	TreeChunkRequests.Reset();
	DropDistantTreeChunks(ViewMeters);
	GatherTreeChunks(ViewMeters);
	BuildRequestedTreeChunks();
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
	// A stored species is not scattered: where its trees stand was settled once by
	// the vegetation pass, and re-deriving it here would be a second opinion.
	if (SpeciesMeshes[SpeciesIndex] == nullptr
		|| MassifFoliage::Species[SpeciesIndex].ObjectTypeId != MassifInvalidObjectTypeId)
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
	MassifFoliageScatter::ScatterTile(*Habitat, Species, SpeciesSeed, Request.TileX, Request.TileY, Instances);

	FPlantedTile Tile;
	Tile.SpeciesIndex = Request.SpeciesIndex;
	Tile.TileX = Request.TileX;
	Tile.TileY = Request.TileY;
	// Ground that grew nothing is still recorded, or it would be rescattered every frame.
	if (!Instances.IsEmpty())
	{
		Tile.ComponentIndex = AcquireComponent(Request.SpeciesIndex);
		ComponentPool[Tile.ComponentIndex]->AddInstances(
			MakeInstanceTransforms(Instances, SpeciesBaseOffsets[Request.SpeciesIndex]), false, true);
	}
	PlantedTiles.Add(PackTileKey(Request.SpeciesIndex, Request.TileX, Request.TileY), Tile);
}

void AMassifFoliageManager::GatherTreeChunks(const FVector& ViewMeters)
{
	const FMassifObjectChunkGrid& Grid = ObjectStore->GetChunkGrid();
	const int32 ChunkReach = FMath::CeilToInt32(TreeVisibleRadiusMeters() / Grid.ChunkSizeMeters);
	const FIntPoint CentreChunk = Grid.ContainingChunk(FVector2d(ViewMeters.X, ViewMeters.Y));

	const int32 SideChunkCount = 2 * ChunkReach + 1;
	for (int32 ChunkIndex = 0; ChunkIndex < SideChunkCount * SideChunkCount; ++ChunkIndex)
	{
		const int32 Column = CentreChunk.X - ChunkReach + ChunkIndex % SideChunkCount;
		const int32 Row = CentreChunk.Y - ChunkReach + ChunkIndex / SideChunkCount;
		const bool bWanted =
			DistanceToTreeChunkMeters(Grid, Column, Row, ViewMeters) <= TreeVisibleRadiusMeters()
			&& !DrawnTreeChunks.Contains(PackTreeChunkKey(Column, Row));
		if (bWanted)
		{
			TreeChunkRequests.Add(FIntPoint(Column, Row));
		}
	}
}

void AMassifFoliageManager::DropDistantTreeChunks(const FVector& ViewMeters)
{
	const FMassifObjectChunkGrid& Grid = ObjectStore->GetChunkGrid();

	TArray<uint64> Departed;
	for (TPair<uint64, FDrawnTreeChunk>& Entry : DrawnTreeChunks)
	{
		const double DistanceMeters =
			DistanceToTreeChunkMeters(Grid, Entry.Value.ChunkColumn, Entry.Value.ChunkRow, ViewMeters);
		if (DistanceMeters > TreeVisibleRadiusMeters() * 1.1)
		{
			ReleaseTreeChunk(Entry.Value);
			Departed.Add(Entry.Key);
		}
	}
	for (const uint64 Key : Departed)
	{
		DrawnTreeChunks.Remove(Key);
	}
}

void AMassifFoliageManager::BuildRequestedTreeChunks()
{
	const int32 BuildCount = FMath::Min(TreeChunkRequests.Num(), MaxTreeChunkBuildsPerFrame);
	for (int32 RequestIndex = 0; RequestIndex < BuildCount; ++RequestIndex)
	{
		DrawTreeChunk(TreeChunkRequests[RequestIndex].X, TreeChunkRequests[RequestIndex].Y);
	}
}

/**
 * One pass over the chunk, sorting its records into the species they belong to.
 *
 * A chunk holds every stored object in its square, boulders included, so the
 * records this does not recognise are another reader's — not an error.
 */
void AMassifFoliageManager::DrawTreeChunk(int32 ChunkColumn, int32 ChunkRow)
{
	const int32 SpeciesCount = UE_ARRAY_COUNT(MassifFoliage::Species);
	TArray<TArray<FTransform>> TransformsBySpecies;
	TransformsBySpecies.SetNum(SpeciesCount);

	const FMassifObjectTypeRegistry& Registry = FMassifObjectTypeRegistry::Get();
	for (const FMassifObjectRecord& Record : ObjectStore->GetChunkRecords(ChunkColumn, ChunkRow))
	{
		const int32* SpeciesIndex = SpeciesIndexByObjectTypeId.Find(Record.TypeId);
		if (SpeciesIndex == nullptr || SpeciesMeshes[*SpeciesIndex] == nullptr)
		{
			continue;
		}

		FMassifObjectPlacement Tree;
		if (!MassifObjectRecordCodec::Decode(Record, Registry, Tree))
		{
			continue;
		}
		TransformsBySpecies[*SpeciesIndex].Add(
			MakeTreeTransform(Tree, MassifFoliage::Species[*SpeciesIndex], SpeciesBaseOffsets[*SpeciesIndex]));
	}

	FDrawnTreeChunk Chunk;
	Chunk.ChunkColumn = ChunkColumn;
	Chunk.ChunkRow = ChunkRow;
	AddTreeComponents(TransformsBySpecies, Chunk);

	// A chunk that grew nothing is still recorded, or it would be rescanned every frame.
	DrawnTreeChunks.Add(PackTreeChunkKey(ChunkColumn, ChunkRow), MoveTemp(Chunk));
}

void AMassifFoliageManager::AddTreeComponents(
	const TArray<TArray<FTransform>>& TransformsBySpecies, FDrawnTreeChunk& OutChunk)
{
	for (int32 SpeciesIndex = 0; SpeciesIndex < TransformsBySpecies.Num(); ++SpeciesIndex)
	{
		if (TransformsBySpecies[SpeciesIndex].IsEmpty())
		{
			continue;
		}
		const int32 ComponentIndex = AcquireComponent(SpeciesIndex);
		ComponentPool[ComponentIndex]->AddInstances(TransformsBySpecies[SpeciesIndex], false, true);
		OutChunk.ComponentIndices.Add(ComponentIndex);
	}
}

void AMassifFoliageManager::ReleaseTreeChunk(FDrawnTreeChunk& Chunk)
{
	for (const int32 ComponentIndex : Chunk.ComponentIndices)
	{
		ComponentPool[ComponentIndex]->ClearInstances();
		FreeComponentIndices.Add(ComponentIndex);
	}
	Chunk.ComponentIndices.Reset();
}

void AMassifFoliageManager::DropAllTreeChunks()
{
	for (TPair<uint64, FDrawnTreeChunk>& Entry : DrawnTreeChunks)
	{
		ReleaseTreeChunk(Entry.Value);
	}
	DrawnTreeChunks.Reset();
	TreeChunkRequests.Reset();
}

int32 AMassifFoliageManager::AcquireComponent(int32 SpeciesIndex)
{
	if (FreeComponentIndices.Num() > 0)
	{
		const int32 Reused = FreeComponentIndices.Pop();
		ConfigureComponent(*ComponentPool[Reused], SpeciesIndex);
		return Reused;
	}

	UHierarchicalInstancedStaticMeshComponent* Component =
		NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
	Component->SetupAttachment(RootComponent);
	Component->RegisterComponent();
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ConfigureComponent(*Component, SpeciesIndex);
	return ComponentPool.Add(Component);
}

/** Components are pooled across species, so everything species-specific is set on acquire. */
void AMassifFoliageManager::ConfigureComponent(
	UHierarchicalInstancedStaticMeshComponent& Component, int32 SpeciesIndex)
{
	const FMassifFoliageSpecies& Species = MassifFoliage::Species[SpeciesIndex];
	Component.SetStaticMesh(SpeciesMeshes[SpeciesIndex]);
	Component.SetCullDistances(0, FMath::CeilToInt(Species.VisibleRadiusMeters * MetersToCentimeters));
	Component.SetMaterial(0, SpeciesMaterials[SpeciesIndex]);
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
