#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassifFoliageManager.generated.h"

class FMassifHabitatField;
class FMassifTerrainField;
class UHierarchicalInstancedStaticMeshComponent;
struct FMassifFoliageSpecies;

/**
 * Plants the massif from the habitat derived over the terrain field: for every
 * species, the scatter tiles within its visible radius of the camera are built
 * and handed to one instanced-mesh component each, and dropped again when the
 * camera leaves.
 *
 * Purely presentation. The scatter is a pure function of the seed and the
 * field (see MassifFoliageScatter), so what grows where is stable for a seed
 * however the camera moves, but nothing here is sim state.
 *
 * A species whose mesh asset is missing is skipped with one warning, so the
 * project runs before the model downloads have landed.
 */
UCLASS()
class AMassifFoliageManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifFoliageManager();

	void Initialize(TSharedPtr<const FMassifTerrainField> InField, uint32 InMasterSeed);

	/** Drops every planted tile and regrows against a new field, as ResetField does for terrain. */
	void ResetField(TSharedPtr<const FMassifTerrainField> InField, uint32 InMasterSeed);

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

	/** Spread the cost of moving: enough that flying does not leave bald ground for long. */
	static constexpr int32 MaxTileBuildsPerFrame = 3;

	TSharedPtr<const FMassifHabitatField> Habitat;
	uint32 MasterSeed = 0;

	UPROPERTY()
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> ComponentPool;

	/** Meshes resolved once at Initialize; null for a species whose asset is absent. */
	UPROPERTY()
	TArray<TObjectPtr<UStaticMesh>> SpeciesMeshes;

	/** Per species, the shift that puts the plant's base on the point it was planted at. */
	TArray<FVector> SpeciesBaseOffsets;

	TArray<int32> FreeComponentIndices;
	TMap<uint64, FPlantedTile> PlantedTiles;
	TArray<FTileRequest> TileRequests;

	FVector GetViewLocationMeters() const;

	void LoadSpeciesMeshes();
	void DropAllTiles();

	/** Collects the tiles that should exist for one species and drops the ones that should not. */
	void GatherSpecies(int32 SpeciesIndex, const FVector& ViewMeters);
	void DropDistantTiles(const FVector& ViewMeters);
	void BuildRequestedTiles();
	void PlantTile(const FTileRequest& Request);

	int32 AcquireComponent(int32 SpeciesIndex);
	void ReleaseComponent(FPlantedTile& Tile);
};
