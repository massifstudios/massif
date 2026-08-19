#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "WorldGenConfig.h"
#include "MassifGameMode.generated.h"

class AMassifTerrainManager;
class FMassifTerrainField;

/**
 * New-game flow for the winter core: generates the world from the seed
 * console variables (massif.Seed, massif.WorldGenResolution), spawns the
 * terrain manager, and places the spectator above the massif.
 *
 * Also owns the live-tuning loop: the massif.Gully.* console variables feed
 * the erosion-filter config, and a change rebuilds the terrain field;
 * massif.Regenerate rebuilds it picking up massif.Seed. Dev tooling only —
 * the field itself stays a pure function of its config.
 */
UCLASS()
class AMassifGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AMassifGameMode();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	void GenerateWorldAndSpawnTerrain();
	void ExportWorldGenDebug(const FWorldGenConfig& Config);
	void SpawnEnvironmentActors();
	void ConfigureSpectatorPawn();
	void ScheduleDebugScreenshot();

	/** Poses the spectator, then shoots a few seconds later once tiles have streamed in. */
	void SchedulePosedScreenshot(float PoseDelaySeconds, FVector LocationCentimeters, FRotator Rotation, FString ShotName);

	/** Config assembled from the console variables and command line. */
	FWorldGenConfig MakeWorldGenConfig() const;

	/** Debounced gully-knob watcher; rebuilds the field once the knobs settle. */
	void PollGullyKnobs();
	void RebuildTerrainField(const FWorldGenConfig& Config);

	/** F1: slider overlay over the massif.* console variables. */
	void ToggleTuningPanel();

	TSharedPtr<const FMassifTerrainField> TerrainField;

	UPROPERTY()
	TObjectPtr<AMassifTerrainManager> TerrainManager;

	FWorldGenConfig PendingKnobConfig;
	double PendingKnobsSinceSeconds = -1.0;

	TSharedPtr<SWidget> TuningPanel;
	bool bTuningPanelVisible = false;

	float SpawnAltitudeMeters = 0.0f;
};
