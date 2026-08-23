#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SpectatorPawn.h"
#include "MassifFlyPawn.generated.h"

class FMassifTerrainField;
struct FInputActionValue;

/**
 * The management camera: a point on the ground, and a distance back from it.
 *
 * Not a free-flying camera. A building game is played by looking at a place
 * rather than by flying to one, so WASD slides the ground under the view and the
 * wheel changes how far back the camera sits from it. Height above the terrain
 * follows from the zoom rather than being steered, which is what stops the
 * camera being flown into a mountain or left pointing at the sky.
 *
 * The wheel zooms towards whatever is under the cursor, so a valley is reached
 * by pointing at it and rolling in. Rolling past the closest step hands over to
 * the walker standing at that spot (ARCHITECTURE §3: presentation only, the
 * camera holds no sim state).
 */
UCLASS()
class AMassifFlyPawn : public ASpectatorPawn
{
	GENERATED_BODY()

public:
	AMassifFlyPawn();

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Puts the camera over a ground point at a given distance back, for a view change. */
	void FrameGroundPoint(FVector2d GroundPositionMeters, double DistanceMeters);

	/** The ground point the camera is looking at, which is where a walker would be dropped. */
	FVector2d GetFocusGroundMeters() const
	{
		return FocusGroundMeters;
	}

	double GetZoomDistanceMeters() const
	{
		return ZoomDistanceMeters;
	}

private:
	/** How far back the camera may sit, in metres. The far end frames the whole massif. */
	static constexpr double MinZoomMeters = 40.0;
	static constexpr double MaxZoomMeters = 14000.0;

	/**
	 * Zoom is geometric, not linear: one wheel notch is a fixed *fraction* of the
	 * distance, so the same flick covers a sensible step whether the camera is
	 * forty metres out or fourteen kilometres.
	 */
	static constexpr double ZoomStepFactor = 1.18;

	/** Below this the camera stops being a camera and the walker takes over. */
	static constexpr double HandOverZoomMeters = 45.0;

	/** Metres a second the ground slides at, per metre of zoom distance. */
	static constexpr double PanSpeedPerZoom = 0.9;
	static constexpr double BoostFactor = 3.5;

	/** How fast the camera catches up with where the input has put it. Higher is tighter. */
	static constexpr double SmoothingRate = 12.0;

	/** Pitch the camera looks down at, and the range the orbit drag may set. */
	static constexpr double DefaultPitchDegrees = -45.0;
	static constexpr double MinPitchDegrees = -89.0;
	static constexpr double MaxPitchDegrees = -8.0;

	void ApplyMove(const FInputActionValue& Value);
	void ApplyLook(const FInputActionValue& Value);
	void ApplyZoom(const FInputActionValue& Value);
	void StartBoost();
	void StopBoost();
	void StartOrbit();
	void StopOrbit();

	/** Where the cursor's ray meets the ground, falling back to the focus point off-terrain. */
	bool TryGetGroundUnderCursor(FVector2d& OutGroundMeters) const;

	/** Slides the focus towards the cursor as the view closes on it, so the wheel zooms where you point. */
	void ZoomTowardsCursor(double PreviousZoomMeters);

	/** Hands the view to the walker once the wheel has rolled past the closest step. */
	void TryHandOverToWalker();

	void PlaceCamera(double DeltaSeconds);

	TSharedPtr<const FMassifTerrainField> GetTerrainField() const;

	double GetGroundAltitudeMeters(FVector2d GroundMeters) const;

	/** Where the camera is looking, on the ground, in metres. */
	FVector2d FocusGroundMeters = FVector2d::ZeroVector;

	double ZoomDistanceMeters = 3000.0;

	/** Yaw the ground slides against and the camera sits behind. */
	double YawDegrees = 0.0;
	double PitchDegrees = DefaultPitchDegrees;

	FVector2d PanInput = FVector2d::ZeroVector;

	bool bBoosting = false;
	bool bOrbiting = false;

	/** False until the first frame has placed the camera, so it does not glide in from the origin. */
	bool bPlaced = false;
};
