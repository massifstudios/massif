#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassifRockManager.generated.h"

class FMassifObjectStore;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;
struct FMassifObjectPlacement;

namespace MassifRockPlacement
{
	/** Radius of the stand-in sphere in centimetres, which a block's own radius scales against. */
	inline constexpr double PlaceholderRadiusCentimeters = 50.0;

	/** How much flatter than a ball a block is drawn, so a scree slope does not read as marbles. */
	inline constexpr double BlockFlattening = 0.62;

	/**
	 * Where one stored block is drawn, in centimetres.
	 *
	 * The store puts a block's position on the ground it came to rest on, and how
	 * far into that ground it settled is carried separately, so the mesh has to be
	 * lifted by what stands proud of the surface rather than centred on the point.
	 */
	FTransform MakeBlockTransform(const FMassifObjectPlacement& Block);
}

/**
 * Draws the boulders the object store holds, a chunk at a time within reach of
 * the camera, and drops them again when it leaves.
 *
 * Unlike the foliage beside it this scatters nothing and decides nothing. Where
 * each boulder sits was settled by the rockfall pass and written into the store
 * (OBJECT_STORE.md §5), so this reads records and turns metres into transforms.
 *
 * It draws the boulder tier only. A chunk holds every stored object in its
 * square, and each reader takes the categories it knows how to draw.
 */
UCLASS()
class AMassifRockManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifRockManager();

	void Initialize(TSharedPtr<const FMassifObjectStore> InObjectStore);

	/** Drops every drawn chunk and reads a new store, as the foliage does on a rebuild. */
	void ResetObjectStore(TSharedPtr<const FMassifObjectStore> InObjectStore);

	virtual void Tick(float DeltaSeconds) override;

private:
	/** One chunk's boulders, and the component they live in. */
	struct FDrawnChunk
	{
		int32 ChunkColumn = 0;
		int32 ChunkRow = 0;
		int32 ComponentIndex = INDEX_NONE;
	};

	struct FChunkRequest
	{
		int32 ChunkColumn = 0;
		int32 ChunkRow = 0;
		double DistanceMeters = 0.0;
	};

	/**
	 * How far boulders are drawn. Shorter than the forest: a half-metre block is
	 * not worth a draw call at a kilometre, and the deposit channel is what paints
	 * distant scree onto the terrain material instead.
	 */
	static constexpr double VisibleRadiusMeters = 700.0;

	/** Spread the cost of moving, as the foliage manager does. */
	static constexpr int32 MaxChunkBuildsPerFrame = 8;

	void LoadRockMesh();
	void DropAllChunks();

	FVector GetViewLocationMeters() const;
	double DistanceToChunkMeters(int32 ChunkColumn, int32 ChunkRow, const FVector& ViewMeters) const;

	void DropDistantChunks(const FVector& ViewMeters);
	void GatherChunks(const FVector& ViewMeters);
	void BuildRequestedChunks();
	void DrawChunk(const FChunkRequest& Request);

	int32 AcquireComponent();
	void ReleaseComponent(FDrawnChunk& Chunk);

	TSharedPtr<const FMassifObjectStore> ObjectStore;

	UPROPERTY()
	TObjectPtr<UStaticMesh> RockMesh;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> RockMaterial;

	UPROPERTY()
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> ComponentPool;

	TArray<int32> FreeComponentIndices;
	TMap<uint64, FDrawnChunk> DrawnChunks;
	TArray<FChunkRequest> ChunkRequests;
};
