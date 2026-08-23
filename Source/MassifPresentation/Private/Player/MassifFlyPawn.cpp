#include "MassifFlyPawn.h"

#include "EnhancedInputComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "MassifGameMode.h"
#include "MassifInputConfig.h"
#include "MassifPlayerController.h"
#include "MassifTerrainField.h"
#include "MassifUnits.h"

using MassifUnits::CentimetersToMeters;
using MassifUnits::MetersToCentimeters;

namespace
{
	/** Spacing the camera reads the ground at. Coarse: it is framing, not standing on it. */
	constexpr double CameraProbeSpacingMeters = 4.0;

	/** Steps the cursor ray is marched in before it is refined, and how far it may reach. */
	constexpr int32 GroundMarchStepCount = 96;
	constexpr double GroundMarchReachMeters = 40000.0;
	constexpr int32 GroundRefineStepCount = 12;

	/** How far a point along the ray stands above the ground. Negative is underground. */
	double RayClearanceMeters(
		const FMassifTerrainField& Field, FVector RayOrigin, FVector RayDirection, double DistanceMeters)
	{
		const FVector Sample = RayOrigin + RayDirection * (DistanceMeters * MetersToCentimeters);
		const FVector2d Ground(Sample.X * CentimetersToMeters, Sample.Y * CentimetersToMeters);
		return Sample.Z * CentimetersToMeters - Field.SampleHeightMeters(Ground, CameraProbeSpacingMeters);
	}

	/** Bisects the step the ray crossed the ground in, and answers where it landed. */
	FVector2d RefineGroundHit(
		const FMassifTerrainField& Field, FVector RayOrigin, FVector RayDirection, double AboveMeters,
		double BelowMeters)
	{
		for (int32 Refine = 0; Refine < GroundRefineStepCount; ++Refine)
		{
			const double MiddleMeters = 0.5 * (AboveMeters + BelowMeters);
			const bool bAbove = RayClearanceMeters(Field, RayOrigin, RayDirection, MiddleMeters) > 0.0;
			if (bAbove)
			{
				AboveMeters = MiddleMeters;
				continue;
			}
			BelowMeters = MiddleMeters;
		}

		const FVector Hit = RayOrigin + RayDirection * (BelowMeters * MetersToCentimeters);
		return FVector2d(Hit.X * CentimetersToMeters, Hit.Y * CentimetersToMeters);
	}
}

AMassifFlyPawn::AMassifFlyPawn()
{
	bAddDefaultMovementBindings = false;
	PrimaryActorTick.bCanEverTick = true;

	// The camera is placed outright rather than driven, so the movement component
	// must not also be pushing it around.
	UFloatingPawnMovement* Movement = Cast<UFloatingPawnMovement>(GetMovementComponent());
	if (Movement != nullptr)
	{
		Movement->MaxSpeed = 0.0f;
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
	Input->BindAction(Config->MoveAction, ETriggerEvent::Completed, this, &AMassifFlyPawn::ApplyMove);
	Input->BindAction(Config->LookAction, ETriggerEvent::Triggered, this, &AMassifFlyPawn::ApplyLook);
	Input->BindAction(Config->ZoomAction, ETriggerEvent::Triggered, this, &AMassifFlyPawn::ApplyZoom);
	Input->BindAction(Config->SprintAction, ETriggerEvent::Started, this, &AMassifFlyPawn::StartBoost);
	Input->BindAction(Config->SprintAction, ETriggerEvent::Completed, this, &AMassifFlyPawn::StopBoost);
	Input->BindAction(Config->OrbitAction, ETriggerEvent::Started, this, &AMassifFlyPawn::StartOrbit);
	Input->BindAction(Config->OrbitAction, ETriggerEvent::Completed, this, &AMassifFlyPawn::StopOrbit);
}

TSharedPtr<const FMassifTerrainField> AMassifFlyPawn::GetTerrainField() const
{
	const AMassifGameMode* GameMode = GetWorld()->GetAuthGameMode<AMassifGameMode>();
	if (GameMode == nullptr)
	{
		return nullptr;
	}
	return GameMode->GetTerrainField();
}

double AMassifFlyPawn::GetGroundAltitudeMeters(FVector2d GroundMeters) const
{
	const TSharedPtr<const FMassifTerrainField> Field = GetTerrainField();
	if (!Field.IsValid())
	{
		return 0.0;
	}
	return Field->SampleHeightMeters(GroundMeters, CameraProbeSpacingMeters);
}

void AMassifFlyPawn::FrameGroundPoint(FVector2d GroundPositionMeters, double DistanceMeters)
{
	FocusGroundMeters = GroundPositionMeters;
	ZoomDistanceMeters = FMath::Clamp(DistanceMeters, MinZoomMeters, MaxZoomMeters);
	bPlaced = false;
}

void AMassifFlyPawn::ApplyMove(const FInputActionValue& Value)
{
	PanInput = FVector2d(Value.Get<FVector2D>().X, Value.Get<FVector2D>().Y);
}

void AMassifFlyPawn::ApplyLook(const FInputActionValue& Value)
{
	// The cursor is always on screen in a building game, so the view only turns
	// while the orbit button is held. Otherwise every mouse move would drag the
	// world out from under a cursor the player is trying to point with.
	if (!bOrbiting)
	{
		return;
	}

	const FVector2D Axis = Value.Get<FVector2D>();
	YawDegrees += Axis.X;
	PitchDegrees = FMath::Clamp(PitchDegrees + Axis.Y, MinPitchDegrees, MaxPitchDegrees);
}

void AMassifFlyPawn::ApplyZoom(const FInputActionValue& Value)
{
	const double Notches = Value.Get<float>();
	if (FMath::IsNearlyZero(Notches))
	{
		return;
	}

	const double PreviousZoomMeters = ZoomDistanceMeters;
	ZoomDistanceMeters = FMath::Clamp(
		ZoomDistanceMeters * FMath::Pow(ZoomStepFactor, -Notches), MinZoomMeters, MaxZoomMeters);

	ZoomTowardsCursor(PreviousZoomMeters);
	TryHandOverToWalker();
}

bool AMassifFlyPawn::TryGetGroundUnderCursor(FVector2d& OutGroundMeters) const
{
	const APlayerController* Controller = Cast<APlayerController>(GetController());
	const TSharedPtr<const FMassifTerrainField> Field = GetTerrainField();
	if (Controller == nullptr || !Field.IsValid())
	{
		return false;
	}

	FVector RayOrigin;
	FVector RayDirection;
	if (!Controller->DeprojectMousePositionToWorld(RayOrigin, RayDirection))
	{
		return false;
	}

	// Marched rather than traced: the terrain is a field, not a collision body, and
	// most of it has no collision mesh built at all. A coarse march to the first
	// step that is underground, then a bisection inside that step.
	const double StepMeters = GroundMarchReachMeters / GroundMarchStepCount;
	double LastAboveMeters = 0.0;
	for (int32 StepIndex = 1; StepIndex <= GroundMarchStepCount; ++StepIndex)
	{
		const double DistanceMeters = StepIndex * StepMeters;
		if (RayClearanceMeters(*Field, RayOrigin, RayDirection, DistanceMeters) > 0.0)
		{
			LastAboveMeters = DistanceMeters;
			continue;
		}

		OutGroundMeters = RefineGroundHit(*Field, RayOrigin, RayDirection, LastAboveMeters, DistanceMeters);
		return true;
	}

	return false;
}

void AMassifFlyPawn::ZoomTowardsCursor(double PreviousZoomMeters)
{
	FVector2d CursorGroundMeters;
	if (!TryGetGroundUnderCursor(CursorGroundMeters))
	{
		return;
	}

	// The focus closes on the cursor by the same fraction the view closed on the
	// ground, so what is under the pointer stays under it as the camera comes in.
	const double ClosedFraction = 1.0 - ZoomDistanceMeters / FMath::Max(PreviousZoomMeters, 1.0);
	if (ClosedFraction <= 0.0)
	{
		return;
	}

	FocusGroundMeters = FMath::Lerp(FocusGroundMeters, CursorGroundMeters, FMath::Min(ClosedFraction, 1.0));
}

void AMassifFlyPawn::TryHandOverToWalker()
{
	if (ZoomDistanceMeters > HandOverZoomMeters)
	{
		return;
	}

	AMassifPlayerController* Controller = Cast<AMassifPlayerController>(GetController());
	if (Controller == nullptr)
	{
		return;
	}

	Controller->EnterWalkModeAt(FocusGroundMeters);
}

void AMassifFlyPawn::StartBoost()
{
	bBoosting = true;
}

void AMassifFlyPawn::StopBoost()
{
	bBoosting = false;
}

void AMassifFlyPawn::StartOrbit()
{
	bOrbiting = true;
}

void AMassifFlyPawn::StopOrbit()
{
	bOrbiting = false;
}

void AMassifFlyPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// The ground slides in the plane, along the view's yaw only. Pan speed scales
	// with zoom because a metre a second is a crawl over the whole massif and a
	// bolt across a lift station.
	double SpeedScale = 1.0;
	if (bBoosting)
	{
		SpeedScale = BoostFactor;
	}

	const double SpeedMetersPerSecond = ZoomDistanceMeters * PanSpeedPerZoom * SpeedScale;
	const FRotator YawOnly(0.0, YawDegrees, 0.0);
	const FVector Forward = YawOnly.RotateVector(FVector::ForwardVector);
	const FVector Right = YawOnly.RotateVector(FVector::RightVector);
	const FVector Slide = (Forward * PanInput.Y + Right * PanInput.X) * SpeedMetersPerSecond * DeltaSeconds;

	FocusGroundMeters += FVector2d(Slide.X, Slide.Y);
	PlaceCamera(DeltaSeconds);
}

void AMassifFlyPawn::PlaceCamera(double DeltaSeconds)
{
	const double FocusAltitudeMeters = GetGroundAltitudeMeters(FocusGroundMeters);
	const FVector FocusCentimeters(
		FocusGroundMeters.X * MetersToCentimeters, FocusGroundMeters.Y * MetersToCentimeters,
		FocusAltitudeMeters * MetersToCentimeters);

	const FRotator ViewRotation(PitchDegrees, YawDegrees, 0.0);
	const FVector Back = -ViewRotation.Vector();
	const FVector Target = FocusCentimeters + Back * (ZoomDistanceMeters * MetersToCentimeters);

	AController* Owner = GetController();
	if (Owner != nullptr)
	{
		Owner->SetControlRotation(ViewRotation);
	}

	if (!bPlaced)
	{
		bPlaced = true;
		SetActorLocationAndRotation(Target, ViewRotation);
		return;
	}

	// Smoothed towards the target rather than snapped, so a wheel notch reads as a
	// move in rather than a cut.
	const double Alpha = FMath::Clamp(SmoothingRate * DeltaSeconds, 0.0, 1.0);
	SetActorLocationAndRotation(
		FMath::Lerp(GetActorLocation(), Target, Alpha), ViewRotation);
}
