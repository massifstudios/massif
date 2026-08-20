#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "MassifGameMode.generated.h"

class AMassifDebugChannelView;
class AMassifFoliageManager;
class AMassifRiverManager;
class AMassifTerrainManager;
class FMassifEnvironmentBake;
class FMassifHeightmap;
class FMassifRiverField;
class FMassifTerrainField;

/**
 * New-game flow for the winter core: loads the elevation model named by
 * massif.Dem into the terrain field, spawns the terrain and foliage managers,
 * and places the spectator above the massif.
 */
UCLASS()
class AMassifGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AMassifGameMode();

	virtual void BeginPlay() override;

	/** The loaded terrain, for the walker to stand on. Null until the elevation model is read. */
	TSharedPtr<const FMassifTerrainField> GetTerrainField() const
	{
		return TerrainField;
	}

private:
	void LoadTerrainAndSpawnManagers();

	/** Solves water over the whole world, which is what habitat and foliage read (ARCHITECTURE 5.6). */
	void BuildEnvironmentBake(const FMassifHeightmap& Heightmap);

	/** The elevation model with the bake's rivers cut into it. Needs the bake to have run. */
	void BuildTerrainField(TSharedRef<const FMassifHeightmap> Heightmap);
	void SpawnEnvironmentActors();
	void SpawnSunAndSky();
	void SpawnAtmosphericHaze();
	void SpawnPostProcess();
	void ApplyEarthGravity();
	void ConfigureFlyPawn();
	void ScheduleDebugScreenshot();

	/** Poses the spectator, then shoots a few seconds later once tiles have streamed in. */
	void SchedulePosedScreenshot(float PoseDelaySeconds, FVector LocationCentimeters, FRotator Rotation, FString ShotName);

	TSharedPtr<const FMassifTerrainField> TerrainField;
	TSharedPtr<const FMassifEnvironmentBake> EnvironmentBake;

	/** The bake's rivers as a field: cut into the terrain, and drawn as water over it. */
	TSharedPtr<const FMassifRiverField> RiverField;

	UPROPERTY()
	TObjectPtr<AMassifTerrainManager> TerrainManager;

	UPROPERTY()
	TObjectPtr<AMassifFoliageManager> FoliageManager;

	UPROPERTY()
	TObjectPtr<AMassifRiverManager> RiverManager;

	UPROPERTY()
	TObjectPtr<AMassifDebugChannelView> DebugChannelView;

	float SpawnAltitudeMeters = 0.0f;
};
