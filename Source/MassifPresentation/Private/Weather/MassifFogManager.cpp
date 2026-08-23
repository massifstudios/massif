#include "Weather/MassifFogManager.h"

#include "Components/LocalFogVolumeComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "MassifEnvironmentState.h"
#include "MassifSimClock.h"
#include "MassifSimSubsystem.h"
#include "MassifTerrainField.h"
#include "MassifUnits.h"

namespace
{
	/**
	 * Time of day the fog is drawn at, overriding the wall clock. Negative to
	 * follow the clock.
	 *
	 * Fog is a thing that happens at six in the morning, and waiting for six in the
	 * morning is not a way to look at it.
	 */
	TAutoConsoleVariable<float> CVarFogTimeOfDay(
		TEXT("massif.Fog.TimeOfDay"), -1.0f,
		TEXT("Hour of day the fog is solved at, 0 to 24. Negative follows the wall clock."),
		ECVF_Cheat);

	TAutoConsoleVariable<int32> CVarFogVolumes(
		TEXT("massif.Fog.Volumes"), 1,
		TEXT("Whether the local fog volumes are drawn at all. The distance haze is unaffected."),
		ECVF_Cheat);

	/** Days into the year the world is played on, until a calendar exists to say otherwise. */
	constexpr double MidwinterDayOfYear = 20.0;

	double SolveDayFraction(const UWorld* World)
	{
		const float ForcedHour = CVarFogTimeOfDay.GetValueOnGameThread();
		if (ForcedHour >= 0.0f)
		{
			return FMath::Frac(ForcedHour / 24.0f);
		}

		const UGameInstance* GameInstance = World->GetGameInstance();
		const UMassifSimSubsystem* Sim = GameInstance->GetSubsystem<UMassifSimSubsystem>();
		if (Sim == nullptr)
		{
			return 0.0;
		}

		return MassifWallClockAtTick(Sim->GetState().TickCount).DayFraction;
	}
}

AMassifFogManager::AMassifFogManager()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AMassifFogManager::Initialize(
	TSharedRef<const FMassifEnvironmentState> InEnvironment,
	TSharedPtr<const FMassifTerrainField> InTerrainField, double InLatitudeDegrees)
{
	Environment = InEnvironment;
	TerrainField = InTerrainField;
	LatitudeDegrees = InLatitudeDegrees;
	Field = MakeUnique<FMassifFogField>(InEnvironment, Params);

	FloorAltitudeMeters = 0.0;
	if (TerrainField.IsValid())
	{
		FloorAltitudeMeters = TerrainField->GetMinAltitudeMeters();
	}
}

bool AMassifFogManager::TryGetCameraPositionMeters(FVector2d& OutPositionMeters) const
{
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (Controller == nullptr || Controller->PlayerCameraManager == nullptr)
	{
		return false;
	}

	const FVector Location = Controller->PlayerCameraManager->GetCameraLocation();
	OutPositionMeters = FVector2d(
		Location.X * MassifUnits::CentimetersToMeters, Location.Y * MassifUnits::CentimetersToMeters);
	return true;
}

void AMassifFogManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!Field.IsValid())
	{
		return;
	}

	SecondsSinceSolve += DeltaSeconds;
	if (SecondsSinceSolve < 1.0 / UpdatesPerSecond)
	{
		return;
	}
	SecondsSinceSolve = 0.0;

	FVector2d CameraPositionMeters;
	if (!TryGetCameraPositionMeters(CameraPositionMeters))
	{
		return;
	}

	SolveFog(CameraPositionMeters);
}

void AMassifFogManager::SolveFog(FVector2d CameraPositionMeters)
{
	Conditions = MassifSolveFogConditions(
		Params, FloorAltitudeMeters, LatitudeDegrees, MidwinterDayOfYear, SolveDayFraction(GetWorld()));

	if (!Conditions.IsAnyFog() || CVarFogVolumes.GetValueOnGameThread() == 0)
	{
		HideVolumesFrom(0);
		return;
	}

	const FMassifSunPosition Sun = MassifSunAtTimeOfDay(
		LatitudeDegrees, MidwinterDayOfYear, SolveDayFraction(GetWorld()));

	// Snapped to the grid rather than centred on the camera, so a volume does not
	// slide along under a camera that is panning.
	const int32 CellRadius = FMath::CeilToInt32(VisibleRadiusMeters / CellMeters);
	const FIntPoint CentreCell(
		FMath::RoundToInt32(CameraPositionMeters.X / CellMeters),
		FMath::RoundToInt32(CameraPositionMeters.Y / CellMeters));

	int32 VolumeIndex = 0;
	for (int32 OffsetY = -CellRadius; OffsetY <= CellRadius; ++OffsetY)
	{
		VolumeIndex = PlaceRow(CentreCell, OffsetY, CellRadius, Sun, VolumeIndex);
	}

	HideVolumesFrom(VolumeIndex);
	LogSolve(VolumeIndex);
}

void AMassifFogManager::LogSolve(int32 DrawnVolumeCount)
{
	// Fog is only judgeable by looking at it, so what is logged is the numbers
	// behind it: whether the solve believes there is fog, how deep, and how much of
	// the ground near the camera came out under the lid.
	if (DrawnVolumeCount == LastLoggedVolumeCount)
	{
		return;
	}
	LastLoggedVolumeCount = DrawnVolumeCount;

	UE_LOG(LogTemp, Display,
		TEXT("Fog: strength %.2f, lid %.0f m (floor %.0f m), %d volumes drawn"),
		Conditions.Strength, Conditions.LidAltitudeMeters, FloorAltitudeMeters, DrawnVolumeCount);
}

int32 AMassifFogManager::PlaceRow(
	FIntPoint CentreCell, int32 OffsetY, int32 CellRadius, const FMassifSunPosition& Sun, int32 VolumeIndex)
{
	for (int32 OffsetX = -CellRadius; OffsetX <= CellRadius; ++OffsetX)
	{
		const FVector2d CellCentre(
			(CentreCell.X + OffsetX) * CellMeters, (CentreCell.Y + OffsetY) * CellMeters);
		VolumeIndex = PlaceCell(CellCentre, Sun, VolumeIndex);
	}
	return VolumeIndex;
}

int32 AMassifFogManager::PlaceCell(
	FVector2d CellCentreMeters, const FMassifSunPosition& Sun, int32 VolumeIndex)
{
	if (VolumeIndex >= MaxVolumeCount)
	{
		return VolumeIndex;
	}

	const double Density = Field->DensityAt(CellCentreMeters, Conditions, Sun);
	if (Density < MinimumDrawnDensity)
	{
		return VolumeIndex;
	}

	const double GroundAltitude = Field->GetAltitudeAt(CellCentreMeters);
	const double ColumnMeters = Conditions.LidAltitudeMeters - GroundAltitude;
	if (ColumnMeters < MinimumColumnMeters)
	{
		return VolumeIndex;
	}

	ULocalFogVolumeComponent* Volume = GetOrCreateVolume(VolumeIndex);

	// The ellipsoid spans the column between the ground and the lid, so its top is
	// the flat surface of the lake and its bottom is the ground under it.
	const double CentreAltitude = GroundAltitude + 0.5 * ColumnMeters;
	Volume->SetWorldLocation(FVector(
		CellCentreMeters.X * MassifUnits::MetersToCentimeters,
		CellCentreMeters.Y * MassifUnits::MetersToCentimeters,
		CentreAltitude * MassifUnits::MetersToCentimeters));

	const double HorizontalRadiusCentimeters =
		0.5 * CellMeters * CellOverlap * MassifUnits::MetersToCentimeters;
	const double VerticalRadiusCentimeters = 0.5 * ColumnMeters * MassifUnits::MetersToCentimeters;
	const double BaseSize = ULocalFogVolumeComponent::GetBaseVolumeSize();
	Volume->SetWorldScale3D(FVector(
		HorizontalRadiusCentimeters / BaseSize, HorizontalRadiusCentimeters / BaseSize,
		VerticalRadiusCentimeters / BaseSize));

	Volume->SetHeightFogExtinction(ExtinctionScale * static_cast<float>(Density));
	Volume->SetRadialFogExtinction(ExtinctionScale * RadialExtinctionShare * static_cast<float>(Density));
	Volume->SetHeightFogFalloff(HeightFalloff);
	Volume->SetFogPhaseG(PhaseG);
	Volume->SetVisibility(true);
	return VolumeIndex + 1;
}

ULocalFogVolumeComponent* AMassifFogManager::GetOrCreateVolume(int32 VolumeIndex)
{
	if (Volumes.IsValidIndex(VolumeIndex))
	{
		return Volumes[VolumeIndex];
	}

	ULocalFogVolumeComponent* Volume = NewObject<ULocalFogVolumeComponent>(this);
	Volume->SetupAttachment(RootComponent);
	Volume->RegisterComponent();
	Volumes.Add(Volume);
	return Volume;
}

void AMassifFogManager::HideVolumesFrom(int32 FirstUnusedIndex)
{
	for (int32 Index = FirstUnusedIndex; Index < Volumes.Num(); ++Index)
	{
		Volumes[Index]->SetVisibility(false);
	}
}
