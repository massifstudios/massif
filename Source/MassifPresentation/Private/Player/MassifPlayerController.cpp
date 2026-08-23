#include "MassifPlayerController.h"

#include "Debug/MassifDebugChannelView.h"
#include "Debug/MassifViewPicker.h"
#include "UI/MassifEnvironmentSettingsScreen.h"
#include "UI/MassifWorldControlsPanel.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "MassifGameMode.h"
#include "MassifInputConfig.h"
#include "MassifTerrainField.h"
#include "MassifUnits.h"
#include "MassifFlyPawn.h"
#include "MassifWalkerPawn.h"

using MassifUnits::CentimetersToMeters;
using MassifUnits::MetersToCentimeters;

namespace
{
	/** Clearance above the field when the walker is dropped in, so it lands rather than starts embedded. */
	constexpr double WalkerDropClearanceMeters = 1.0;

	constexpr double SpawnProbeSpacingMeters = 1.0;

	/** How far back the camera picks the walker up from, so stepping out reverses stepping in. */
	constexpr double ReturnToCameraZoomMeters = 120.0;
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
	ShowCursor();
	MassifWorldControlsPanel::Show(GetWorld());
}

void AMassifPlayerController::ShowCursor()
{
	// A building game is played with a pointer. The cursor is shown and stays
	// shown, and the game keeps receiving keys alongside it, so the panels in the
	// corner and the camera under them are both usable without a mode switch.
	FInputModeGameAndUI InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	InputMode.SetHideCursorDuringCapture(false);
	SetInputMode(InputMode);
	bShowMouseCursor = true;
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

	Input->BindAction(InputConfig->ToggleViewPickerAction, ETriggerEvent::Started, this,
		&AMassifPlayerController::ToggleViewPicker);

	Input->BindAction(InputConfig->ToggleSettingsAction, ETriggerEvent::Started, this,
		&AMassifPlayerController::ToggleSettings);
}

void AMassifPlayerController::ToggleSettings()
{
	MassifEnvironmentSettingsScreen::Toggle(GetWorld());
}

void AMassifPlayerController::ToggleViewPicker()
{
	MassifViewPicker::Toggle();
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

void AMassifPlayerController::EnterWalkModeAt(FVector2d GroundPositionMeters)
{
	if (ViewMode == EMassifViewMode::Walk)
	{
		return;
	}

	WalkerDropGroundMeters = GroundPositionMeters;
	bHasWalkerDropPoint = true;
	EnterWalkMode();
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

	// The camera frames the ground the walker was standing on rather than taking
	// its eye position, so zooming back out is the reverse of zooming in.
	const FVector WalkerCentimeters = PlayerCameraManager->GetCameraLocation();
	AMassifFlyPawn* Camera = Cast<AMassifFlyPawn>(FlyPawn);
	if (Camera != nullptr)
	{
		Camera->FrameGroundPoint(
			FVector2d(WalkerCentimeters.X * CentimetersToMeters, WalkerCentimeters.Y * CentimetersToMeters),
			ReturnToCameraZoomMeters);
	}

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

	FVector2d PositionMeters(
		PlayerCameraManager->GetCameraLocation().X * CentimetersToMeters,
		PlayerCameraManager->GetCameraLocation().Y * CentimetersToMeters);

	// Zooming all the way in names the spot; the view switch key does not, and
	// falls back to whatever the camera was over.
	if (bHasWalkerDropPoint)
	{
		PositionMeters = WalkerDropGroundMeters;
		bHasWalkerDropPoint = false;
	}

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

	const double GroundMeters = TerrainField->SampleHeightMeters(PositionMeters, SpawnProbeSpacingMeters);
	const double StandMeters = GroundMeters + WalkerPawn->GetEyeHeightMeters() + WalkerDropClearanceMeters;
	WalkerPawn->SetActorLocation(FVector(
		PositionMeters.X * MetersToCentimeters, PositionMeters.Y * MetersToCentimeters,
		StandMeters * MetersToCentimeters));
	return WalkerPawn;
}
