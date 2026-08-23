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
		double MaxVoidMeters = 0.0;
		double MeanVoidMeters = 0.0;
		double VoidFraction = 0.0;
		double EdgeThalwegFraction = 0.0;
		double LevelRunFraction = 0.0;
		int32 LongestLevelRun = 0;
		double MeanSectionReliefMeters = 0.0;
		double MeanCentrelineOffsetMeters = 0.0;
		double MaxCentrelineOffsetMeters = 0.0;
	};

	/** Within this of the backstop counts as having reached it. */
	constexpr double IncisionLimitToleranceMeters = 0.05;

	/** Void this deep under the water is what reads as a plate standing over open air. */
	constexpr double VoidToleranceMeters = 1.0;

	/** Running tallies over stations, kept apart from the metres the summary reports. */
	struct FMassifRiverBedCounts
	{
		int32 StationCount = 0;
		int32 LimitedCount = 0;
		int32 VoidCount = 0;
		int32 EdgeThalwegCount = 0;
		int32 LevelRunCount = 0;
		int32 RunLength = 0;
	};

	/** Two surfaces this close together draw as one level slab. */
	constexpr double LevelRunToleranceMeters = 0.01;

	int32 ProbeThalwegOffset(const FMassifChannelProfile& Profile)
	{
		int32 ThalwegOffset = 0;
		for (int32 OffsetIndex = 1; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			if (Profile.BedAltitudeMeters[OffsetIndex] < Profile.BedAltitudeMeters[ThalwegOffset])
			{
				ThalwegOffset = OffsetIndex;
			}
		}
		return ThalwegOffset;
	}

	/**
	 * How far off the section's own low ground the traced centreline sits. A trace
	 * that followed the channel puts it near the middle whatever the terrain does.
	 */
	double CentrelineOffsetMeters(const FMassifChannelProfile& Profile)
	{
		int32 LowestOffset = 0;
		for (int32 OffsetIndex = 1; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			if (Profile.GroundAltitudeMeters[OffsetIndex] < Profile.GroundAltitudeMeters[LowestOffset])
			{
				LowestOffset = OffsetIndex;
			}
		}
		return FMath::Abs(Profile.OffsetMeters(LowestOffset));
	}

	/**
	 * How far the ground under a station falls past the bottom of its own water,
	 * across the width the water is drawn over.
	 *
	 * The terrain carve only ever cuts ground down, so ground already lower than the
	 * channel the solve settled on keeps its own level and the water drawn over it
	 * spans a void rather than a bed.
	 */
	double VoidUnderWaterMeters(const FMassifRiverNode& Node)
	{
		const FMassifChannelProfile& Profile = Node.Profile;
		const double HalfWidthMeters = 0.5 * Profile.WettedWidthMeters;
		double LowestMeters = Node.WaterSurfaceMeters;
		for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			if (FMath::Abs(Profile.OffsetMeters(OffsetIndex)) <= HalfWidthMeters)
			{
				LowestMeters = FMath::Min(
					LowestMeters, static_cast<double>(Profile.GroundAltitudeMeters[OffsetIndex]));
			}
		}
		return (Node.WaterSurfaceMeters - LowestMeters) - Node.DepthMeters;
	}

	/** Ground fall from the highest offset in a section to the lowest. */
	double SectionReliefMeters(const FMassifChannelProfile& Profile)
	{
		double LowestMeters = Profile.GroundAltitudeMeters[0];
		double HighestMeters = Profile.GroundAltitudeMeters[0];
		for (int32 OffsetIndex = 1; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			LowestMeters = FMath::Min(LowestMeters, static_cast<double>(Profile.GroundAltitudeMeters[OffsetIndex]));
			HighestMeters = FMath::Max(HighestMeters, static_cast<double>(Profile.GroundAltitudeMeters[OffsetIndex]));
		}
		return HighestMeters - LowestMeters;
	}

	/** A section whose lowest ground is at either end has no bank on that side, so it is a slope and not a channel. */
	bool HasEdgeThalweg(const FMassifChannelProfile& Profile)
	{
		const int32 ThalwegOffset = ProbeThalwegOffset(Profile);
		return ThalwegOffset == 0 || ThalwegOffset == MassifChannelProfileOffsetCount - 1;
	}

	void AccumulateReachBed(
		const FMassifRiverReach& Reach, double IncisionLimitMeters, FMassifRiverBedSummary& InOutSummary,
		FMassifRiverBedCounts& InOutCounts)
	{
		for (int32 NodeIndex = 0; NodeIndex < Reach.Nodes.Num(); ++NodeIndex)
		{
			const FMassifRiverNode& Node = Reach.Nodes[NodeIndex];
			InOutSummary.MeanDepthMeters += Node.DepthMeters;
			InOutSummary.MaxDepthMeters = FMath::Max(InOutSummary.MaxDepthMeters, Node.DepthMeters);
			++InOutCounts.StationCount;

			InOutSummary.MeanSectionReliefMeters += SectionReliefMeters(Node.Profile);
			const double CentrelineMeters = CentrelineOffsetMeters(Node.Profile);
			InOutSummary.MeanCentrelineOffsetMeters += CentrelineMeters;
			InOutSummary.MaxCentrelineOffsetMeters =
				FMath::Max(InOutSummary.MaxCentrelineOffsetMeters, CentrelineMeters);
			if (HasEdgeThalweg(Node.Profile))
			{
				++InOutCounts.EdgeThalwegCount;
			}

			const double VoidMeters = FMath::Max(VoidUnderWaterMeters(Node), 0.0);
			InOutSummary.MaxVoidMeters = FMath::Max(InOutSummary.MaxVoidMeters, VoidMeters);
			InOutSummary.MeanVoidMeters += VoidMeters;
			if (VoidMeters > VoidToleranceMeters)
			{
				++InOutCounts.VoidCount;
			}

			const double IncisionMeters = Node.GroundAltitudeMeters - Node.BedAltitudeMeters;
			if (IncisionMeters >= IncisionLimitMeters - IncisionLimitToleranceMeters)
			{
				++InOutCounts.LimitedCount;
			}
			if (NodeIndex == 0)
			{
				continue;
			}

			const FMassifRiverNode& Above = Reach.Nodes[NodeIndex - 1];
			const double SurfaceStepMeters =
				FMath::Abs(Above.WaterSurfaceMeters - Node.WaterSurfaceMeters);
			if (SurfaceStepMeters <= LevelRunToleranceMeters)
			{
				++InOutCounts.LevelRunCount;
				++InOutCounts.RunLength;
				InOutSummary.LongestLevelRun =
					FMath::Max(InOutSummary.LongestLevelRun, InOutCounts.RunLength);
			}
			else
			{
				InOutCounts.RunLength = 0;
			}

			const double StepMeters = FMath::Abs(Above.BedAltitudeMeters - Node.BedAltitudeMeters);
			if (StepMeters > InOutSummary.MaxStepMeters)
			{
				InOutSummary.MaxStepMeters = StepMeters;
				InOutSummary.MaxStepSpanMeters =
					(Node.PositionMeters - Above.PositionMeters).Length();
			}
		}
	}

	FString FormatProfileRow(const float* AltitudeMeters)
	{
		FString Row;
		for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			Row += FString::Printf(TEXT("%.1f "), AltitudeMeters[OffsetIndex]);
		}
		return Row;
	}

	void ProbeStationSection(const FMassifRiverNode& Node, int32 NodeIndex)
	{
		const FMassifChannelProfile& Profile = Node.Profile;
		UE_LOG(LogTemp, Display,
			TEXT("  section [%4d] halfspan %.1f m, spacing %.2f m, max wetted %.1f m, thalweg offset %d"),
			NodeIndex, Profile.HalfSpanMeters, Profile.OffsetSpacingMeters(),
			FMath::Min(Profile.MaxWettedWidthMeters, 9999.0), ProbeThalwegOffset(Profile));
		UE_LOG(LogTemp, Display, TEXT("    ground %s"), *FormatProfileRow(Profile.GroundAltitudeMeters));
		UE_LOG(LogTemp, Display, TEXT("    bed    %s"), *FormatProfileRow(Profile.BedAltitudeMeters));
	}

	void ProbeWorstVoid(const FMassifRiverNetwork& Rivers)
	{
		int32 WorstReach = INDEX_NONE;
		int32 WorstNode = INDEX_NONE;
		double WorstMeters = 0.0;
		const TArray<FMassifRiverReach>& Reaches = Rivers.GetReaches();
		for (int32 ReachIndex = 0; ReachIndex < Reaches.Num(); ++ReachIndex)
		{
			for (int32 NodeIndex = 0; NodeIndex < Reaches[ReachIndex].Nodes.Num(); ++NodeIndex)
			{
				const FMassifRiverNode& Node = Reaches[ReachIndex].Nodes[NodeIndex];
				const double VoidMeters = VoidUnderWaterMeters(Node);
				if (VoidMeters <= WorstMeters)
				{
					continue;
				}
				WorstMeters = VoidMeters;
				WorstReach = ReachIndex;
				WorstNode = NodeIndex;
			}
		}
		if (WorstReach == INDEX_NONE)
		{
			return;
		}

		const TArray<FMassifRiverNode>& Nodes = Reaches[WorstReach].Nodes;
		UE_LOG(LogTemp, Display, TEXT("Worst void: reach %d node %d of %d, %.2f m"),
			WorstReach, WorstNode, Nodes.Num(), WorstMeters);
		const int32 FirstIndex = FMath::Max(WorstNode - 6, 0);
		const int32 LastIndex = FMath::Min(WorstNode + 6, Nodes.Num() - 1);
		for (int32 NodeIndex = FirstIndex; NodeIndex <= LastIndex; ++NodeIndex)
		{
			const FMassifRiverNode& Node = Nodes[NodeIndex];
			UE_LOG(LogTemp, Display,
				TEXT("  [%4d] ground %8.2f bed %8.2f surface %8.2f depth %6.2f Q %6.2f slope %.5f"),
				NodeIndex, Node.GroundAltitudeMeters, Node.BedAltitudeMeters, Node.WaterSurfaceMeters,
				Node.DepthMeters, Node.DischargeCubicMetersPerSecond, Node.Profile.EnergySlope);
		}

		const int32 FirstSection = FMath::Max(WorstNode - 2, 0);
		const int32 LastSection = FMath::Min(WorstNode + 2, Nodes.Num() - 1);
		for (int32 NodeIndex = FirstSection; NodeIndex <= LastSection; ++NodeIndex)
		{
			ProbeStationSection(Nodes[NodeIndex], NodeIndex);
		}
	}

	FMassifRiverBedSummary MeasureRiverBed(const FMassifRiverNetwork& Rivers)
	{
		FMassifRiverBedSummary Summary;
		FMassifRiverBedCounts Counts;
		const double IncisionLimitMeters = FMassifChannelCarveParams().MaxIncisionBelowGroundMeters;
		for (const FMassifRiverReach& Reach : Rivers.GetReaches())
		{
			AccumulateReachBed(Reach, IncisionLimitMeters, Summary, Counts);
		}

		if (Counts.StationCount == 0)
		{
			return Summary;
		}
		Summary.MeanDepthMeters /= Counts.StationCount;
		Summary.IncisionLimitFraction =
			static_cast<double>(Counts.LimitedCount) / Counts.StationCount;
		Summary.VoidFraction = static_cast<double>(Counts.VoidCount) / Counts.StationCount;
		Summary.MeanVoidMeters /= Counts.StationCount;
		Summary.LevelRunFraction = static_cast<double>(Counts.LevelRunCount) / Counts.StationCount;
		Summary.EdgeThalwegFraction =
			static_cast<double>(Counts.EdgeThalwegCount) / Counts.StationCount;
		Summary.MeanSectionReliefMeters /= Counts.StationCount;
		Summary.MeanCentrelineOffsetMeters /= Counts.StationCount;
		return Summary;
	}

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

	RiverManager = GetWorld()->SpawnActor<AMassifRiverManager>();
	RiverManager->Initialize(RiverField, LoadRiverMaterial());

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

	const FMassifRiverBedSummary Bed = MeasureRiverBed(Rivers);
	UE_LOG(LogTemp, Display,
		TEXT("River bed: %.2f m mean depth, deepest %.2f m, steepest step %.2f m over %.1f m, "
			"%.1f%% of stations at the incision limit"),
		Bed.MeanDepthMeters, Bed.MaxDepthMeters, Bed.MaxStepMeters, Bed.MaxStepSpanMeters,
		100.0 * Bed.IncisionLimitFraction);

	UE_LOG(LogTemp, Display,
		TEXT("River surface: %.1f%% of stations span a void over a metre deep, %.2f m mean and %.2f m worst"),
		100.0 * Bed.VoidFraction, Bed.MeanVoidMeters, Bed.MaxVoidMeters);

	UE_LOG(LogTemp, Display,
		TEXT("River sections: %.1f%% have no bank on one side, %.2f m of ground fall across the mean section, "
			"centreline %.2f m off the low ground on average and %.2f m at worst"),
		100.0 * Bed.EdgeThalwegFraction, Bed.MeanSectionReliefMeters,
		Bed.MeanCentrelineOffsetMeters, Bed.MaxCentrelineOffsetMeters);
	UE_LOG(LogTemp, Display,
		TEXT("River terracing: %.1f%% of spans are level, longest level run %d stations"),
		100.0 * Bed.LevelRunFraction, Bed.LongestLevelRun);
	ProbeWorstVoid(Rivers);
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
