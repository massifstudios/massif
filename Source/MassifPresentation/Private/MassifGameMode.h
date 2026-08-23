#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "MassifGameMode.generated.h"

class AMassifDebugChannelView;
class AMassifDaylightManager;
class AMassifFogManager;
class AMassifFoliageManager;
class AMassifShallowWaterRegion;
class AMassifRiverManager;
class AMassifTerrainManager;
class FMassifEnvironmentScheduler;
class FMassifHeightmap;
class FMassifRiverField;
class FMassifRiverNetwork;
class FMassifTerrainField;
class ADirectionalLight;

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

	/** The solved environment, for the debug inspector and the fidelity commands. Null until it is built. */
	TSharedPtr<FMassifEnvironmentScheduler> GetEnvironmentScheduler() const
	{
		return EnvironmentScheduler;
	}

	/** The loaded terrain, for the walker to stand on. Null until the elevation model is read. */
	TSharedPtr<const FMassifTerrainField> GetTerrainField() const
	{
		return TerrainField;
	}

private:
	/** Puts the simulation on the master seed and starts it ticking, so the wall clock runs. */
	void StartSimulation();

	void LoadTerrainAndSpawnManagers();

	/**
	 * Solves water over the whole world, which is what habitat and foliage read
	 * (ARCHITECTURE 5.6). False where the graph refused to schedule, in which case
	 * nothing downstream of it is built.
	 */
	bool BuildEnvironment(TSharedRef<const FMassifHeightmap> Heightmap);

	void LogEnvironmentSummary(double SolveSeconds) const;
	void LogRiverSummary(const FMassifRiverNetwork& Rivers) const;

	/** The elevation model with the solved rivers cut into it. Needs the environment to have run. */
	void BuildTerrainField(TSharedRef<const FMassifHeightmap> Heightmap);

	/** Puts the baked fluid region on the largest channel the world traced. */
	void SpawnShallowWaterRegion(const FMassifRiverNetwork& Rivers);
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

	/** Kept rather than only its state: fidelities are switched on it while the world is up. */
	TSharedPtr<FMassifEnvironmentScheduler> EnvironmentScheduler;

	/** The solved rivers as a field: cut into the terrain, and drawn as water over it. */
	TSharedPtr<const FMassifRiverField> RiverField;

	UPROPERTY()
	TObjectPtr<AMassifTerrainManager> TerrainManager;

	UPROPERTY()
	TObjectPtr<AMassifFoliageManager> FoliageManager;

	UPROPERTY()
	TObjectPtr<AMassifRiverManager> RiverManager;

	UPROPERTY()
	TObjectPtr<AMassifShallowWaterRegion> ShallowWaterRegion;

	UPROPERTY()
	TObjectPtr<AMassifDebugChannelView> DebugChannelView;

	UPROPERTY()
	TObjectPtr<AMassifFogManager> FogManager;

	UPROPERTY()
	TObjectPtr<AMassifDaylightManager> DaylightManager;

	/** Kept so the daylight manager can aim it where the clock says the sun is. */
	UPROPERTY()
	TObjectPtr<ADirectionalLight> SunLight;

	float SpawnAltitudeMeters = 0.0f;
};
