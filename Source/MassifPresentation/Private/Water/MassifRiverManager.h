#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "GameFramework/Actor.h"
#include "MassifRiverManager.generated.h"

class FMassifRiverField;
class UDynamicMeshComponent;
class UMaterialInterface;

namespace UE::Geometry
{
	class FDynamicMesh3;
}

/**
 * Draws the world's rivers as water surfaces, a tile of the world at a time,
 * around wherever the camera is.
 *
 * Tiles are resident rather than built once: three hundred kilometres of channel
 * cut finely enough to hold a brook's shape is more geometry than the world needs
 * on screen at any moment, and almost all of it is somewhere else. Distant water
 * is not drawn at all — the valley it runs in is still carved into the terrain,
 * which is what reads at that range.
 *
 * There is no level of detail. The river field answers the same surface altitude
 * at a point however finely it is asked, so a tile is one mesh rather than a
 * choice between several, and a tile leaving and returning brings back exactly
 * what it had.
 *
 * Purely presentation: owns no gameplay state, and reads the field without
 * writing to it.
 */
UCLASS()
class AMassifRiverManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifRiverManager();

	void Initialize(TSharedPtr<const FMassifRiverField> InRiverField, UMaterialInterface* InRiverMaterial);

	virtual void Tick(float DeltaSeconds) override;

private:
	/**
	 * How far from the camera water is drawn. Past it a channel is a few pixels
	 * across and the carved valley carries the reading on its own.
	 */
	static constexpr double VisibleRadiusMeters = 1200.0;

	/** Tiles are dropped past a tenth beyond the radius, so a camera on the boundary does not rebuild one every frame. */
	static constexpr double DropRadiusFactor = 1.1;

	static constexpr int32 MaxInFlightBuilds = 6;
	static constexpr int32 MaxMeshApplicationsPerFrame = 4;

	struct FWaterTile
	{
		int32 TileX = 0;
		int32 TileY = 0;
		bool bBuildInFlight = false;
		/** Water covers a fraction of the world, so most tiles resolve to no mesh at all. */
		int32 ComponentIndex = INDEX_NONE;
	};

	struct FFinishedBuild
	{
		uint64 TileKey = 0;
		TSharedPtr<UE::Geometry::FDynamicMesh3> Mesh;
	};

	using FFinishedBuildQueue = TQueue<FFinishedBuild, EQueueMode::Mpsc>;

	struct FBuildRequest
	{
		int32 TileX = 0;
		int32 TileY = 0;
		double DistanceMeters = 0.0;
	};

	static uint64 MakeTileKey(int32 TileX, int32 TileY);
	static double DistanceToTileMeters(int32 TileX, int32 TileY, const FVector& ViewMeters);

	FVector GetViewLocationMeters() const;

	void GatherTiles(const FVector& ViewMeters);
	void DropDistantTiles(const FVector& ViewMeters);
	void LaunchBuilds();
	void ApplyFinishedBuilds();
	void ApplyMesh(FWaterTile& Tile, TSharedPtr<UE::Geometry::FDynamicMesh3> Mesh);

	int32 AcquireComponent(int32 TileX, int32 TileY);
	void ReleaseComponent(FWaterTile& Tile);

	/** Once the water around the camera has settled, so what it costs is visible at all. */
	void LogWaterSummaryOnce();

	/**
	 * Takes every tile down when the day of the year moves.
	 *
	 * The whole mesh changes, not just its altitude: a lower river is a narrower
	 * one, so the boundary the tile was cut along has moved and there is nothing
	 * in the old geometry worth keeping. Tiles still building are left to finish
	 * and land on a key nothing is holding, which drops them.
	 */
	void RebuildOnSeasonChange();

	/** Half-month of the year the water is drawn at, from massif.Water.DayOfYear. */
	int32 DrawnTimeSlice = 0;

	TSharedPtr<const FMassifRiverField> RiverField;
	TSharedPtr<FFinishedBuildQueue> FinishedBuilds;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> RiverMaterial;

	UPROPERTY()
	TArray<TObjectPtr<UDynamicMeshComponent>> ComponentPool;

	TArray<int32> FreeComponentIndices;
	TMap<uint64, FWaterTile> ResidentTiles;
	TArray<FBuildRequest> BuildRequests;
	int32 InFlightBuildCount = 0;
	bool bLoggedWaterSummary = false;
};
