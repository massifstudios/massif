#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "MassifWorldBuild.h"
#include "MassifGameMode.generated.h"

class AMassifDebugChannelView;
class AMassifDaylightManager;
class AMassifFogManager;
class AMassifFoliageManager;
class AMassifRockManager;
class AMassifWaterBodyManager;
class AMassifTerrainManager;
class FMassifEnvironmentScheduler;
class FMassifEnvironmentState;
class FMassifObjectStore;
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

	/**
	 * Solves the world from the seed and spawns everything that draws it.
	 *
	 * Called by the main menu rather than by BeginPlay, so a run opens on the menu
	 * in a second instead of on a solve nobody asked for.
	 */
	void StartNewGame();

	/** Same build, with the seed and the objects coming off the save instead. */
	void LoadGame(const FString& SaveName);

	/** Writes the world as it stands. False fills OutError with something the UI can show. */
	bool SaveGame(const FString& SaveName, FString& OutError);

	/** Whether there is a world to save. */
	bool HasWorld() const
	{
		return TerrainField.IsValid();
	}

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

	/** The traced rivers, for the debug overlay that draws them. Null until world gen has run. */
	TSharedPtr<const FMassifRiverNetwork> GetRiverNetwork() const
	{
		return RiverNetwork;
	}

private:
	/** Puts the simulation on the built world and starts it ticking, so the wall clock runs. */
	void StartSimulation(FMassifWorldBuildResult& Result);

	/** Hands the solve to a worker. The game thread keeps ticking, so the loading screen moves. */
	void BeginWorldBuild(const FString& SaveName);

	/** Takes the solved world back on the game thread and spawns everything that draws it. */
	void FinishWorldBuild(FMassifWorldBuildResult Result);

	void SpawnWorldActors(TSharedRef<const FMassifHabitatField> HabitatField);

	/** Drops everything that reads the old terrain, so a second world does not stack on the first. */
	void TearDownWorld();

	/** Honours -MassifSaveAfter, which is how the save path is exercised without hands. */
	void WriteRequestedSave();

	void LogEnvironmentSummary(const FMassifWorldBuildResult& Result) const;
	void LogRockfallSummary(const FMassifEnvironmentState& Environment, bool bFromSave) const;
	void LogRiverSummary(const FMassifRiverNetwork& Rivers) const;
	void LogObjectStoreSummary(const FMassifWorldBuildResult& Result) const;

	/** Puts the baked fluid region on the largest channel the world traced. */
	void SpawnWaterBodies();
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

	/** The solved rivers as a field, which is what the terrain is cut against. */
	TSharedPtr<const FMassifRiverField> RiverField;

	/** The same rivers as reaches and stations, which is what the water bodies are built from. */
	TSharedPtr<const FMassifRiverNetwork> RiverNetwork;

	/** Every generated object, drained out of the bake's point sets once the graph has run. */
	TSharedPtr<const FMassifObjectStore> ObjectStore;

	UPROPERTY()
	TObjectPtr<AMassifTerrainManager> TerrainManager;

	UPROPERTY()
	TObjectPtr<AMassifFoliageManager> FoliageManager;

	UPROPERTY()
	TObjectPtr<AMassifRockManager> RockManager;

	UPROPERTY()
	TObjectPtr<AMassifWaterBodyManager> WaterBodyManager;

	UPROPERTY()
	TObjectPtr<AMassifDebugChannelView> DebugChannelView;

	UPROPERTY()
	TObjectPtr<AMassifFogManager> FogManager;

	UPROPERTY()
	TObjectPtr<AMassifDaylightManager> DaylightManager;

	/** Kept so the daylight manager can aim it where the clock says the sun is. */
	UPROPERTY()
	TObjectPtr<ADirectionalLight> SunLight;

	/** Shared with the worker and with the loading screen reading it. Null when no build is running. */
	TSharedPtr<FMassifWorldBuildProgress> BuildProgress;

	/** What a save written from this world records about the ground it stands on. */
	FString ElevationModelName;
	uint64 ElevationModelHash = 0;
	uint32 MasterSeed = 0;

	float SpawnAltitudeMeters = 0.0f;
};
