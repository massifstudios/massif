#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassifTerrainManager.generated.h"

class FMassifTerrainField;
class UDynamicMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UTexture2D;

namespace UE::Geometry
{
	class FDynamicMesh3;
}

/**
 * Renders the terrain field as a quadtree of dynamic mesh tiles. The root tile
 * covers the whole world; every tile near the camera splits into four until
 * the vertex spacing under the camera reaches centimetre scale. Each tile is
 * one component, so engine frustum culling applies per tile.
 *
 * Splits and merges swap only when the replacing meshes are built, so the
 * ground never has holes: a parent stays visible until all four children are
 * ready, and children stay visible until the parent is. Mesh builds run on
 * the thread pool and sample the field directly — nothing is materialised
 * beyond the tiles currently on screen.
 * Purely presentation: owns no gameplay state.
 */
UCLASS()
class AMassifTerrainManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifTerrainManager();

	void Initialize(TSharedPtr<const FMassifTerrainField> InField, UMaterialInterface* InTerrainMaterial);

	/**
	 * Swap in a rebuilt field and regrow the quadtree from the root, so the
	 * ground reflects the new field coarse-to-fine. Results of tile builds still
	 * in flight against the old field are dropped, never applied to the new tree.
	 */
	void ResetField(TSharedPtr<const FMassifTerrainField> InField);

	/**
	 * Paints a baked channel over the ground in place of the terrain material
	 * (ARCHITECTURE §5.7). One texture covers the world and is sampled by world
	 * XY, so no mesh, tile or field is touched by turning a view on.
	 */
	void SetDebugChannel(UTexture2D* ChannelTexture);
	void ClearDebugChannel();

	virtual void Tick(float DeltaSeconds) override;

private:
	struct FQuadNode
	{
		uint64 Key = 0;
		FVector2d CenterMeters = FVector2d::ZeroVector;
		double SizeMeters = 0.0;
		int32 Depth = 0;
		/** Coarse ground altitude at the center, for view distance only. */
		double CenterAltitudeMeters = 0.0;
		bool bMeshReady = false;
		bool bBuildInFlight = false;
		/** Whether this tile is currently the one drawn for its area. */
		bool bRendered = false;
		bool bChildrenActive = false;
		/** Whether this tile's mesh is currently cooked as collision for the player to stand on. */
		bool bCollisionEnabled = false;
		int32 ComponentIndex = INDEX_NONE;
		TUniquePtr<FQuadNode> Children[4];
	};

	struct FFinishedBuild
	{
		uint64 NodeKey = 0;
		TSharedPtr<UE::Geometry::FDynamicMesh3> Mesh;
	};

	using FFinishedBuildQueue = TQueue<FFinishedBuild, EQueueMode::Mpsc>;

	struct FBuildRequest
	{
		FQuadNode* Node = nullptr;
		double DistanceMeters = 0.0;
	};

	static constexpr int32 TileQuadCount = 64;
	static constexpr double FinestVertexSpacingMeters = 0.25;
	/** Split under twice the tile size, merge above; the gap keeps tiles from flickering at the boundary. */
	static constexpr double SplitDistanceFactor = 1.8;
	static constexpr double MergeDistanceFactor = 2.2;
	static constexpr int32 MaxInFlightBuilds = 8;
	static constexpr int32 MaxMeshApplicationsPerFrame = 4;

	/**
	 * Tiles this close to the view are cooked as collision so the walker has
	 * ground under it. Only the fine tiles are worth cooking: a coarse one
	 * covers kilometres, and by the time the player reaches it the quadtree has
	 * split it into tiles that are.
	 */
	static constexpr double CollisionRadiusMeters = 150.0;
	static constexpr double MaxCollisionTileSizeMeters = 80.0;
	/** Cooking is not free, so tiles enter and leave collision a couple at a time. */
	static constexpr int32 MaxCollisionUpdatesPerFrame = 2;

	TSharedPtr<const FMassifTerrainField> Field;
	TSharedPtr<FFinishedBuildQueue> FinishedBuilds;

	/**
	 * Every tile is drawn with one shared instance, so a debug channel is set
	 * once rather than on each of the hundreds of components the quadtree holds.
	 */
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> TerrainMaterial;

	UPROPERTY()
	TArray<TObjectPtr<UDynamicMeshComponent>> ComponentPool;

	TArray<int32> FreeComponentIndices;
	TUniquePtr<FQuadNode> RootNode;
	TMap<uint64, FQuadNode*> LiveNodes;
	TArray<FBuildRequest> BuildRequests;
	int32 MaxTreeDepth = 0;
	int32 InFlightBuildCount = 0;

	FVector GetViewLocationMeters() const;
	double DistanceToNodeMeters(const FQuadNode& Node, const FVector& ViewMeters) const;

	TUniquePtr<FQuadNode> MakeNode(int32 Depth, int32 GridX, int32 GridY);
	void EnsureChildren(FQuadNode& Node);
	bool AreAllChildrenReady(const FQuadNode& Node) const;
	void ActivateChildren(FQuadNode& Node);
	void CollapseChildren(FQuadNode& Node);
	void DestroySubtree(FQuadNode& Node);

	/** Walks the active tree, swapping split/merge states and collecting mesh builds to request. */
	void UpdateNode(FQuadNode& Node, const FVector& ViewMeters);
	void RequestBuild(FQuadNode& Node, double DistanceMeters);
	void LaunchBuilds();
	void ApplyFinishedBuilds();

	/** Cooks collision for the tiles around the view and drops it from the ones left behind. */
	void UpdateTileCollision(const FVector& ViewMeters);
	bool ApplyNodeCollision(FQuadNode& Node, const FVector& ViewMeters);
	void SetNodeCollision(FQuadNode& Node, bool bEnabled);

	/** Maps world metres onto the debug texture's 0-1 range, for the material's own sampling. */
	void ApplyDebugChannelMapping();

	int32 AcquireComponent();
	void ReleaseComponent(FQuadNode& Node);
	void SetNodeVisibility(FQuadNode& Node, bool bVisible);
};
