#include "MassifPlayerController.h"

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

const UMassifInputConfig* AMassifPlayerController::GetInputConfig(const AController* Controller)
{
	const AMassifPlayerController* PlayerController = Cast<AMassifPlayerController>(Controller);
	if (PlayerController == nullptr)
	{
		return nullptr;
	}
	return PlayerController->InputConfig;
}

void AMassifPlayerController::BeginPlay()
{
	Super::BeginPlay();

	InputConfig = NewObject<UMassifInputConfig>(this);
	InputConfig->Build();

	UEnhancedInputLocalPlayerSubsystem* Subsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer());
	if (Subsystem != nullptr)
	{
		Subsystem->AddMappingContext(InputConfig->CommonContext, 0);
		Subsystem->AddMappingContext(InputConfig->FlyContext, 1);
	}

	FlyPawn = GetPawn();
	SetInputMode(FInputModeGameOnly());
	bShowMouseCursor = false;
}

void AMassifPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent);
	if (Input == nullptr || InputConfig == nullptr)
	{
		return;
	}
	Input->BindAction(InputConfig->ToggleViewAction, ETriggerEvent::Started, this,
		&AMassifPlayerController::ToggleViewMode);
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
	SwapMappingContext(InputConfig->FlyContext, InputConfig->WalkContext);
	ViewMode = EMassifViewMode::Walk;
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
	SwapMappingContext(InputConfig->WalkContext, InputConfig->FlyContext);
	ViewMode = EMassifViewMode::Fly;
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

void AMassifPlayerController::SwapMappingContext(
	const UInputMappingContext* Leaving, const UInputMappingContext* Entering)
{
	UEnhancedInputLocalPlayerSubsystem* Subsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer());
	if (Subsystem == nullptr)
	{
		return;
	}
	Subsystem->RemoveMappingContext(Leaving);
	Subsystem->AddMappingContext(Entering, 1);
}
