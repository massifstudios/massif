#include "MassifFlyPawn.h"

#include "EnhancedInputComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "MassifInputConfig.h"
#include "MassifPlayerController.h"

AMassifFlyPawn::AMassifFlyPawn()
{
	bAddDefaultMovementBindings = false;

	UFloatingPawnMovement* Movement = Cast<UFloatingPawnMovement>(GetMovementComponent());
	if (Movement != nullptr)
	{
		Movement->MaxSpeed = CruiseSpeed;
		Movement->Acceleration = 2.0f * CruiseSpeed;
		Movement->Deceleration = 4.0f * CruiseSpeed;
	}
}

void AMassifFlyPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	const UMassifInputConfig* Config = AMassifPlayerController::GetInputConfig(GetController());
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (Config == nullptr || Input == nullptr)
	{
		return;
	}

	Input->BindAction(Config->MoveAction, ETriggerEvent::Triggered, this, &AMassifFlyPawn::ApplyMove);
	Input->BindAction(Config->LookAction, ETriggerEvent::Triggered, this, &AMassifFlyPawn::ApplyLook);
	Input->BindAction(Config->VerticalAction, ETriggerEvent::Triggered, this, &AMassifFlyPawn::ApplyVertical);
	Input->BindAction(Config->SprintAction, ETriggerEvent::Started, this, &AMassifFlyPawn::StartBoost);
	Input->BindAction(Config->SprintAction, ETriggerEvent::Completed, this, &AMassifFlyPawn::StopBoost);
}

void AMassifFlyPawn::ApplyMove(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	const FRotator ViewRotation = GetControlRotation();
	AddMovementInput(ViewRotation.RotateVector(FVector::ForwardVector), Axis.Y);
	AddMovementInput(ViewRotation.RotateVector(FVector::RightVector), Axis.X);
}

void AMassifFlyPawn::ApplyLook(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);
}

void AMassifFlyPawn::ApplyVertical(const FInputActionValue& Value)
{
	AddMovementInput(FVector::UpVector, Value.Get<float>());
}

void AMassifFlyPawn::StartBoost()
{
	SetMaxSpeed(BoostSpeed);
}

void AMassifFlyPawn::StopBoost()
{
	SetMaxSpeed(CruiseSpeed);
}

void AMassifFlyPawn::SetMaxSpeed(float MaxSpeed)
{
	UFloatingPawnMovement* Movement = Cast<UFloatingPawnMovement>(GetMovementComponent());
	if (Movement == nullptr)
	{
		return;
	}
	Movement->MaxSpeed = MaxSpeed;
	Movement->Acceleration = 2.0f * MaxSpeed;
	Movement->Deceleration = 4.0f * MaxSpeed;
}
