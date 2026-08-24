#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassifFoliageManager.generated.h"

class FMassifEnvironmentState;
class FMassifHabitatField;
class FMassifObjectStore;
class FMassifTerrainField;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
struct FMassifFoliageSpecies;

/**
 * Draws the flora of the massif, from two sources that are deliberately different.
 *
 * **Trees are read, not decided.** They were planted once by the vegetation world
 * pass and written into the object store, so this streams them a chunk at a time
 * and turns records into transforms — the same relationship the rock manager has
 * with the boulders beside them. That is what lets a save hold the forest, and
 * what will let a felled tree stay felled.
 *
 * **Everything shorter is scattered here.** The 44 shrub, grass and flower species
 * are a pure function of the habitat, the species, the seed and the tile (see
 * MassifFoliageScatter), so they are built within a species' visible radius of the
 * camera and dropped again when it leaves. Nobody can interact with an individual
 * tussock, so nothing is gained by storing 2.4 billion of them.
 *
 * Purely presentation either way: nothing here is sim state.
 *
 * A species whose model has not been made yet falls back to a tinted primitive
 * of the right size for its growth form, so the whole table can be planted and
 * judged while the models are still being generated. A finished model dropped
 * at the species' own asset path takes over with no other change.
 */
UCLASS()
class AMassifFoliageManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifFoliageManager();

	void Initialize(
		TSharedPtr<const FMassifTerrainField> InField,
		TSharedPtr<const FMassifEnvironmentState> InEnvironment,
		uint32 InMasterSeed,
		TSharedPtr<const FMassifObjectStore> InObjectStore);

	/** Drops every planted tile and regrows against a new field, as ResetField does for terrain. */
	void ResetField(
		TSharedPtr<const FMassifTerrainField> InField,
		TSharedPtr<const FMassifEnvironmentState> InEnvironment,
		uint32 InMasterSeed,
		TSharedPtr<const FMassifObjectStore> InObjectStore);

	virtual void Tick(float DeltaSeconds) override;

private:
	/** One species' tile at one coordinate, holding the component its instances live in. */
	struct FPlantedTile
	{
		int32 SpeciesIndex = 0;
		int32 TileX = 0;
		int32 TileY = 0;
		int32 ComponentIndex = INDEX_NONE;
	};

	struct FTileRequest
	{
		int32 SpeciesIndex = 0;
		int32 TileX = 0;
		int32 TileY = 0;
		double DistanceMeters = 0.0;
	};

	/**
	 * Spread the cost of moving: enough that flying does not leave bald ground
	 * for long. Every species wants its own tiles, so this is shared across all
	 * of them and the whole table has to fill through it.
	 */
	static constexpr int32 MaxTileBuildsPerFrame = 16;

	/**
	 * Fewer than the scatter's, because a chunk is 512 m of every tree species at
	 * once: one of these costs far more than one shrub tile.
	 */
	static constexpr int32 MaxTreeChunkBuildsPerFrame = 4;

	/** One chunk's trees. A chunk holds several species, so it holds a component per species drawn. */
	struct FDrawnTreeChunk
	{
		int32 ChunkColumn = 0;
		int32 ChunkRow = 0;
		TArray<int32> ComponentIndices;
	};

	TSharedPtr<const FMassifHabitatField> Habitat;
	TSharedPtr<const FMassifObjectStore> ObjectStore;
	uint32 MasterSeed = 0;

	/** Which species a stored record belongs to, so a chunk scan does not search the table per record. */
	TMap<uint16, int32> SpeciesIndexByObjectTypeId;

	TMap<uint64, FDrawnTreeChunk> DrawnTreeChunks;
	TArray<FIntPoint> TreeChunkRequests;

	UPROPERTY()
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> ComponentPool;

	/** Meshes resolved once at Initialize; null for a species with neither model nor placeholder. */
	UPROPERTY()
	TArray<TObjectPtr<UStaticMesh>> SpeciesMeshes;

	/** Tinted placeholder material per species, null where the species draws its own model. */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SpeciesMaterials;

	/** Parent of every placeholder material; absent until Tools/create_placeholder_flora.py has run. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> PlaceholderMaterial;

	/** Per species, the shift that puts the plant's base on the point it was planted at. */
	TArray<FVector> SpeciesBaseOffsets;

	TArray<int32> FreeComponentIndices;
	TMap<uint64, FPlantedTile> PlantedTiles;
	TArray<FTileRequest> TileRequests;

	FVector GetViewLocationMeters() const;

	void LoadSpeciesMeshes();

	/** Builds the record type to species lookup the chunk scan reads. */
	void MapStoredSpecies();
	bool LoadOneSpecies(const FMassifFoliageSpecies& Species);
	void AddSpeciesMesh(UStaticMesh& Mesh, UMaterialInstanceDynamic* Material);
	UMaterialInstanceDynamic* MakePlaceholderMaterial(const FLinearColor& Tint);
	void DropAllTiles();

	/** Collects the tiles that should exist for one species and drops the ones that should not. */
	void GatherSpecies(int32 SpeciesIndex, const FVector& ViewMeters);
	void DropDistantTiles(const FVector& ViewMeters);
	void BuildRequestedTiles();
	void PlantTile(const FTileRequest& Request);

	void GatherTreeChunks(const FVector& ViewMeters);
	void DropDistantTreeChunks(const FVector& ViewMeters);
	void BuildRequestedTreeChunks();
	void DrawTreeChunk(int32 ChunkColumn, int32 ChunkRow);
	void AddTreeComponents(const TArray<TArray<FTransform>>& TransformsBySpecies, FDrawnTreeChunk& OutChunk);
	void ReleaseTreeChunk(FDrawnTreeChunk& Chunk);
	void DropAllTreeChunks();

	int32 AcquireComponent(int32 SpeciesIndex);
	void ConfigureComponent(UHierarchicalInstancedStaticMeshComponent& Component, int32 SpeciesIndex);
	void ReleaseComponent(FPlantedTile& Tile);
};
