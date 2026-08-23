#include "Weather/MassifDaylightManager.h"

#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Weather/MassifWorldTime.h"

AMassifDaylightManager::AMassifDaylightManager()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AMassifDaylightManager::Initialize(ADirectionalLight* InSunLight, double InLatitudeDegrees)
{
	SunLight = InSunLight;
	LatitudeDegrees = InLatitudeDegrees;

	if (SunLight != nullptr)
	{
		FullIntensity = SunLight->GetLightComponent()->Intensity;
	}
}

void AMassifDaylightManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (SunLight == nullptr)
	{
		return;
	}

	AimSun();
}

void AMassifDaylightManager::AimSun()
{
	const FMassifSunPosition Sun = MassifWorldTime::GetSunPosition(GetWorld(), LatitudeDegrees);

	// The light travels the way the sun is looked at, so the rotation points down
	// the direction to the sun rather than along it. The atmosphere reads the same
	// rotation, which is what repaints the sky.
	SunLight->SetActorRotation((-Sun.Direction).Rotation());

	const double AltitudeDegrees = FMath::RadiansToDegrees(Sun.AltitudeRadians);
	const double NightFade = FMath::GetMappedRangeValueClamped(
		FVector2D(FadeEndAltitudeDegrees, FadeStartAltitudeDegrees), FVector2D(0.0, 1.0), AltitudeDegrees);

	SunLight->GetLightComponent()->SetIntensity(FullIntensity * static_cast<float>(NightFade));
}
