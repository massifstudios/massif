#include "MassifGameMode.h"

#include "MassifSimSubsystem.h"
#include "Weather/MassifDaylightManager.h"
#include "Weather/MassifFogManager.h"

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
#include "UI/MassifEnvironmentSettings.h"
#include "MassifHeightmap.h"
#include "MassifObjectStore.h"
#include "MassifObjectStoreBuilder.h"
#include "MassifObjectTypeRegistry.h"
#include "MassifRiverField.h"
#include "MassifRiverNetwork.h"
#include "MassifRockfallProcesses.h"
#include "MassifTerrainField.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Player/MassifFlyPawn.h"
#include "Player/MassifPlayerController.h"
#include "Terrain/MassifRockManager.h"
#include "Terrain/MassifTerrainManager.h"
#include "UObject/SoftObjectPath.h"
#include "GameFramework/PlayerController.h"
#include "Water/MassifWaterBodyManager.h"

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

	/**
	 * How graded the carved bed came out, which is what decides whether the ground
	 * under the water reads as a channel or as a row of pits. A step is the drop
	 * from one station's invert to the next: a bed the flow could have graded has
	 * small ones everywhere, and a bed that hit a limit somewhere has a few large.
	 */
	struct FMassifRiverBedSummary
	{
		double MeanDepthMeters = 0.0;
		double MaxDepthMeters = 0.0;
		double MaxStepMeters = 0.0;
		double MaxStepSpanMeters = 0.0;
		double IncisionLimitFraction = 0.0;
	};

	/** Within this of the backstop counts as having reached it. */
	constexpr double IncisionLimitToleranceMeters = 0.05;

	void AccumulateReachBed(
		const FMassifRiverReach& Reach, double IncisionLimitMeters, FMassifRiverBedSummary& InOutSummary,
		int32& InOutStationCount, int32& InOutLimitedCount)
	{
		for (int32 NodeIndex = 0; NodeIndex < Reach.Nodes.Num(); ++NodeIndex)
		{
			const FMassifRiverNode& Node = Reach.Nodes[NodeIndex];
			InOutSummary.MeanDepthMeters += Node.DepthMeters;
			InOutSummary.MaxDepthMeters = FMath::Max(InOutSummary.MaxDepthMeters, Node.DepthMeters);
			++InOutStationCount;

			const double IncisionMeters = Node.GroundAltitudeMeters - Node.BedAltitudeMeters;
			if (IncisionMeters >= IncisionLimitMeters - IncisionLimitToleranceMeters)
			{
				++InOutLimitedCount;
			}
			if (NodeIndex == 0)
			{
				continue;
			}

			const FMassifRiverNode& Above = Reach.Nodes[NodeIndex - 1];
			const double StepMeters = FMath::Abs(Above.BedAltitudeMeters - Node.BedAltitudeMeters);
			if (StepMeters > InOutSummary.MaxStepMeters)
			{
				InOutSummary.MaxStepMeters = StepMeters;
				InOutSummary.MaxStepSpanMeters =
					(Node.PositionMeters - Above.PositionMeters).Length();
			}
		}
	}

	FMassifRiverBedSummary MeasureRiverBed(const FMassifRiverNetwork& Rivers)
	{
		FMassifRiverBedSummary Summary;
		int32 StationCount = 0;
		int32 LimitedCount = 0;
		const double IncisionLimitMeters = FMassifChannelCarveParams().MaxIncisionBelowGroundMeters;
		for (const FMassifRiverReach& Reach : Rivers.GetReaches())
		{
			AccumulateReachBed(Reach, IncisionLimitMeters, Summary, StationCount, LimitedCount);
		}

		if (StationCount == 0)
		{
			return Summary;
		}
		Summary.MeanDepthMeters /= StationCount;
		Summary.IncisionLimitFraction = static_cast<double>(LimitedCount) / StationCount;
		return Summary;
	}

	/** The station carrying the most water in one reach, which is where its channel is widest. */
	const FMassifRiverNode* BusiestNode(const FMassifRiverReach& Reach)
	{
		const FMassifRiverNode* Busiest = nullptr;
		for (const FMassifRiverNode& Node : Reach.Nodes)
		{
			if (Busiest != nullptr
				&& Node.DischargeCubicMetersPerSecond <= Busiest->DischargeCubicMetersPerSecond)
			{
				continue;
			}
			Busiest = &Node;
		}
		return Busiest;
	}

	/** The busiest station in the whole world, which is the one worth baking first. */
	const FMassifRiverNode* BusiestNode(const FMassifRiverNetwork& Rivers)
	{
		const FMassifRiverNode* Busiest = nullptr;
		for (const FMassifRiverReach& Reach : Rivers.GetReaches())
		{
			const FMassifRiverNode* Node = BusiestNode(Reach);
			if (Node == nullptr)
			{
				continue;
			}
			if (Busiest != nullptr
				&& Node->DischargeCubicMetersPerSecond <= Busiest->DischargeCubicMetersPerSecond)
			{
				continue;
			}
			Busiest = Node;
		}
		return Busiest;
	}

	/** The busiest station in the network, so the camera can be sent to the biggest river by name. */
	FVector2d GBusiestNodeMeters = FVector2d::ZeroVector;
	bool bGBusiestNodeFound = false;

	/** Head height over the ground, so a teleport lands standing rather than buried. */
	constexpr double TeleportEyeHeightMeters = 2.0;

	void TeleportCameraTo(UWorld* World, FVector2d PositionMeters)
	{
		AMassifGameMode* GameMode = World->GetAuthGameMode<AMassifGameMode>();
		APlayerController* Controller = World->GetFirstPlayerController();
		if (GameMode == nullptr || Controller == nullptr || Controller->GetPawn() == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("Teleport: no camera to move yet."));
			return;
		}

		const TSharedPtr<const FMassifTerrainField> Terrain = GameMode->GetTerrainField();
		double GroundMeters = 0.0;
		if (Terrain.IsValid())
		{
			GroundMeters = Terrain->SampleHeightMeters(PositionMeters, 1.0);
		}

		const FVector TargetMeters(
			PositionMeters.X, PositionMeters.Y, GroundMeters + TeleportEyeHeightMeters);
		Controller->GetPawn()->SetActorLocation(TargetMeters * 100.0);
		UE_LOG(LogTemp, Display, TEXT("Teleport: camera at %.1f, %.1f m, ground %.1f m"),
			PositionMeters.X, PositionMeters.Y, GroundMeters);
	}

	void TeleportCommand(const TArray<FString>& Arguments, UWorld* World)
	{
		if (Arguments.Num() < 2)
		{
			UE_LOG(LogTemp, Warning, TEXT("Teleport: needs an x and a y in metres."));
			return;
		}
		TeleportCameraTo(World, FVector2d(FCString::Atod(*Arguments[0]), FCString::Atod(*Arguments[1])));
	}

	void TeleportToWaterCommand(const TArray<FString>&, UWorld* World)
	{
		if (!bGBusiestNodeFound)
		{
			UE_LOG(LogTemp, Warning, TEXT("Teleport: no river has been traced yet."));
			return;
		}
		TeleportCameraTo(World, GBusiestNodeMeters);
	}

	FAutoConsoleCommandWithWorldAndArgs GTeleportCommand(
		TEXT("massif.Teleport"),
		TEXT("Move the camera to a world position in metres: massif.Teleport <x> <y>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&TeleportCommand));

	FAutoConsoleCommandWithWorldAndArgs GTeleportToWaterCommand(
		TEXT("massif.GotoWater"),
		TEXT("Move the camera to the busiest river in the network."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&TeleportToWaterCommand));

	/** Earth's surface gravity, 9.81 m/s down. */
	constexpr float EarthGravityCentimetersPerSecondSquared = -981.0f;

	/** Mid-morning sun: low enough that ridges cast shadows down their own flanks. */
	const FRotator SunRotation(-30.0f, 25.0f, 0.0f);

	/** How far back the opening view sits, which frames most of the massif. */
	constexpr double OpeningZoomMeters = 9000.0;

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
	StartSimulation();
	LoadTerrainAndSpawnManagers();
	SpawnEnvironmentActors();
	ConfigureFlyPawn();
	ScheduleDebugScreenshot();
}

void AMassifGameMode::StartSimulation()
{
	// The wall clock is derived from the tick count, so nothing that reads the time
	// of day means anything until the simulation is running (ARCHITECTURE §6.1).
	UMassifSimSubsystem* Sim = GetGameInstance()->GetSubsystem<UMassifSimSubsystem>();
	Sim->StartNewGame(static_cast<int32>(GetMasterSeed()));
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

	BuildObjectStore(Environment.Get());
	RockManager = GetWorld()->SpawnActor<AMassifRockManager>();
	RockManager->Initialize(ObjectStore);

	SpawnWaterBodies();

	DebugChannelView = GetWorld()->SpawnActor<AMassifDebugChannelView>();
	DebugChannelView->Initialize(HabitatField, TerrainManager);

	FogManager = GetWorld()->SpawnActor<AMassifFogManager>();
	FogManager->Initialize(Environment, TerrainField, FMassifGeoreference().LatitudeDegrees);

}

void AMassifGameMode::BuildTerrainField(TSharedRef<const FMassifHeightmap> Heightmap)
{
	const TSharedRef<const FMassifRiverNetwork> Rivers =
		MassifExtractRivers(
			EnvironmentScheduler->GetState(), EnvironmentScheduler->GetElevation(),
			GetEnvironmentParams().Rivers);
	LogRiverSummary(Rivers.Get());

	RiverNetwork = Rivers;
	RiverField = FMassifRiverField::Build(Rivers.Get(), MassifDefaultBankReachMeters);
	TerrainField = MakeShared<const FMassifTerrainField>(MoveTemp(Heightmap), RiverField.ToSharedRef());

	UE_LOG(LogTemp, Display, TEXT("Terrain field: %.0f m across, %d channel segments cut into it"),
		TerrainField->GetWorldSizeMeters(), RiverField->GetSegmentCount());

	const FMassifRiverNode* Node = BusiestNode(Rivers.Get());
	if (Node != nullptr)
	{
		GBusiestNodeMeters = Node->PositionMeters;
		bGBusiestNodeFound = true;
	}
}

void AMassifGameMode::SpawnWaterBodies()
{
	if (!RiverNetwork.IsValid() || !TerrainField.IsValid())
	{
		return;
	}

	WaterBodyManager = GetWorld()->SpawnActor<AMassifWaterBodyManager>();
	WaterBodyManager->Initialize(*RiverNetwork, GBusiestNodeMeters, MassifDrawnWaterTimeSlice());
}

bool AMassifGameMode::BuildEnvironment(TSharedRef<const FMassifHeightmap> Heightmap)
{
	const double StartSeconds = FPlatformTime::Seconds();

	FString BuildError;
	TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifPrepareEnvironment(
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

	// Before the first solve, so a world set to something other than the defaults
	// is solved once at what it is set to rather than twice.
	MassifEnvironmentSettings::ApplySaved(*Scheduler);
	Scheduler->Execute();

	EnvironmentScheduler = MakeShareable(Scheduler.Release());
	LogEnvironmentSummary(FPlatformTime::Seconds() - StartSeconds);
	return true;
}

void AMassifGameMode::BuildObjectStore(const FMassifEnvironmentState& Environment)
{
	const double StartSeconds = FPlatformTime::Seconds();
	ObjectStore = MakeShared<const FMassifObjectStore>(
		MassifObjectStoreBuilder::BuildFromEnvironment(Environment, FMassifObjectTypeRegistry::Get()));

	UE_LOG(LogTemp, Display, TEXT("Object store: %d objects over %d chunks, %.1f MB resident, built in %.2f s"),
		ObjectStore->GetRecordCount(), ObjectStore->GetChunkGrid().ChunkCount(),
		ObjectStore->GetResidentByteCount() / (1024.0 * 1024.0),
		FPlatformTime::Seconds() - StartSeconds);
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

	LogRockfallSummary(Environment);
}

void AMassifGameMode::LogRockfallSummary(const FMassifEnvironmentState& Environment) const
{
	const FMassifPointSetHandle Placements =
		Environment.FindPointSet(MassifEnvironmentPointSets::RockPlacements());
	if (!Placements.IsValid())
	{
		return;
	}

	UE_LOG(LogTemp, Display,
		TEXT("Rockfall: %d boulders placed, deepest deposit %.2f m, largest block %.1f m across, "
			"solved in %.2f s (environment %.1f MB resident)"),
		Environment.GetPointCount(Placements),
		GetChannelMaximum(Environment, MassifEnvironmentChannels::RockfallDeposit()),
		2.0 * GetChannelMaximum(Environment, MassifEnvironmentChannels::RockfallBlockRadius()),
		EnvironmentScheduler->GetLastExecutionSeconds(MassifEnvironmentProcesses::Rockfall()),
		Environment.GetResidentByteCount() / (1024.0 * 1024.0));
}

void AMassifGameMode::LogRiverSummary(const FMassifRiverNetwork& Rivers) const
{
	UE_LOG(LogTemp, Display,
		TEXT("Rivers: %d reaches from %d sources, %.1f km of channel, order %d, "
			"largest %.1f m3/s and %.1f m wide"),
		Rivers.GetReaches().Num(), Rivers.GetSourceCount(), Rivers.GetTotalLengthMeters() / 1000.0,
		Rivers.GetMaxStrahlerOrder(), Rivers.GetMaxDischargeCubicMetersPerSecond(), Rivers.GetMaxWidthMeters());

	const FMassifRiverBedSummary Bed = MeasureRiverBed(Rivers);
	UE_LOG(LogTemp, Display,
		TEXT("River bed: %.2f m mean depth, deepest %.2f m, steepest step %.2f m over %.1f m, "
			"%.1f%% of stations at the incision limit"),
		Bed.MeanDepthMeters, Bed.MaxDepthMeters, Bed.MaxStepMeters, Bed.MaxStepSpanMeters,
		100.0 * Bed.IncisionLimitFraction);
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

	// After the sun, not before it: the manager drives that light, and spawning it
	// with the managers that read the environment handed it a null one.
	DaylightManager = GetWorld()->SpawnActor<AMassifDaylightManager>();
	DaylightManager->Initialize(SunLight, FMassifGeoreference().LatitudeDegrees);
	SpawnAtmosphericHaze();
	SpawnPostProcess();
}

void AMassifGameMode::SpawnSunAndSky()
{
	UWorld* World = GetWorld();

	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>();
	SunLight = Sun;
	Sun->SetMobility(EComponentMobility::Movable);
	Sun->SetActorRotation(SunRotation);
	UDirectionalLightComponent* SunComponent = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
	SunComponent->SetAtmosphereSunLight(true);

	// The atmosphere attenuates and reddens the sunlight along its own path, so a
	// low sun goes orange and a set one stops lighting the ground without any of it
	// being faked from outside. It is also what makes the sky and the ground agree:
	// both are then reading one scattering model rather than two.
	SunComponent->bPerPixelAtmosphereTransmittance = true;

	World->SpawnActor<ASkyAtmosphere>();

	// Real-time capture, so the ambient follows the sky the atmosphere is drawing
	// as the sun moves, rather than holding the one it was built with.
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

	// The camera frames a ground point rather than being posed, so the opening view
	// is stated the same way every later one is.
	AMassifFlyPawn* Camera = Cast<AMassifFlyPawn>(PlayerController->GetPawn());
	if (Camera == nullptr)
	{
		return;
	}

	Camera->FrameGroundPoint(FVector2d::ZeroVector, OpeningZoomMeters);
}
