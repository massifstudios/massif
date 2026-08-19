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
#include "Components/InputComponent.h"
#include "Engine/GameViewportClient.h"
#include "MassifTerrainField.h"
#include "MassifTuningPanel.h"
#include "Materials/MaterialInterface.h"
#include "Terrain/MassifTerrainManager.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/Layout/SBox.h"
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

	// Erosion-filter knobs, live: a change rebuilds the terrain field around the
	// already-solved bake once the value has settled. Defaults mirror
	// FWorldGenConfig.
	TAutoConsoleVariable<float> CVarMassifGullyDepth(
		TEXT("massif.Gully.Depth"), 9.0f,
		TEXT("Largest gully depth in meters — overall erosion-filter strength."));

	TAutoConsoleVariable<float> CVarMassifGullyWavelength(
		TEXT("massif.Gully.Wavelength"), 220.0f,
		TEXT("Spacing of the coarsest gullies in meters."));

	TAutoConsoleVariable<float> CVarMassifGullyBranchStrength(
		TEXT("massif.Gully.BranchStrength"), 0.25f,
		TEXT("How far a gully drags finer gullies sideways, as a fraction of its wavelength."));

	TAutoConsoleVariable<float> CVarMassifGullyRidgeRounding(
		TEXT("massif.Gully.RidgeRounding"), 0.6f,
		TEXT("1 rounded cosine ridge, 0 pinched straight-sided edge."));

	TAutoConsoleVariable<float> CVarMassifGullyCreaseRounding(
		TEXT("massif.Gully.CreaseRounding"), 0.15f,
		TEXT("1 rounded gully floor, 0 pinched crease."));

	TAutoConsoleVariable<float> CVarMassifGullySlopeReference(
		TEXT("massif.Gully.SlopeReference"), 0.35f,
		TEXT("Surface gradient at which gullies cut to full depth."));

	bool bMassifRegenerateRequested = false;

	FAutoConsoleCommand CmdMassifRegenerate(
		TEXT("massif.Regenerate"),
		TEXT("Rebuild the terrain field including the fluvial bake, picking up massif.Seed."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			bMassifRegenerateRequested = true;
		}));

	void ApplyGullyKnobCVars(FWorldGenConfig& Config)
	{
		Config.LargestGullyDepthMeters = CVarMassifGullyDepth.GetValueOnGameThread();
		Config.LargestGullyWavelengthMeters = CVarMassifGullyWavelength.GetValueOnGameThread();
		Config.GullyBranchStrength = CVarMassifGullyBranchStrength.GetValueOnGameThread();
		Config.GullyRidgeRounding = CVarMassifGullyRidgeRounding.GetValueOnGameThread();
		Config.GullyCreaseRounding = CVarMassifGullyCreaseRounding.GetValueOnGameThread();
		Config.GullySlopeReference = CVarMassifGullySlopeReference.GetValueOnGameThread();
	}

	bool SameGullyKnobs(const FWorldGenConfig& Left, const FWorldGenConfig& Right)
	{
		return Left.LargestGullyDepthMeters == Right.LargestGullyDepthMeters
			&& Left.LargestGullyWavelengthMeters == Right.LargestGullyWavelengthMeters
			&& Left.GullyBranchStrength == Right.GullyBranchStrength
			&& Left.GullyRidgeRounding == Right.GullyRidgeRounding
			&& Left.GullyCreaseRounding == Right.GullyCreaseRounding
			&& Left.GullySlopeReference == Right.GullySlopeReference;
	}

	/** Long enough that dragging a value in steps triggers one rebuild, not one per step. */
	constexpr double GullyRebuildDebounceSeconds = 0.5;

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
	PrimaryActorTick.bCanEverTick = true;
}

void AMassifGameMode::BeginPlay()
{
	Super::BeginPlay();
	GenerateWorldAndSpawnTerrain();
	SpawnEnvironmentActors();
	ConfigureSpectatorPawn();
	ScheduleDebugScreenshot();
}

void AMassifGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!TerrainField.IsValid() || TerrainManager == nullptr)
	{
		return;
	}

	if (bMassifRegenerateRequested)
	{
		bMassifRegenerateRequested = false;
		PendingKnobsSinceSeconds = -1.0;
		RebuildTerrainField(MakeWorldGenConfig(), false);
		return;
	}

	PollGullyKnobs();
}

void AMassifGameMode::PollGullyKnobs()
{
	FWorldGenConfig Desired = TerrainField->GetConfig();
	ApplyGullyKnobCVars(Desired);
	if (SameGullyKnobs(Desired, TerrainField->GetConfig()))
	{
		PendingKnobsSinceSeconds = -1.0;
		return;
	}

	const double NowSeconds = FPlatformTime::Seconds();
	const bool bKnobsMovedAgain = PendingKnobsSinceSeconds < 0.0 || !SameGullyKnobs(Desired, PendingKnobConfig);
	if (bKnobsMovedAgain)
	{
		PendingKnobConfig = Desired;
		PendingKnobsSinceSeconds = NowSeconds;
	}
	if (NowSeconds - PendingKnobsSinceSeconds < GullyRebuildDebounceSeconds)
	{
		return;
	}

	PendingKnobsSinceSeconds = -1.0;
	RebuildTerrainField(Desired, true);
}

void AMassifGameMode::ToggleTuningPanel()
{
	UGameViewportClient* Viewport = GetWorld()->GetGameViewport();
	APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	if (Viewport == nullptr || PlayerController == nullptr)
	{
		return;
	}

	if (bTuningPanelVisible)
	{
		Viewport->RemoveViewportWidgetContent(TuningPanel.ToSharedRef());
		PlayerController->SetShowMouseCursor(false);
		PlayerController->SetInputMode(FInputModeGameOnly());
		bTuningPanelVisible = false;
		return;
	}

	if (!TuningPanel.IsValid())
	{
		TuningPanel = SNew(SBox).HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(FMargin(20.0f))
		[
			SNew(SBox).WidthOverride(430.0f)
			[
				SNew(SMassifTuningPanel)
			]
		];
	}
	Viewport->AddViewportWidgetContent(TuningPanel.ToSharedRef());
	PlayerController->SetShowMouseCursor(true);
	PlayerController->SetInputMode(FInputModeGameAndUI());
	bTuningPanelVisible = true;
}

void AMassifGameMode::RebuildTerrainField(const FWorldGenConfig& Config, bool bReuseBake)
{
	const double StartSeconds = FPlatformTime::Seconds();
	TSharedPtr<const FMassifTerrainField> NewField;
	if (bReuseBake)
	{
		NewField = MakeShared<const FMassifTerrainField>(Config, TerrainField->GetFluvialBakeShared());
	}
	else
	{
		NewField = MakeShared<const FMassifTerrainField>(Config);
	}

	TerrainField = NewField;
	TerrainManager->ResetField(NewField);
	UE_LOG(LogTemp, Display, TEXT("Terrain field rebuilt in %.2f s (%s)"),
		FPlatformTime::Seconds() - StartSeconds,
		bReuseBake ? TEXT("bake reused") : TEXT("bake re-solved"));
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
	float LowestMeters = TNumericLimits<float>::Max();
	float HighestMeters = TNumericLimits<float>::Lowest();
	for (const float HeightMeters : Result.HeightMeters)
	{
		LowestMeters = FMath::Min(LowestMeters, HeightMeters);
		HighestMeters = FMath::Max(HighestMeters, HeightMeters);
	}

	const int32 LocalMinimumCount = Result.CountLocalMinima();
	UE_LOG(LogTemp, Display, TEXT("WorldGen sanity: altitude %.0f-%.0f m, max neighbor height delta %.1f m, %d pits (%.3f%% of cells)"),
		LowestMeters, HighestMeters, MaxNeighborDeltaMeters, LocalMinimumCount,
		100.0f * static_cast<float>(LocalMinimumCount) / static_cast<float>(Result.HeightMeters.Num()));

	const FString ExportDirectory = FPaths::ProjectSavedDir() / TEXT("WorldGenDebug");
	const bool bExported = FWorldGenDebugExport::SaveHeightfieldPngs(Result, ExportDirectory);
	UE_LOG(LogTemp, Display, TEXT("WorldGen debug export %s: %s"),
		bExported ? TEXT("written to") : TEXT("FAILED for"), *ExportDirectory);
	FPlatformMisc::RequestExit(false);
}

FWorldGenConfig AMassifGameMode::MakeWorldGenConfig() const
{
	FWorldGenConfig Config;
	Config.MasterSeed = CVarMassifSeed.GetValueOnGameThread();
	Config.Resolution = CVarMassifWorldGenResolution.GetValueOnGameThread();
	FParse::Value(FCommandLine::Get(), TEXT("MassifSeed="), Config.MasterSeed);
	ApplyGullyKnobCVars(Config);
	return Config;
}

void AMassifGameMode::GenerateWorldAndSpawnTerrain()
{
	const FWorldGenConfig Config = MakeWorldGenConfig();
	if (FParse::Param(FCommandLine::Get(), TEXT("MassifExportWorldGen")))
	{
		ExportWorldGenDebug(Config);
		return;
	}

	const double FieldStartSeconds = FPlatformTime::Seconds();
	TerrainField = MakeShared<const FMassifTerrainField>(Config);
	UE_LOG(LogTemp, Display, TEXT("WorldGen: seed %d, terrain field over %.0f m built in %.2f s"),
		Config.MasterSeed, Config.WorldSizeMeters, FPlatformTime::Seconds() - FieldStartSeconds);

	SpawnAltitudeMeters = Config.MaxAltitudeMeters + 2200.0f;

	TerrainManager = GetWorld()->SpawnActor<AMassifTerrainManager>();
	TerrainManager->Initialize(TerrainField, LoadTerrainMaterial());
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

	if (PlayerController->InputComponent != nullptr)
	{
		PlayerController->InputComponent->BindKey(EKeys::F1, IE_Pressed, this, &AMassifGameMode::ToggleTuningPanel);
	}
	if (GEngine != nullptr)
	{
		GEngine->AddOnScreenDebugMessage(INDEX_NONE, 10.0f, FColor::White, TEXT("F1: terrain tuning panel"));
	}

	UFloatingPawnMovement* Movement = Cast<UFloatingPawnMovement>(Pawn->GetMovementComponent());
	if (Movement != nullptr)
	{
		// 600 m/s crosses the 20 km world in about half a minute.
		Movement->MaxSpeed = 60000.0f;
		Movement->Acceleration = 120000.0f;
		Movement->Deceleration = 240000.0f;
	}
}
