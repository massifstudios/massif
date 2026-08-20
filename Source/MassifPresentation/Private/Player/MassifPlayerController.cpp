#include "MassifPlayerController.h"

#include "Debug/MassifDebugChannelView.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "MassifGameMode.h"
#include "MassifInputConfig.h"
#include "MassifTerrainField.h"
#include "MassifWalkerPawn.h"

namespace
{
	constexpr double MetersToCentimeters = 100.0;

	/** Clearance above the field when the walker is dropped in, so it lands rather than starts embedded. */
	constexpr double WalkerDropClearanceMeters = 1.0;

	constexpr double GroundSampleSpacingMeters = 1.0;
}

AMassifPlayerController::AMassifPlayerController()
{
	PrimaryActorTick.bCanEverTick = true;
}

UMassifInputConfig* AMassifPlayerController::GetInputConfig(AController* Controller)
{
	AMassifPlayerController* PlayerController = Cast<AMassifPlayerController>(Controller);
	if (PlayerController == nullptr)
	{
		return nullptr;
	}
	return PlayerController->EnsureInputConfig();
}

UMassifInputConfig* AMassifPlayerController::EnsureInputConfig()
{
	if (InputConfig == nullptr)
	{
		InputConfig = NewObject<UMassifInputConfig>(this);
		InputConfig->Build();
	}
	return InputConfig;
}

void AMassifPlayerController::BeginPlay()
{
	Super::BeginPlay();

	EnsureInputConfig();
	ApplyMappingContexts();

	FlyPawn = GetPawn();
	SetInputMode(FInputModeGameOnly());
	bShowMouseCursor = false;
}

void AMassifPlayerController::ApplyMappingContexts()
{
	UEnhancedInputLocalPlayerSubsystem* Subsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer());
	if (Subsystem == nullptr)
	{
		return;
	}

	Subsystem->ClearAllMappings();
	Subsystem->AddMappingContext(EnsureInputConfig()->CommonContext, 0);
	if (ViewMode == EMassifViewMode::Walk)
	{
		Subsystem->AddMappingContext(InputConfig->WalkContext, 1);
		return;
	}
	Subsystem->AddMappingContext(InputConfig->FlyContext, 1);
}

void AMassifPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent);
	if (Input == nullptr)
	{
		return;
	}
	Input->BindAction(EnsureInputConfig()->ToggleViewAction, ETriggerEvent::Started, this,
		&AMassifPlayerController::ToggleViewMode);

	const TArray<TObjectPtr<UInputAction>>& ChannelActions = InputConfig->DebugChannelActions;
	for (int32 ChannelIndex = 0; ChannelIndex < ChannelActions.Num(); ++ChannelIndex)
	{
		Input->BindAction(ChannelActions[ChannelIndex], ETriggerEvent::Started, this,
			&AMassifPlayerController::SelectDebugChannel, ChannelIndex);
	}
}

void AMassifPlayerController::SelectDebugChannel(int32 ChannelIndex)
{
	AMassifDebugChannelView::RequestChannel(static_cast<EMassifDebugChannel>(ChannelIndex));
}

void AMassifPlayerController::ToggleViewMode()
{
	if (ViewMode == EMassifViewMode::Fly)
	{
		EnterWalkMode();
		return;
	}
	EnterFlyMode();
}

void AMassifPlayerController::EnterWalkMode()
{
	AMassifWalkerPawn* Walker = AcquireWalkerPawn();
	if (Walker == nullptr)
	{
		return;
	}

	FlyPawn = GetPawn();
	Possess(Walker);
	ViewMode = EMassifViewMode::Walk;
	ApplyMappingContexts();
}

void AMassifPlayerController::EnterFlyMode()
{
	if (FlyPawn == nullptr)
	{
		return;
	}

	// The camera picks up where the walker was looking from, so the switch reads
	// as stepping back rather than as a teleport.
	FlyPawn->SetActorLocation(PlayerCameraManager->GetCameraLocation());
	Possess(FlyPawn);
	ViewMode = EMassifViewMode::Fly;
	ApplyMappingContexts();
}

AMassifWalkerPawn* AMassifPlayerController::AcquireWalkerPawn()
{
	const AMassifGameMode* GameMode = GetWorld()->GetAuthGameMode<AMassifGameMode>();
	TSharedPtr<const FMassifTerrainField> TerrainField;
	if (GameMode != nullptr)
	{
		TerrainField = GameMode->GetTerrainField();
	}

	const FVector CameraCentimeters = PlayerCameraManager->GetCameraLocation();
	const FVector2d PositionMeters(
		CameraCentimeters.X / MetersToCentimeters, CameraCentimeters.Y / MetersToCentimeters);

	if (WalkerPawn == nullptr)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		WalkerPawn = GetWorld()->SpawnActor<AMassifWalkerPawn>(SpawnParameters);
		WalkerPawn->SetTerrainField(TerrainField);
	}
	if (!TerrainField.IsValid())
	{
		return WalkerPawn;
	}

	const double GroundMeters = TerrainField->SampleHeightMeters(PositionMeters, GroundSampleSpacingMeters);
	const double StandMeters = GroundMeters + WalkerPawn->GetEyeHeightMeters() + WalkerDropClearanceMeters;
	WalkerPawn->SetActorLocation(
		FVector(CameraCentimeters.X, CameraCentimeters.Y, StandMeters * MetersToCentimeters));
	return WalkerPawn;
}
