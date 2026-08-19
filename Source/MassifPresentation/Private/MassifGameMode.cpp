#include "MassifGameMode.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/SpectatorPawn.h"
#include "MassifTerrainField.h"
#include "Materials/MaterialInterface.h"
#include "Terrain/MassifTerrainManager.h"
#include "UObject/SoftObjectPath.h"
#include "WorldGenDebugExport.h"
#include "WorldGenPipeline.h"

namespace
{
	TAutoConsoleVariable<int32> CVarMassifSeed(
		TEXT("massif.Seed"), 1337,
		TEXT("Master seed for world generation. Takes effect on level restart."));

	TAutoConsoleVariable<int32> CVarMassifWorldGenResolution(
		TEXT("massif.WorldGenResolution"), 4097,
		TEXT("Heightfield samples per side; (value - 1) must be divisible by 256."));

	/** -MassifScreenshotAfter=N: debug screenshot after N seconds, then quit. Command-line
	 *  arg instead of a cvar because -ExecCmds runs only after GameMode::BeginPlay. */
	float GetAutoScreenshotDelaySeconds()
	{
		float DelaySeconds = 0.0f;
		FParse::Value(FCommandLine::Get(), TEXT("MassifScreenshotAfter="), DelaySeconds);
		return DelaySeconds;
	}

	UMaterialInterface* LoadTerrainMaterial()
	{
		const FSoftObjectPath MaterialPath(TEXT("/Game/Materials/M_Terrain.M_Terrain"));
		UMaterialInterface* Material = Cast<UMaterialInterface>(MaterialPath.TryLoad());
		if (Material != nullptr)
		{
			return Material;
		}

		UE_LOG(LogTemp, Warning, TEXT("M_Terrain missing, falling back to WorldGridMaterial"));
		return LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
	}
}

AMassifGameMode::AMassifGameMode()
{
	DefaultPawnClass = ASpectatorPawn::StaticClass();
}

void AMassifGameMode::BeginPlay()
{
	Super::BeginPlay();
	GenerateWorldAndSpawnTerrain();
	SpawnEnvironmentActors();
	ConfigureSpectatorPawn();
	ScheduleDebugScreenshot();
}

void AMassifGameMode::ScheduleDebugScreenshot()
{
	const float DelaySeconds = GetAutoScreenshotDelaySeconds();
	if (DelaySeconds <= 0.0f)
	{
		return;
	}

	FTimerHandle ProfileShotTimer;
	GetWorldTimerManager().SetTimer(ProfileShotTimer, []()
	{
		FScreenshotRequest::RequestScreenshot(TEXT("MassifShot_Profile"), false, false);
	}, DelaySeconds, false);

	FTimerHandle OverviewPoseTimer;
	GetWorldTimerManager().SetTimer(OverviewPoseTimer, [this]()
	{
		APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
		PlayerController->GetPawn()->SetActorLocation(FVector(0.0, 0.0, (SpawnAltitudeMeters + 2800.0) * 100.0));
		PlayerController->SetControlRotation(FRotator(-55.0f, 90.0f, 0.0f));
	}, DelaySeconds + 3.0f, false);

	FTimerHandle OverviewShotTimer;
	GetWorldTimerManager().SetTimer(OverviewShotTimer, []()
	{
		FScreenshotRequest::RequestScreenshot(TEXT("MassifShot_Overview"), false, false);
	}, DelaySeconds + 6.0f, false);

	FTimerHandle QuitTimer;
	GetWorldTimerManager().SetTimer(QuitTimer, [this]()
	{
		GetWorld()->GetFirstPlayerController()->ConsoleCommand(TEXT("quit"));
	}, DelaySeconds + 9.0f, false);
}

void AMassifGameMode::SpawnEnvironmentActors()
{
	UWorld* World = GetWorld();

	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>();
	Sun->SetMobility(EComponentMobility::Movable);
	Sun->SetActorRotation(FRotator(-40.0f, 25.0f, 0.0f));
	UDirectionalLightComponent* SunComponent = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
	SunComponent->SetAtmosphereSunLight(true);

	World->SpawnActor<ASkyAtmosphere>();

	ASkyLight* SkyLight = World->SpawnActor<ASkyLight>();
	SkyLight->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	SkyLight->GetLightComponent()->bRealTimeCapture = true;
	SkyLight->GetLightComponent()->MarkRenderStateDirty();

	World->SpawnActor<AExponentialHeightFog>();
}

void AMassifGameMode::ExportWorldGenDebug(const FWorldGenConfig& Config)
{
	FWorldGenResult Result = FWorldGenPipeline::Generate(Config);

	float MaxNeighborDeltaMeters = 0.0f;
	for (int32 Index = 0; Index + 1 < Result.HeightMeters.Num(); ++Index)
	{
		const bool bRowEnd = (Index % Result.Resolution) + 1 == Result.Resolution;
		if (!bRowEnd)
		{
			MaxNeighborDeltaMeters = FMath::Max(MaxNeighborDeltaMeters,
				FMath::Abs(Result.HeightMeters[Index + 1] - Result.HeightMeters[Index]));
		}
	}
	const int32 LocalMinimumCount = Result.CountLocalMinima();
	UE_LOG(LogTemp, Display, TEXT("WorldGen sanity: max neighbor height delta %.1f m, %d pits (%.3f%% of cells)"),
		MaxNeighborDeltaMeters, LocalMinimumCount,
		100.0f * static_cast<float>(LocalMinimumCount) / static_cast<float>(Result.HeightMeters.Num()));

	const FString ExportDirectory = FPaths::ProjectSavedDir() / TEXT("WorldGenDebug");
	const bool bExported = FWorldGenDebugExport::SaveHeightfieldPngs(Result, ExportDirectory);
	UE_LOG(LogTemp, Display, TEXT("WorldGen debug export %s: %s"),
		bExported ? TEXT("written to") : TEXT("FAILED for"), *ExportDirectory);
	FPlatformMisc::RequestExit(false);
}

void AMassifGameMode::GenerateWorldAndSpawnTerrain()
{
	FWorldGenConfig Config;
	Config.MasterSeed = CVarMassifSeed.GetValueOnGameThread();
	Config.Resolution = CVarMassifWorldGenResolution.GetValueOnGameThread();
	FParse::Value(FCommandLine::Get(), TEXT("MassifSeed="), Config.MasterSeed);

	if (FParse::Param(FCommandLine::Get(), TEXT("MassifExportWorldGen")))
	{
		ExportWorldGenDebug(Config);
		return;
	}

	const double FieldStartSeconds = FPlatformTime::Seconds();
	TSharedPtr<const FMassifTerrainField> Field = MakeShared<const FMassifTerrainField>(Config);
	UE_LOG(LogTemp, Display, TEXT("WorldGen: seed %d, terrain field over %.0f m built in %.2f s"),
		Config.MasterSeed, Config.WorldSizeMeters, FPlatformTime::Seconds() - FieldStartSeconds);

	SpawnAltitudeMeters = Config.MaxAltitudeMeters + 2200.0f;

	AMassifTerrainManager* TerrainManager = GetWorld()->SpawnActor<AMassifTerrainManager>();
	TerrainManager->Initialize(Field, LoadTerrainMaterial());
}

void AMassifGameMode::ConfigureSpectatorPawn()
{
	APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	if (PlayerController == nullptr || PlayerController->GetPawn() == nullptr)
	{
		return;
	}

	APawn* Pawn = PlayerController->GetPawn();
	Pawn->SetActorLocation(FVector(0.0, -300000.0, SpawnAltitudeMeters * 100.0));
	PlayerController->SetControlRotation(FRotator(-22.0f, 90.0f, 0.0f));

	UFloatingPawnMovement* Movement = Cast<UFloatingPawnMovement>(Pawn->GetMovementComponent());
	if (Movement != nullptr)
	{
		// 3 km/s, which crosses the 150 km world in under a minute. At the old
		// massif-sized speed getting anywhere took longer than looking at it.
		Movement->MaxSpeed = 300000.0f;
		Movement->Acceleration = 600000.0f;
		Movement->Deceleration = 1200000.0f;
	}
}
