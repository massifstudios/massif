#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassifRockManager.generated.h"

class FMassifEnvironmentState;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;
struct FMassifEnvironmentPoint;

namespace MassifRockPlacement
{
	/** Radius of the stand-in sphere in centimetres, which a block's own radius scales against. */
	inline constexpr double PlaceholderRadiusCentimeters = 50.0;

	/** How much flatter than a ball a block is drawn, so a scree slope does not read as marbles. */
	inline constexpr double BlockFlattening = 0.62;

	/**
	 * Where one solved block is drawn, in centimetres.
	 *
	 * The solve puts a block's position on the ground it came to rest on, and how
	 * far into that ground it settled is carried separately, so the mesh has to be
	 * lifted by what stands proud of the surface rather than centred on the point.
	 */
	FTransform MakeBlockTransform(const FMassifEnvironmentPoint& Block);
}

/**
 * Draws the boulders the rockfall left, a storage tile at a time within reach of
 * the camera, and drops them again when it leaves.
 *
 * Unlike the foliage beside it this scatters nothing. Where each boulder sits was
 * decided by the fall that put it there (MassifRockfallProcesses), so this reads
 * the placements out of the solve and does no more than turn metres into
 * transforms — which is the whole reason the fall runs over the world rather than
 * per tile: a block arrives from a cliff far outside the tile it lands in, and no
 * apron is wide enough to find it locally (ARCHITECTURE §5.4).
 */
UCLASS()
class AMassifRockManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifRockManager();

	void Initialize(TSharedPtr<const FMassifEnvironmentState> InEnvironment);

	/** Drops every drawn tile and reads a new solve, as the foliage does on a rebuild. */
	void ResetEnvironment(TSharedPtr<const FMassifEnvironmentState> InEnvironment);

	virtual void Tick(float DeltaSeconds) override;

private:
	/** One storage tile's boulders, and the component they live in. */
	struct FDrawnTile
	{
		int32 TileColumn = 0;
		int32 TileRow = 0;
		int32 ComponentIndex = INDEX_NONE;
	};

	struct FTileRequest
	{
		int32 TileColumn = 0;
		int32 TileRow = 0;
		double DistanceMeters = 0.0;
	};

	/**
	 * How far boulders are drawn. Shorter than the forest: a half-metre block is
	 * not worth a draw call at a kilometre, and the deposit channel is what paints
	 * distant scree onto the terrain material instead.
	 */
	static constexpr double VisibleRadiusMeters = 700.0;

	/** Spread the cost of moving, as the foliage manager does. */
	static constexpr int32 MaxTileBuildsPerFrame = 8;

	void LoadRockMesh();
	void DropAllTiles();

	FVector GetViewLocationMeters() const;
	double DistanceToTileMeters(int32 TileColumn, int32 TileRow, const FVector& ViewMeters) const;

	void DropDistantTiles(const FVector& ViewMeters);
	void GatherTiles(const FVector& ViewMeters);
	void BuildRequestedTiles();
	void DrawTile(const FTileRequest& Request);

	int32 AcquireComponent();
	void ReleaseComponent(FDrawnTile& Tile);

	TSharedPtr<const FMassifEnvironmentState> Environment;

	UPROPERTY()
	TObjectPtr<UStaticMesh> RockMesh;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> RockMaterial;

	UPROPERTY()
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> ComponentPool;

	TArray<int32> FreeComponentIndices;
	TMap<uint64, FDrawnTile> DrawnTiles;
	TArray<FTileRequest> TileRequests;
};
