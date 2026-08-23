#include "Weather/MassifFogManager.h"

#include "Components/LocalFogVolumeComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "MassifEnvironmentState.h"
#include "MassifTerrainField.h"
#include "Player/MassifFlyPawn.h"
#include "MassifUnits.h"
#include "Weather/MassifWorldTime.h"

namespace
{
	TAutoConsoleVariable<int32> CVarFogVolumes(
		TEXT("massif.Fog.Volumes"), 1,
		TEXT("Whether the local fog volumes are drawn at all. The distance haze is unaffected."),
		ECVF_Cheat);

	/**
	 * Ground one volume covers, and how far past its cell it reaches.
	 *
	 * The two together are what decide whether the layer reads as fog or as a row
	 * of clouds. A volume is an ellipsoid, so one that is as tall as it is wide is
	 * a ball; reach has to be several times the spacing for neighbours to merge
	 * into a sheet instead of sitting beside one another.
	 *
	 * A reach of six over a three hundred metre cell is a lot of overlap, and it is
	 * why the extinction beneath is a third of what a single volume would want: with
	 * that many layers stacked, each one contributes little. The two are tuned
	 * against each other and moving one alone will not hold the look.
	 */
	TAutoConsoleVariable<float> CVarFogCellMeters(
		TEXT("massif.Fog.CellMeters"), 300.0f,
		TEXT("Ground one fog volume covers, in metres."), ECVF_Cheat);

	TAutoConsoleVariable<float> CVarFogReach(
		TEXT("massif.Fog.Reach"), 6.0f,
		TEXT("Volume radius as a multiple of the cell. Above 1 the volumes overlap."), ECVF_Cheat);

	/**
	 * Share of the density carried by the volume's radial falloff rather than its
	 * height falloff.
	 *
	 * Zero by default, and this is the setting that decides whether fog looks like
	 * fog: the radial term falls off from the centre of the ellipsoid in every
	 * direction at once, which draws a sphere. The height term falls off upwards
	 * only, which draws a layer.
	 */
	TAutoConsoleVariable<float> CVarFogRadialShare(
		TEXT("massif.Fog.RadialShare"), 0.06f,
		TEXT("Share of fog density from the radial falloff. Above zero draws spheres."), ECVF_Cheat);

	/** Higher packs the fog into the bottom of its column, which is what gives the lake a top. */
	TAutoConsoleVariable<float> CVarFogFalloff(
		TEXT("massif.Fog.Falloff"), 4200.0f,
		TEXT("Height falloff inside each fog volume. Higher is a sharper top."), ECVF_Cheat);

	TAutoConsoleVariable<float> CVarFogExtinction(
		TEXT("massif.Fog.Extinction"), 0.2f,
		TEXT("Scales the field's 0-1 density into fog extinction."), ECVF_Cheat);

	/** Fog scatters forwards, which is what puts a halo round a low sun in it. */
	TAutoConsoleVariable<float> CVarFogPhaseG(
		TEXT("massif.Fog.PhaseG"), 0.45f,
		TEXT("Scattering distribution, 0 even and 0.9 strongly forward."), ECVF_Cheat);

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

bool AMassifFogManager::TryGetViewMeters(FVector2d& OutCentreMeters, double& OutRadiusMeters) const
{
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (Controller == nullptr || Controller->PlayerCameraManager == nullptr)
	{
		return false;
	}

	const AMassifFlyPawn* Camera = Cast<AMassifFlyPawn>(Controller->GetPawn());
	if (Camera != nullptr)
	{
		OutCentreMeters = Camera->GetFocusGroundMeters();
		OutRadiusMeters = FMath::Clamp(
			Camera->GetZoomDistanceMeters(), MinVisibleRadiusMeters, MaxVisibleRadiusMeters);
		return true;
	}

	// On foot the eye is the view, and it does not see far through fog anyway.
	const FVector Location = Controller->PlayerCameraManager->GetCameraLocation();
	OutCentreMeters = FVector2d(
		Location.X * MassifUnits::CentimetersToMeters, Location.Y * MassifUnits::CentimetersToMeters);
	OutRadiusMeters = MinVisibleRadiusMeters;
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

	FVector2d ViewCentreMeters;
	double VisibleRadiusMeters = 0.0;
	if (!TryGetViewMeters(ViewCentreMeters, VisibleRadiusMeters))
	{
		return;
	}

	SolveFog(ViewCentreMeters, VisibleRadiusMeters);
}

void AMassifFogManager::SolveFog(FVector2d ViewCentreMeters, double VisibleRadiusMeters)
{
	Conditions = MassifSolveFogConditions(
		Params, FloorAltitudeMeters, LatitudeDegrees, MassifWorldTime::DayOfYear,
		MassifWorldTime::GetDayFraction(GetWorld()));

	if (!Conditions.IsAnyFog() || CVarFogVolumes.GetValueOnGameThread() == 0)
	{
		HideVolumesFrom(0);
		return;
	}

	const FMassifSunPosition Sun = MassifWorldTime::GetSunPosition(GetWorld(), LatitudeDegrees);

	// The cell grows with the view, so the count stays bounded and the overlap the
	// look is tuned on is the same however far back the camera is.
	const double CellMeters = FMath::Max(
		double(CVarFogCellMeters.GetValueOnGameThread()) * VisibleRadiusMeters / ReferenceRadiusMeters, 1.0);

	// Snapped to the grid rather than centred on the view, so a volume does not
	// slide along under a camera that is panning.
	const int32 CellRadius = FMath::CeilToInt32(VisibleRadiusMeters / CellMeters);
	const FIntPoint CentreCell(
		FMath::RoundToInt32(ViewCentreMeters.X / CellMeters),
		FMath::RoundToInt32(ViewCentreMeters.Y / CellMeters));

	int32 VolumeIndex = 0;
	for (int32 OffsetY = -CellRadius; OffsetY <= CellRadius; ++OffsetY)
	{
		VolumeIndex = PlaceRow(CentreCell, OffsetY, CellRadius, CellMeters, Sun, VolumeIndex);
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
	FIntPoint CentreCell, int32 OffsetY, int32 CellRadius, double CellMeters,
	const FMassifSunPosition& Sun, int32 VolumeIndex)
{
	for (int32 OffsetX = -CellRadius; OffsetX <= CellRadius; ++OffsetX)
	{
		const FVector2d CellCentre(
			(CentreCell.X + OffsetX) * CellMeters, (CentreCell.Y + OffsetY) * CellMeters);
		VolumeIndex = PlaceCell(CellCentre, CellMeters, Sun, VolumeIndex);
	}
	return VolumeIndex;
}

int32 AMassifFogManager::PlaceCell(
	FVector2d CellCentreMeters, double CellMeters, const FMassifSunPosition& Sun, int32 VolumeIndex)
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
		CellMeters * CVarFogReach.GetValueOnGameThread() * MassifUnits::MetersToCentimeters;
	const double VerticalRadiusCentimeters = 0.5 * ColumnMeters * MassifUnits::MetersToCentimeters;
	const double BaseSize = ULocalFogVolumeComponent::GetBaseVolumeSize();
	Volume->SetWorldScale3D(FVector(
		HorizontalRadiusCentimeters / BaseSize, HorizontalRadiusCentimeters / BaseSize,
		VerticalRadiusCentimeters / BaseSize));

	const float Extinction = CVarFogExtinction.GetValueOnGameThread() * static_cast<float>(Density);
	Volume->SetHeightFogExtinction(Extinction);
	Volume->SetRadialFogExtinction(Extinction * CVarFogRadialShare.GetValueOnGameThread());
	Volume->SetHeightFogFalloff(CVarFogFalloff.GetValueOnGameThread());
	Volume->SetFogPhaseG(CVarFogPhaseG.GetValueOnGameThread());
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
