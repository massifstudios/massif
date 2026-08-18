#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "MassifGameMode.generated.h"

/**
 * New-game flow for the winter core: generates the world from the seed
 * console variables (massif.Seed, massif.WorldGenResolution), spawns the
 * terrain manager, and places the spectator above the massif.
 */
UCLASS()
class AMassifGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AMassifGameMode();

	virtual void BeginPlay() override;

private:
	void GenerateWorldAndSpawnTerrain();
	void SpawnEnvironmentActors();
	void ConfigureSpectatorPawn();
	void ScheduleDebugScreenshot();

	float SpawnAltitudeMeters = 0.0f;
};
