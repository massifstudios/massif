#include "MassifGameMode.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Foliage/MassifFoliageManager.h"
#include "GameFramework/WorldSettings.h"
#include "MassifEnvironmentBake.h"
#include "MassifHeightmap.h"
#include "MassifTerrainField.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Player/MassifFlyPawn.h"
#include "Player/MassifPlayerController.h"
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

	/** Gap between posing the camera and shooting, so the quadtree can stream the new view in. */
	constexpr float DebugScreenshotSettleSeconds = 3.0f;

	/** Earth's surface gravity, 9.81 m/s down. */
	constexpr float EarthGravityCentimetersPerSecondSquared = -981.0f;

	/** Mid-morning sun: low enough that ridges cast shadows down their own flanks. */
	const FRotator SunRotation(-30.0f, 25.0f, 0.0f);

	/**
	 * Haze settings for a massif twenty kilometres across. The engine defaults
	 * fall off over a few hundred metres of height, which puts the whole
	 * altitude range inside the densest part of the fog; these spread the same
	 * amount of haze over the kilometres the valleys actually span, and hold it
	 * off the ground the camera stands on.
	 */
	constexpr float FogDensity = 0.015f;
	constexpr float FogHeightFalloffPerCentimeter = 0.02f;
	constexpr float FogStartDistanceCentimeters = 20000.0f;
	constexpr float FogMaxOpacity = 0.85f;

	/**
	 * Exposure compensation and the band the histogram may drift inside. A
	 * snowfield fills the frame with high albedo, so metering to mid grey
	 * overexposes everything else; the bias pulls it back and the band keeps
	 * the image from swinging as the camera pans between sky and valley.
	 */
	constexpr float ExposureBias = -1.25f;
	constexpr float ExposureMinBrightness = 0.4f;
	constexpr float ExposureMaxBrightness = 4.0f;

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

	const TCHAR* DescribeBakePath(bool bBuiltOnGpu)
	{
		if (bBuiltOnGpu)
		{
			return TEXT("GPU");
		}
		return TEXT("CPU");
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
	DefaultPawnClass = AMassifFlyPawn::StaticClass();
	PlayerControllerClass = AMassifPlayerController::StaticClass();
}

void AMassifGameMode::BeginPlay()
{
	Super::BeginPlay();
	ApplyEarthGravity();
	LoadTerrainAndSpawnManagers();
	SpawnEnvironmentActors();
	ConfigureFlyPawn();
	ScheduleDebugScreenshot();
}

void AMassifGameMode::ApplyEarthGravity()
{
	AWorldSettings* WorldSettings = GetWorldSettings();
	WorldSettings->bGlobalGravitySet = true;
	WorldSettings->GlobalGravityZ = EarthGravityCentimetersPerSecondSquared;
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
	BuildEnvironmentBake(*Heightmap);

	TerrainManager = GetWorld()->SpawnActor<AMassifTerrainManager>();
	TerrainManager->Initialize(TerrainField, LoadTerrainMaterial());

	FoliageManager = GetWorld()->SpawnActor<AMassifFoliageManager>();
	FoliageManager->Initialize(TerrainField, EnvironmentBake, GetMasterSeed());
}

void AMassifGameMode::BuildEnvironmentBake(const FMassifHeightmap& Heightmap)
{
	const double StartSeconds = FPlatformTime::Seconds();
	EnvironmentBake = FMassifEnvironmentBake::Build(
		Heightmap, GetMasterSeed(), FMassifEnvironmentBakeParams());

	UE_LOG(LogTemp, Display,
		TEXT("Environment: %d x %d cells at %.0f m, %s, solved in %.2f s "
			"(water to %.1f m, flow to %.0f m2, soil to %.2f m)"),
		EnvironmentBake->GetWidth(), EnvironmentBake->GetHeight(), EnvironmentBake->GetCellSizeMeters(),
		DescribeBakePath(EnvironmentBake->WasBuiltOnGpu()),
		FPlatformTime::Seconds() - StartSeconds,
		EnvironmentBake->GetChannelMaximum(EMassifBakeChannel::StandingWaterDepth),
		EnvironmentBake->GetChannelMaximum(EMassifBakeChannel::FlowAccumulation),
		EnvironmentBake->GetChannelMaximum(EMassifBakeChannel::SoilDepth));
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
	SpawnSunAndSky();
	SpawnAtmosphericHaze();
	SpawnPostProcess();
}

void AMassifGameMode::SpawnSunAndSky()
{
	UWorld* World = GetWorld();

	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>();
	Sun->SetMobility(EComponentMobility::Movable);
	Sun->SetActorRotation(SunRotation);
	UDirectionalLightComponent* SunComponent = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
	SunComponent->SetAtmosphereSunLight(true);

	World->SpawnActor<ASkyAtmosphere>();

	ASkyLight* SkyLight = World->SpawnActor<ASkyLight>();
	SkyLight->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	SkyLight->GetLightComponent()->bRealTimeCapture = true;
	SkyLight->GetLightComponent()->MarkRenderStateDirty();
}

void AMassifGameMode::SpawnAtmosphericHaze()
{
	// Sitting the fog on the valley floor rather than the world origin puts the
	// dense layer where the valleys are instead of below sea level.
	const double FogAltitudeMeters = TerrainField.IsValid() ? TerrainField->GetMinAltitudeMeters() : 0.0;
	AExponentialHeightFog* Fog = GetWorld()->SpawnActor<AExponentialHeightFog>(
		FVector(0.0, 0.0, FogAltitudeMeters * 100.0), FRotator::ZeroRotator);

	UExponentialHeightFogComponent* FogComponent = Fog->GetComponent();
	FogComponent->SetFogDensity(FogDensity);
	FogComponent->SetFogHeightFalloff(FogHeightFalloffPerCentimeter);
	FogComponent->SetStartDistance(FogStartDistanceCentimeters);
	FogComponent->SetFogMaxOpacity(FogMaxOpacity);
}

void AMassifGameMode::SpawnPostProcess()
{
	APostProcessVolume* Volume = GetWorld()->SpawnActor<APostProcessVolume>();
	Volume->bUnbound = true;

	FPostProcessSettings& Settings = Volume->Settings;
	Settings.bOverride_AutoExposureBias = true;
	Settings.AutoExposureBias = ExposureBias;
	Settings.bOverride_AutoExposureMinBrightness = true;
	Settings.AutoExposureMinBrightness = ExposureMinBrightness;
	Settings.bOverride_AutoExposureMaxBrightness = true;
	Settings.AutoExposureMaxBrightness = ExposureMaxBrightness;
}

void AMassifGameMode::ConfigureFlyPawn()
{
	APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	if (PlayerController == nullptr || PlayerController->GetPawn() == nullptr)
	{
		return;
	}

	PlayerController->GetPawn()->SetActorLocation(FVector(0.0, -300000.0, SpawnAltitudeMeters * 100.0));
	PlayerController->SetControlRotation(FRotator(-22.0f, 90.0f, 0.0f));
}
