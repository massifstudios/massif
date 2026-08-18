#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WorldGenResult.h"
#include "MassifTerrainManager.generated.h"

class UDynamicMeshComponent;
class UMaterialInterface;

namespace UE::Geometry
{
	class FDynamicMesh3;
}

/**
 * Builds and streams the visual terrain from FWorldGenResult as a grid of
 * dynamic mesh chunks with distance-based LOD. Mesh generation runs on the
 * thread pool; finished meshes are applied on the game thread.
 * Purely presentation: owns no gameplay state.
 */
UCLASS()
class AMassifTerrainManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifTerrainManager();

	void Initialize(TSharedPtr<const FWorldGenResult> InWorldGen, UMaterialInterface* InTerrainMaterial);

	virtual void Tick(float DeltaSeconds) override;

private:
	struct FChunkState
	{
		FVector2f CenterMeters = FVector2f::ZeroVector;
		int32 CurrentStep = 0;
		int32 PendingStep = 0;
	};

	struct FFinishedBuild
	{
		int32 ChunkIndex = INDEX_NONE;
		int32 Step = 0;
		TSharedPtr<UE::Geometry::FDynamicMesh3> Mesh;
	};

	using FFinishedBuildQueue = TQueue<FFinishedBuild, EQueueMode::Mpsc>;

	static constexpr int32 ChunkQuadCount = 256;
	static constexpr int32 MaxInFlightBuilds = 8;
	static constexpr int32 MaxMeshApplicationsPerFrame = 4;

	TSharedPtr<const FWorldGenResult> WorldGen;
	TSharedPtr<FFinishedBuildQueue> FinishedBuilds;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> TerrainMaterial;

	UPROPERTY()
	TArray<TObjectPtr<UDynamicMeshComponent>> ChunkComponents;

	TArray<FChunkState> Chunks;
	int32 ChunkCountPerSide = 0;
	float ChunkSizeMeters = 0.0f;
	int32 InFlightBuildCount = 0;

	void CreateChunkGrid();
	FVector GetViewLocation() const;
	int32 DesiredStepForDistance(float DistanceMeters) const;
	void LaunchLodBuilds();
	void LaunchBuild(int32 ChunkIndex, int32 Step);
	void ApplyFinishedBuilds();
};
