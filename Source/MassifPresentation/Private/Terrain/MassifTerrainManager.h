#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassifTerrainManager.generated.h"

class FMassifTerrainField;
class UDynamicMeshComponent;
class UMaterialInterface;

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

	TSharedPtr<const FMassifTerrainField> Field;
	TSharedPtr<FFinishedBuildQueue> FinishedBuilds;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> TerrainMaterial;

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

	int32 AcquireComponent();
	void ReleaseComponent(FQuadNode& Node);
	void SetNodeVisibility(FQuadNode& Node, bool bVisible);
};
