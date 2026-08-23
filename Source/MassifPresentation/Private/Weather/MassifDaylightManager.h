#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassifDaylightManager.generated.h"

class ADirectionalLight;

/**
 * Points the sun where the wall clock says it is (ARCHITECTURE §6.1).
 *
 * Without this the time of day is a number the fog reads and nothing else, and
 * the shadows on the mountain disagree with the fog burning off under them. With
 * it, one clock drives the light, the sky and the fog together, and whatever
 * reads the hour next joins them rather than keeping an override of its own.
 *
 * The sky is not driven directly and must not be. The directional light is the
 * atmosphere's sun light, so `SkyAtmosphere` recolours the whole sky from its
 * direction on its own, and the sky light captures that in real time. Aiming the
 * light is therefore the entire day cycle: sunrise reddening, the blue of noon
 * and the dark of night all come out of one scattering model instead of a curve
 * per system that would drift apart from each other.
 *
 * Purely presentation: reads the clock, writes a light.
 */
UCLASS()
class AMassifDaylightManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifDaylightManager();

	void Initialize(ADirectionalLight* InSunLight, double InLatitudeDegrees);

	virtual void Tick(float DeltaSeconds) override;

private:
	/**
	 * Sun altitudes the light fades out between, in degrees.
	 *
	 * The atmosphere already dims and reddens the light along its own path
	 * (`bPerPixelAtmosphereTransmittance`), which is what carries sunset. This is
	 * only the backstop under it: below the horizon a directional light would
	 * otherwise keep lighting the ground from underneath, and the fade takes it out
	 * over the span of civil twilight so nothing snaps off at the moment of sunset.
	 */
	static constexpr double FadeStartAltitudeDegrees = -1.0;
	static constexpr double FadeEndAltitudeDegrees = -9.0;

	void AimSun();

	UPROPERTY()
	TObjectPtr<ADirectionalLight> SunLight;

	/** What the light was built with, which the fade scales rather than replaces. */
	float FullIntensity = 0.0f;

	double LatitudeDegrees = 47.2;
};
