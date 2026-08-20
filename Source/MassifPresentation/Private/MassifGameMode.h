#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "MassifGameMode.generated.h"

class AMassifFoliageManager;
class AMassifTerrainManager;
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

	UPROPERTY()
	TObjectPtr<AMassifTerrainManager> TerrainManager;

	UPROPERTY()
	TObjectPtr<AMassifFoliageManager> FoliageManager;

	float SpawnAltitudeMeters = 0.0f;
};
