#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassifDaylightManager.generated.h"

class ADirectionalLight;
class ASkyLight;

/**
 * Points the sun where the wall clock says it is (ARCHITECTURE §6.1).
 *
 * Without this the time of day is a number the fog reads and nothing else, and
 * the shadows on the mountain disagree with the fog burning off under them. With
 * it, one clock drives both, and anything that reads the hour next joins them
 * rather than keeping an override of its own.
 *
 * Purely presentation: reads the clock, writes a light.
 */
UCLASS()
class AMassifDaylightManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifDaylightManager();

	void Initialize(ADirectionalLight* InSunLight, ASkyLight* InSkyLight, double InLatitudeDegrees);

	virtual void Tick(float DeltaSeconds) override;

private:
	/**
	 * How far the sun must move before the sky is captured again.
	 *
	 * A real-time capture every frame is the most expensive thing on this actor by
	 * a wide margin, and the sky does not change visibly over a minute of a
	 * nineteen-minute day.
	 */
	static constexpr double SkyRecaptureDegrees = 2.0;

	/** Below the horizon there is nothing to light with, so the light is switched off rather than aimed. */
	static constexpr double MinimumSunAltitudeDegrees = -2.0;

	void AimSun(double LatitudeDegrees);

	UPROPERTY()
	TObjectPtr<ADirectionalLight> SunLight;

	UPROPERTY()
	TObjectPtr<ASkyLight> SkyLight;

	double LatitudeDegrees = 47.2;
	double LastCaptureAltitudeDegrees = -1000.0;
};
