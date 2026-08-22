#include "MassifGameMode.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Debug/MassifDebugChannelView.h"
#include "Debug/MassifDebugHud.h"
#include "Foliage/MassifFoliageManager.h"
#include "GameFramework/WorldSettings.h"
#include "MassifElevationSource.h"
#include "MassifEnvironmentBakeParams.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifEnvironmentState.h"
#include "MassifHabitat.h"
#include "MassifHeightmap.h"
#include "MassifRiverField.h"
#include "MassifRiverNetwork.h"
#include "MassifTerrainField.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Player/MassifFlyPawn.h"
#include "Player/MassifPlayerController.h"
#include "Terrain/MassifTerrainManager.h"
#include "UObject/SoftObjectPath.h"
#include "Water/MassifRiverManager.h"

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

	/**
	 * A material that fails to compile is replaced by the engine default, which
	 * renders as plausible grey ground rather than as anything obviously wrong.
	 * The engine says so at Warning level among thousands of other lines; this
	 * says so at Error level, naming the node that broke it.
	 */
	void ReportMaterialCompileErrors(UMaterialInterface& Material)
	{
		const FMaterialResource* Resource = Material.GetMaterialResource(GMaxRHIShaderPlatform);
		if (Resource == nullptr || Resource->GetCompileErrors().IsEmpty())
		{
			return;
		}

		UE_LOG(LogTemp, Error, TEXT("%s failed to compile, so the engine default material is drawn instead:"),
			*Material.GetName());
		for (const FString& CompileError : Resource->GetCompileErrors())
		{
			UE_LOG(LogTemp, Error, TEXT("  %s"), *CompileError);
		}
	}

	/** One set of knobs for the solve and for the rivers traced out of it, so the two cannot drift. */
	const FMassifEnvironmentBakeParams& GetEnvironmentParams()
	{
		static const FMassifEnvironmentBakeParams Params;
		return Params;
	}

	double GetChannelMaximum(const FMassifEnvironmentState& Environment, const FName& ChannelName)
	{
		return Environment.GetChannelMaximum(Environment.FindChannel(ChannelName));
	}

	UMaterialInterface* LoadTerrainMaterial()
	{
		const FSoftObjectPath MaterialPath(TEXT("/Game/Materials/M_Terrain.M_Terrain"));
		UMaterialInterface* Material = Cast<UMaterialInterface>(MaterialPath.TryLoad());
		if (Material != nullptr)
		{
			ReportMaterialCompileErrors(*Material);
			return Material;
		}

		UE_LOG(LogTemp, Warning, TEXT("M_Terrain missing, falling back to WorldGridMaterial"));
		return LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
	}

	UMaterialInterface* LoadRiverMaterial()
	{
		const FSoftObjectPath MaterialPath(TEXT("/Game/Materials/M_River.M_River"));
		UMaterialInterface* Material = Cast<UMaterialInterface>(MaterialPath.TryLoad());
		if (Material == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("M_River missing, no water is drawn. Run Tools/create_river_content.py."));
			return nullptr;
		}

		ReportMaterialCompileErrors(*Material);
		return Material;
	}
}

AMassifGameMode::AMassifGameMode()
{
	DefaultPawnClass = AMassifFlyPawn::StaticClass();
	PlayerControllerClass = AMassifPlayerController::StaticClass();
	HUDClass = AMassifDebugHud::StaticClass();
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

	UE_LOG(LogTemp, Display, TEXT("Terrain: %s, altitude %.0f-%.0f m, %d mips, loaded in %.2f s"),
		*Heightmap->GetSourceInfo().Label, Heightmap->GetMinAltitudeMeters(), Heightmap->GetMaxAltitudeMeters(),
		Heightmap->GetMipCount(), FPlatformTime::Seconds() - StartSeconds);

	// The environment reads the elevation model, and the terrain field reads the
	// rivers traced out of it. That order is what keeps the carve out of the solve
	// that produced it.
	if (!BuildEnvironment(Heightmap.ToSharedRef()))
	{
		return;
	}
	BuildTerrainField(Heightmap.ToSharedRef());
	SpawnAltitudeMeters = static_cast<float>(TerrainField->GetMaxAltitudeMeters()) + 2200.0f;

	// One habitat field for everything that reads the world: the terrain's
	// textures, the plants standing on them and the debug views all have to
	// answer the same about a given point, and they do so by construction when
	// they read the same field.
	const TSharedRef<const FMassifEnvironmentState> Environment = EnvironmentScheduler->GetSharedState();
	const TSharedRef<const FMassifHabitatField> HabitatField = MakeShared<const FMassifHabitatField>(
		TerrainField.ToSharedRef(), Environment, GetMasterSeed());

	TerrainManager = GetWorld()->SpawnActor<AMassifTerrainManager>();
	TerrainManager->Initialize(TerrainField, LoadTerrainMaterial());
	TerrainManager->ApplySurfaceMap(HabitatField.Get(), GetMasterSeed());

	FoliageManager = GetWorld()->SpawnActor<AMassifFoliageManager>();
	FoliageManager->Initialize(TerrainField, Environment, GetMasterSeed());

	RiverManager = GetWorld()->SpawnActor<AMassifRiverManager>();
	RiverManager->Initialize(RiverField, LoadRiverMaterial());

	DebugChannelView = GetWorld()->SpawnActor<AMassifDebugChannelView>();
	DebugChannelView->Initialize(HabitatField, TerrainManager);
}

void AMassifGameMode::BuildTerrainField(TSharedRef<const FMassifHeightmap> Heightmap)
{
	const TSharedRef<const FMassifRiverNetwork> Rivers =
		MassifExtractRivers(EnvironmentScheduler->GetState(), GetEnvironmentParams().Rivers);
	LogRiverSummary(Rivers.Get());

	const FMassifRiverCarveParams CarveParams;
	RiverField = FMassifRiverField::Build(Rivers.Get(), CarveParams.MaxBankMeters);
	TerrainField =
		MakeShared<const FMassifTerrainField>(MoveTemp(Heightmap), RiverField.ToSharedRef(), CarveParams);

	UE_LOG(LogTemp, Display, TEXT("Terrain field: %.0f m across, %d channel segments cut into it"),
		TerrainField->GetWorldSizeMeters(), RiverField->GetSegmentCount());
}

bool AMassifGameMode::BuildEnvironment(TSharedRef<const FMassifHeightmap> Heightmap)
{
	const double StartSeconds = FPlatformTime::Seconds();

	FString BuildError;
	TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifBuildEnvironment(
		MakeShared<const FMassifHeightmapElevationSource>(MoveTemp(Heightmap)),
		FMassifGeoreference(),
		GetMasterSeed(),
		GetEnvironmentParams(),
		BuildError);
	if (!Scheduler.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Environment: %s. Nothing downstream of the solve is built."), *BuildError);
		return false;
	}

	EnvironmentScheduler = MakeShareable(Scheduler.Release());
	LogEnvironmentSummary(FPlatformTime::Seconds() - StartSeconds);
	return true;
}

void AMassifGameMode::LogEnvironmentSummary(double SolveSeconds) const
{
	const FMassifEnvironmentState& Environment = EnvironmentScheduler->GetState();
	const FMassifChannelGrid& Grid =
		Environment.GetGrid(Environment.FindChannel(MassifEnvironmentChannels::Wetness()));

	UE_LOG(LogTemp, Display,
		TEXT("Environment: %d x %d cells at %.0f m, %d channels, solved in %.2f s "
			"(water to %.1f m, flow to %.0f m2, soil to %.2f m)"),
		Grid.Width, Grid.Height, Grid.CellSizeMeters, Environment.GetChannelCount(), SolveSeconds,
		GetChannelMaximum(Environment, MassifEnvironmentChannels::StandingWaterDepth()),
		GetChannelMaximum(Environment, MassifEnvironmentChannels::FlowAccumulation()),
		GetChannelMaximum(Environment, MassifEnvironmentChannels::SoilDepth()));
}

void AMassifGameMode::LogRiverSummary(const FMassifRiverNetwork& Rivers) const
{
	UE_LOG(LogTemp, Display,
		TEXT("Rivers: %d reaches from %d sources, %.1f km of channel, order %d, "
			"largest %.1f m3/s and %.1f m wide"),
		Rivers.GetReaches().Num(), Rivers.GetSourceCount(), Rivers.GetTotalLengthMeters() / 1000.0,
		Rivers.GetMaxStrahlerOrder(), Rivers.GetMaxDischargeCubicMetersPerSecond(), Rivers.GetMaxWidthMeters());
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
