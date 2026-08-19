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
#include "Foliage/MassifFoliageManager.h"
#include "MassifHeightmap.h"
#include "MassifTerrainField.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Terrain/MassifTerrainManager.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	TAutoConsoleVariable<int32> CVarMassifSeed(
		TEXT("massif.Seed"), 1337,
		TEXT("Master seed for scattered content. Takes effect on level restart."));

	TAutoConsoleVariable<FString> CVarMassifDem(
		TEXT("massif.Dem"), TEXT("oetztal"),
		TEXT("Elevation model under Data/Terrain, without the .massifdem suffix."));

	/** Gap between posing the spectator and shooting, so the quadtree can stream the new view in. */
	constexpr float DebugScreenshotSettleSeconds = 3.0f;

	/** Where the close debug shot stands, how far above the ground, and which way it faces. */
	const FVector2d GroundShotCenterMeters(-3000.0, -3000.0);
	constexpr double GroundShotEyeHeightMeters = 40.0;
	const FRotator GroundShotRotation(-8.0f, 45.0f, 0.0f);

	/** -MassifScreenshotAfter=N: debug screenshot after N seconds, then quit. Command-line
	 *  arg instead of a cvar because -ExecCmds runs only after GameMode::BeginPlay. */
	float GetAutoScreenshotDelaySeconds()
	{
		float DelaySeconds = 0.0f;
		FParse::Value(FCommandLine::Get(), TEXT("MassifScreenshotAfter="), DelaySeconds);
		return DelaySeconds;
	}

	uint32 GetMasterSeed()
	{
		int32 MasterSeed = CVarMassifSeed.GetValueOnGameThread();
		FParse::Value(FCommandLine::Get(), TEXT("MassifSeed="), MasterSeed);
		return static_cast<uint32>(MasterSeed);
	}

	FString GetHeightmapPath()
	{
		FString DemName = CVarMassifDem.GetValueOnGameThread();
		FParse::Value(FCommandLine::Get(), TEXT("MassifDem="), DemName);
		return FPaths::ProjectDir() / TEXT("Data/Terrain") / DemName + TEXT(".massifdem");
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
	LoadTerrainAndSpawnManagers();
	SpawnEnvironmentActors();
	ConfigureSpectatorPawn();
	ScheduleDebugScreenshot();
}

void AMassifGameMode::LoadTerrainAndSpawnManagers()
{
	const FString HeightmapPath = GetHeightmapPath();
	const double StartSeconds = FPlatformTime::Seconds();

	FString LoadError;
	TSharedPtr<const FMassifHeightmap> Heightmap = FMassifHeightmap::LoadFromFile(HeightmapPath, LoadError);
	if (!Heightmap.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Terrain: %s. Fetch one with Tools/fetch_tirol_dem.py."), *LoadError);
		return;
	}

	TerrainField = MakeShared<const FMassifTerrainField>(Heightmap.ToSharedRef());
	UE_LOG(LogTemp, Display, TEXT("Terrain: %s, %.0f m across, altitude %.0f-%.0f m, %d mips, loaded in %.2f s"),
		*Heightmap->GetSourceInfo().Label, TerrainField->GetWorldSizeMeters(),
		TerrainField->GetMinAltitudeMeters(), TerrainField->GetMaxAltitudeMeters(),
		Heightmap->GetMipCount(), FPlatformTime::Seconds() - StartSeconds);

	SpawnAltitudeMeters = static_cast<float>(TerrainField->GetMaxAltitudeMeters()) + 2200.0f;

	TerrainManager = GetWorld()->SpawnActor<AMassifTerrainManager>();
	TerrainManager->Initialize(TerrainField, LoadTerrainMaterial());

	FoliageManager = GetWorld()->SpawnActor<AMassifFoliageManager>();
	FoliageManager->Initialize(TerrainField, GetMasterSeed());
}

void AMassifGameMode::SchedulePosedScreenshot(
	float PoseDelaySeconds, FVector LocationCentimeters, FRotator Rotation, FString ShotName)
{
	FTimerHandle PoseTimer;
	GetWorldTimerManager().SetTimer(PoseTimer, [this, LocationCentimeters, Rotation]()
	{
		APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
		PlayerController->GetPawn()->SetActorLocation(LocationCentimeters);
		PlayerController->SetControlRotation(Rotation);
	}, PoseDelaySeconds, false);

	FTimerHandle ShotTimer;
	GetWorldTimerManager().SetTimer(ShotTimer, [ShotName]()
	{
		FScreenshotRequest::RequestScreenshot(ShotName, false, false);
	}, PoseDelaySeconds + DebugScreenshotSettleSeconds, false);
}

void AMassifGameMode::ScheduleDebugScreenshot()
{
	const float DelaySeconds = GetAutoScreenshotDelaySeconds();
	if (DelaySeconds <= 0.0f || !TerrainField.IsValid())
	{
		return;
	}

	FTimerHandle ProfileShotTimer;
	GetWorldTimerManager().SetTimer(ProfileShotTimer, []()
	{
		FScreenshotRequest::RequestScreenshot(TEXT("MassifShot_Profile"), false, false);
	}, DelaySeconds, false);

	SchedulePosedScreenshot(DelaySeconds + 3.0f,
		FVector(0.0, 0.0, (SpawnAltitudeMeters + 2800.0) * 100.0),
		FRotator(-55.0f, 90.0f, 0.0f), TEXT("MassifShot_Overview"));

	// Eye level on the ground: the only pose that shows texture tiling, layer
	// blending and foliage at the scale a player will actually see them.
	const double GroundAltitudeMeters = TerrainField->SampleHeightMeters(GroundShotCenterMeters, 1.0);
	SchedulePosedScreenshot(DelaySeconds + 9.0f,
		FVector(GroundShotCenterMeters.X, GroundShotCenterMeters.Y, GroundAltitudeMeters + GroundShotEyeHeightMeters) * 100.0,
		GroundShotRotation, TEXT("MassifShot_Ground"));

	FTimerHandle QuitTimer;
	GetWorldTimerManager().SetTimer(QuitTimer, [this]()
	{
		GetWorld()->GetFirstPlayerController()->ConsoleCommand(TEXT("quit"));
	}, DelaySeconds + 15.0f, false);
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
		// 600 m/s crosses the 20 km world in about half a minute.
		Movement->MaxSpeed = 60000.0f;
		Movement->Acceleration = 120000.0f;
		Movement->Deceleration = 240000.0f;
	}
}
