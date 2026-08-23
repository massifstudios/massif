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

	// Loud rather than silent. Without a light this actor does nothing at all, and
	// the one way that happens is being handed one before it has been spawned — in
	// which case the sun sits wherever it was posed and nothing says why.
	if (SunLight == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("Daylight: no sun light, so the sky will not follow the clock."));
		return;
	}

	FullIntensity = SunLight->GetLightComponent()->Intensity;
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
	LogSun(AltitudeDegrees, Sun.GetAzimuthRadians());
}

void AMassifDaylightManager::LogSun(double AltitudeDegrees, double AzimuthRadians)
{
	// Where the sun stands is a thing only a look can judge, so what is logged is
	// the number behind it: an hour that advances and an altitude that follows it.
	const int32 WholeDegrees = FMath::FloorToInt32(AltitudeDegrees);
	if (WholeDegrees == LastLoggedAltitudeDegrees)
	{
		return;
	}
	LastLoggedAltitudeDegrees = WholeDegrees;

	const FMassifWallClock Clock = MassifWorldTime::GetWallClock(GetWorld());
	UE_LOG(LogTemp, Display, TEXT("Daylight: %02d:%02d, sun %.1f degrees up, bearing %.0f"),
		Clock.GetHour(), Clock.GetMinute(), AltitudeDegrees, FMath::RadiansToDegrees(AzimuthRadians));
}
