#include "Weather/MassifDaylightManager.h"

#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Weather/MassifWorldTime.h"

AMassifDaylightManager::AMassifDaylightManager()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AMassifDaylightManager::Initialize(
	ADirectionalLight* InSunLight, ASkyLight* InSkyLight, double InLatitudeDegrees)
{
	SunLight = InSunLight;
	SkyLight = InSkyLight;
	LatitudeDegrees = InLatitudeDegrees;
}

void AMassifDaylightManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (SunLight == nullptr)
	{
		return;
	}

	AimSun(LatitudeDegrees);
}

void AMassifDaylightManager::AimSun(double InLatitudeDegrees)
{
	const FMassifSunPosition Sun = MassifWorldTime::GetSunPosition(GetWorld(), InLatitudeDegrees);
	const double AltitudeDegrees = FMath::RadiansToDegrees(Sun.AltitudeRadians);

	SunLight->SetActorHiddenInGame(AltitudeDegrees < MinimumSunAltitudeDegrees);

	// The light travels the way the sun is looked at, so the rotation points down
	// the direction to the sun rather than along it.
	SunLight->SetActorRotation((-Sun.Direction).Rotation());

	if (SkyLight == nullptr)
	{
		return;
	}

	if (FMath::Abs(AltitudeDegrees - LastCaptureAltitudeDegrees) < SkyRecaptureDegrees)
	{
		return;
	}

	LastCaptureAltitudeDegrees = AltitudeDegrees;
	SkyLight->GetLightComponent()->RecaptureSky();
}
