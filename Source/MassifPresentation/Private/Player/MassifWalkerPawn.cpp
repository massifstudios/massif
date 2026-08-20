#include "MassifWalkerPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "MassifInputConfig.h"
#include "MassifPlayerController.h"
#include "MassifTerrainField.h"

namespace
{
	constexpr double MetersToCentimeters = 100.0;

	/** Sampling spacing for the catch: the ground the pawn stands on, not the ridge behind it. */
	constexpr double GroundSampleSpacingMeters = 1.0;
}

AMassifWalkerPawn::AMassifWalkerPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(GetCapsuleComponent());
	Camera->SetRelativeLocation(FVector(0.0f, 0.0f, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 15.0f));
	Camera->bUsePawnControlRotation = true;

	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->MaxWalkSpeed = WalkSpeed;
	Movement->JumpZVelocity = JumpSpeed;
	Movement->SetWalkableFloorAngle(WalkableFloorAngleDegrees);
	Movement->bOrientRotationToMovement = false;
	Movement->MaxStepHeight = 60.0f;
	Movement->AirControl = 0.4f;
}

void AMassifWalkerPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	const UMassifInputConfig* Config = AMassifPlayerController::GetInputConfig(GetController());
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (Config == nullptr || Input == nullptr)
	{
		return;
	}

	Input->BindAction(Config->MoveAction, ETriggerEvent::Triggered, this, &AMassifWalkerPawn::ApplyMove);
	Input->BindAction(Config->LookAction, ETriggerEvent::Triggered, this, &AMassifWalkerPawn::ApplyLook);
	Input->BindAction(Config->JumpAction, ETriggerEvent::Started, this, &AMassifWalkerPawn::StartJump);
	Input->BindAction(Config->JumpAction, ETriggerEvent::Completed, this, &AMassifWalkerPawn::StopJump);
	Input->BindAction(Config->SprintAction, ETriggerEvent::Started, this, &AMassifWalkerPawn::StartRun);
	Input->BindAction(Config->SprintAction, ETriggerEvent::Completed, this, &AMassifWalkerPawn::StopRun);
}

void AMassifWalkerPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	CatchFallThroughGround();
}

double AMassifWalkerPawn::GetEyeHeightMeters() const
{
	return GetCapsuleComponent()->GetScaledCapsuleHalfHeight() / MetersToCentimeters;
}

void AMassifWalkerPawn::SetTerrainField(TSharedPtr<const FMassifTerrainField> InField)
{
	TerrainField = InField;
}

void AMassifWalkerPawn::ApplyMove(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	const FRotator HeadingRotation(0.0f, GetControlRotation().Yaw, 0.0f);
	AddMovementInput(HeadingRotation.RotateVector(FVector::ForwardVector), Axis.Y);
	AddMovementInput(HeadingRotation.RotateVector(FVector::RightVector), Axis.X);
}

void AMassifWalkerPawn::ApplyLook(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);
}

void AMassifWalkerPawn::StartJump()
{
	Jump();
}

void AMassifWalkerPawn::StopJump()
{
	StopJumping();
}

void AMassifWalkerPawn::StartRun()
{
	GetCharacterMovement()->MaxWalkSpeed = RunSpeed;
}

void AMassifWalkerPawn::StopRun()
{
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
}

void AMassifWalkerPawn::CatchFallThroughGround()
{
	if (!TerrainField.IsValid())
	{
		return;
	}

	const FVector LocationCentimeters = GetActorLocation();
	const FVector2d PositionMeters(
		LocationCentimeters.X / MetersToCentimeters, LocationCentimeters.Y / MetersToCentimeters);
	const double GroundMeters = TerrainField->SampleHeightMeters(PositionMeters, GroundSampleSpacingMeters);
	const double FeetMeters = LocationCentimeters.Z / MetersToCentimeters - GetEyeHeightMeters();
	if (FeetMeters > GroundMeters - MaxSinkMeters)
	{
		return;
	}

	// Tiles cook their collision as the player nears them, so a walker that
	// outruns the cook drops through ground that is drawn but not yet solid.
	const double StandMeters = GroundMeters + GetEyeHeightMeters();
	SetActorLocation(FVector(LocationCentimeters.X, LocationCentimeters.Y, StandMeters * MetersToCentimeters));
	GetCharacterMovement()->Velocity = FVector::ZeroVector;
}
