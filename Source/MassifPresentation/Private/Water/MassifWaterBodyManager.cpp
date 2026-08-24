#include "MassifWaterBodyManager.h"

#include "MassifRiverBody.h"
#include "MassifRiverNetwork.h"
#include "MassifUnits.h"
#include "HAL/IConsoleManager.h"
#include "WaterBodyComponent.h"
#include "WaterRuntimeSettings.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"
#include "WaterZoneActor.h"

namespace
{
	using MassifUnits::MetersToCentimeters;

	TAutoConsoleVariable<int32> GWaterDayOfYear(
		TEXT("massif.Water.DayOfYear"),
		196,
		TEXT("Day of the year the rivers are drawn at, 1 to 365. Sets how much water they carry."),
		ECVF_Default);

	/**
	 * Smallest channel drawn as a water body.
	 *
	 * Strahler order is what separates a river from a trickle: order one is a
	 * channel head, and each step up is two equal channels meeting. The brooks
	 * below the threshold are not drawn at all yet.
	 */
	TAutoConsoleVariable<int32> GWaterMinStrahlerOrder(
		TEXT("massif.Water.MinStrahlerOrder"),
		4,
		TEXT("Smallest Strahler order drawn as a water body. Takes effect on level restart."),
		ECVF_Default);

	/**
	 * Side of the water zone, in metres.
	 *
	 * The zone is one render target over whatever it covers, so its size and its
	 * resolution together are what a waterline resolves at. The whole world at
	 * twenty kilometres across would be metres to the texel, which is coarser than
	 * the rivers in it are wide.
	 */
	TAutoConsoleVariable<float> GWaterZoneExtentMeters(
		TEXT("massif.Water.ZoneExtentMeters"),
		2000.0f,
		TEXT("Side of the water zone in metres. Takes effect on level restart."),
		ECVF_Default);

	/**
	 * Texels across the zone's water info texture.
	 *
	 * Square, half precision and four channels, so this is eight bytes a texel of
	 * video memory: two thousand texels is thirty-three megabytes and four
	 * thousand is a hundred and thirty-four. Together with the zone's size it is
	 * what a waterline resolves at, and doubling one is the same as halving the
	 * other for a quarter of the memory.
	 */
	TAutoConsoleVariable<int32> GWaterInfoResolution(
		TEXT("massif.Water.InfoResolution"),
		2048,
		TEXT("Texels across the water info texture. Takes effect on level restart."),
		ECVF_Default);

	/**
	 * Spacing the rivers are turned into spline points at.
	 *
	 * The network's stations are six metres apart, which is far finer than a
	 * spline needs to hold a river's shape. The polyline they were resampled from
	 * was already smoothed through its own corners, so dropping most of them costs
	 * no shape.
	 */
	constexpr double SplinePointSpacingMeters = 24.0;

	/** Bytes a texel of the water info texture costs: four half-precision channels. */
	constexpr int32 WaterInfoBytesPerTexel = 8;

	int32 WaterInfoResolution()
	{
		return FMath::Clamp(GWaterInfoResolution.GetValueOnGameThread(), 256, 4096);
	}

	const TCHAR* RiverSurfaceMaterialPath =
		TEXT("/Water/Materials/WaterSurface/Water_Material_River.Water_Material_River");
	const TCHAR* RiverStaticMeshMaterialPath =
		TEXT("/Water/Materials/WaterSurface/LODs/Water_Material_River_LOD.Water_Material_River_LOD");
	const TCHAR* UnderwaterPostProcessMaterialPath =
		TEXT("/Water/Materials/PostProcessing/M_UnderWater_PostProcess_Volume.M_UnderWater_PostProcess_Volume");

	// --- One river as a spline -----------------------------------------------

	/** Whichever of the two the year knows about: a slice is left at zero where nothing was solved for it. */
	double SeasonalOr(double SeasonalValue, double AnnualValue)
	{
		if (SeasonalValue > 0.0)
		{
			return SeasonalValue;
		}
		return AnnualValue;
	}

	/** One spline point: where the water's top is, and the channel it runs in there. */
	struct FRiverStation
	{
		FVector LocationCentimeters = FVector::ZeroVector;
		float WidthCentimeters = 0.0f;
		float DepthCentimeters = 0.0f;
		float SpeedCentimetersPerSecond = 0.0f;
	};

	FRiverStation StationAt(const FMassifRiverNode& Node, int32 TimeSlice)
	{
		const FMassifStationSeason& Season = Node.Season;
		const double SurfaceMeters =
			SeasonalOr(Season.WaterSurfaceMeters[TimeSlice], Node.WaterSurfaceMeters);

		FRiverStation Station;
		Station.LocationCentimeters =
			FVector(Node.PositionMeters.X, Node.PositionMeters.Y, SurfaceMeters) * MetersToCentimeters;
		Station.WidthCentimeters = static_cast<float>(
			2.0 * SeasonalOr(Season.HalfWidthMeters[TimeSlice], 0.5 * Node.WidthMeters)
				* MetersToCentimeters);
		Station.DepthCentimeters = static_cast<float>(
			SeasonalOr(Season.DepthMeters[TimeSlice], Node.DepthMeters) * MetersToCentimeters);
		Station.SpeedCentimetersPerSecond = static_cast<float>(
			SeasonalOr(Season.SpeedMetersPerSecond[TimeSlice], Node.SpeedMetersPerSecond)
				* MetersToCentimeters);
		return Station;
	}

	double StepMeters(const TArray<FMassifRiverNode>& Nodes, int32 NodeIndex)
	{
		if (NodeIndex == 0)
		{
			return 0.0;
		}
		return (Nodes[NodeIndex].PositionMeters - Nodes[NodeIndex - 1].PositionMeters).Length();
	}

	/** How far down the river each of its stations stands. */
	TArray<double> StationDistancesMeters(const TArray<FMassifRiverNode>& Nodes)
	{
		TArray<double> DistancesMeters;
		DistancesMeters.Reserve(Nodes.Num());

		double TravelledMeters = 0.0;
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			TravelledMeters += StepMeters(Nodes, NodeIndex);
			DistancesMeters.Add(TravelledMeters);
		}
		return DistancesMeters;
	}

	/** The river at the spline's own spacing. The last station is always taken, or it stops short of its mouth. */
	TArray<FRiverStation> GatherStations(const FMassifRiverReach& River, int32 TimeSlice)
	{
		const TArray<FMassifRiverNode>& Nodes = River.Nodes;
		if (Nodes.Num() < 2)
		{
			return TArray<FRiverStation>();
		}

		const TArray<double> DistancesMeters = StationDistancesMeters(Nodes);
		const int32 LastIndex = Nodes.Num() - 1;

		TArray<FRiverStation> Stations;
		double LastKeptMeters = -SplinePointSpacingMeters;
		for (int32 NodeIndex = 0; NodeIndex <= LastIndex; ++NodeIndex)
		{
			const bool bFarEnough =
				DistancesMeters[NodeIndex] - LastKeptMeters >= SplinePointSpacingMeters;
			if (!bFarEnough && NodeIndex != LastIndex)
			{
				continue;
			}

			LastKeptMeters = DistancesMeters[NodeIndex];
			Stations.Add(StationAt(Nodes[NodeIndex], TimeSlice));
		}
		return Stations;
	}

	double RiverLengthMeters(const FMassifRiverReach& River)
	{
		if (River.Nodes.Num() < 2)
		{
			return 0.0;
		}
		return StationDistancesMeters(River.Nodes).Last();
	}

	// --- The bodies themselves ------------------------------------------------

	void WriteSpline(const TArray<FRiverStation>& Stations, UWaterSplineComponent& Spline)
	{
		Spline.ClearSplinePoints(false);
		for (int32 StationIndex = 0; StationIndex < Stations.Num(); ++StationIndex)
		{
			Spline.AddSplinePoint(
				Stations[StationIndex].LocationCentimeters, ESplineCoordinateSpace::World, false);
			Spline.SetSplinePointType(StationIndex, ESplinePointType::Curve, false);
		}
		Spline.UpdateSpline();
	}

	/**
	 * The channel at each point, which is what the river's surface is generated
	 * from: width sets how far out the banks go, velocity scrolls the flow, and
	 * depth is how far the water info says the bed is below the top.
	 */
	void WriteSplineMetadata(const TArray<FRiverStation>& Stations, UWaterSplineMetadata& Metadata)
	{
		Metadata.Reset(Stations.Num());
		for (int32 StationIndex = 0; StationIndex < Stations.Num(); ++StationIndex)
		{
			const float InputKey = static_cast<float>(StationIndex);
			Metadata.RiverWidth.Points.Emplace(InputKey, Stations[StationIndex].WidthCentimeters);
			Metadata.Depth.Points.Emplace(InputKey, Stations[StationIndex].DepthCentimeters);
			Metadata.WaterVelocityScalar.Points.Emplace(
				InputKey, Stations[StationIndex].SpeedCentimetersPerSecond);
			Metadata.AudioIntensity.Points.Emplace(InputKey, 1.0f);
		}
	}

	UMaterialInterface* LoadWaterMaterial(const TCHAR* AssetPath)
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, AssetPath);
		if (Material == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("Water material missing, rivers will not draw: %s"), AssetPath);
		}
		return Material;
	}

	/** What the editor's own water body factory assigns, which a body spawned from code has to do itself. */
	void ApplyRiverMaterials(UWaterBodyComponent& BodyComponent)
	{
		BodyComponent.SetWaterMaterial(LoadWaterMaterial(RiverSurfaceMaterialPath));
		BodyComponent.SetWaterStaticMeshMaterial(LoadWaterMaterial(RiverStaticMeshMaterialPath));
		BodyComponent.SetUnderwaterPostProcessMaterial(
			LoadWaterMaterial(UnderwaterPostProcessMaterialPath));
		BodyComponent.SetWaterInfoMaterial(
			GetDefault<UWaterRuntimeSettings>()->GetDefaultWaterInfoMaterial());
	}

	// --- Choosing what to draw ------------------------------------------------

	/** Whether this river ever grows large enough to be worth an actor and a surface. */
	bool IsDrawnAsWaterBody(const FMassifRiverReach& River)
	{
		return River.StrahlerOrder >= GWaterMinStrahlerOrder.GetValueOnGameThread()
			&& River.Nodes.Num() >= 2;
	}

	/** What the network holds, so the thresholds above can be set against real numbers. */
	void LogNetworkByOrder(const FMassifRiverNetwork& Rivers)
	{
		TMap<int32, int32> ReachesByOrder;
		for (const FMassifRiverReach& Reach : Rivers.GetReaches())
		{
			ReachesByOrder.FindOrAdd(Reach.StrahlerOrder) += 1;
		}

		TArray<int32> Orders;
		ReachesByOrder.GetKeys(Orders);
		Orders.Sort();
		for (const int32 Order : Orders)
		{
			UE_LOG(LogTemp, Display, TEXT("Rivers of order %d: %d reaches"), Order, ReachesByOrder[Order]);
		}
	}
}

int32 MassifDrawnWaterTimeSlice()
{
	const int32 DayOfYear = FMath::Clamp(GWaterDayOfYear.GetValueOnGameThread(), 1, 365);
	return FMath::Clamp(
		(DayOfYear - 1) * MassifSeasonalSliceCount / 365, 0, MassifSeasonalSliceCount - 1);
}

AMassifWaterBodyManager::AMassifWaterBodyManager()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AMassifWaterBodyManager::Initialize(
	const FMassifRiverNetwork& Rivers, FVector2d ZoneCentreMeters, int32 TimeSlice)
{
	LogNetworkByOrder(Rivers);
	SpawnWaterZone(ZoneCentreMeters);

	// Every river gets a body wherever it is. The zone bounds where the water info
	// texture resolves, not which water exists: a body outside it costs one actor
	// and one spline, and is there the moment the zone reaches it.
	for (const FMassifRiverReach& River : Rivers.GetReaches())
	{
		if (!IsDrawnAsWaterBody(River))
		{
			continue;
		}

		AMassifRiverBody* Body = SpawnRiverBody(River, TimeSlice);
		if (Body != nullptr)
		{
			RiverBodies.Add(Body);
		}
	}

	LogWaterBodySummary(Rivers.GetReaches().Num());
}

void AMassifWaterBodyManager::SpawnWaterZone(FVector2d CentreMeters)
{
	const double ExtentMeters =
		FMath::Max(static_cast<double>(GWaterZoneExtentMeters.GetValueOnGameThread()), 100.0);
	const FVector2d HalfExtentMeters(0.5 * ExtentMeters, 0.5 * ExtentMeters);
	ZoneMeters = FBox2d(CentreMeters - HalfExtentMeters, CentreMeters + HalfExtentMeters);

	WaterZone = GetWorld()->SpawnActor<AWaterZone>(
		FVector(CentreMeters.X, CentreMeters.Y, 0.0) * MetersToCentimeters, FRotator::ZeroRotator);
	WaterZone->SetZoneExtent(
		FVector2D(ExtentMeters * MetersToCentimeters, ExtentMeters * MetersToCentimeters));
	WaterZone->SetRenderTargetResolution(FIntPoint(WaterInfoResolution(), WaterInfoResolution()));
}

AMassifRiverBody* AMassifWaterBodyManager::SpawnRiverBody(
	const FMassifRiverReach& River, int32 TimeSlice)
{
	const TArray<FRiverStation> Stations = GatherStations(River, TimeSlice);
	if (Stations.Num() < 2)
	{
		return nullptr;
	}

	AMassifRiverBody* Body = GetWorld()->SpawnActor<AMassifRiverBody>(
		Stations[0].LocationCentimeters, FRotator::ZeroRotator);
	UWaterBodyComponent* BodyComponent = Body->GetWaterBodyComponent();
	UWaterSplineComponent* Spline = BodyComponent->GetWaterSpline();
	UWaterSplineMetadata* Metadata = BodyComponent->GetWaterSplineMetadata();
	if (Spline == nullptr || Metadata == nullptr)
	{
		return nullptr;
	}

	// A placed body is registered static, which is what stops its surface being
	// rebuilt once the world has begun play. This one has no surface until then.
	BodyComponent->SetMobility(EComponentMobility::Movable);
	ApplyRiverMaterials(*BodyComponent);

	WriteSpline(Stations, *Spline);
	WriteSplineMetadata(Stations, *Metadata);
	SplinePointCount += Stations.Num();
	DrawnLengthMeters += RiverLengthMeters(River);

	FOnWaterBodyChangedParams ChangedParams;
	ChangedParams.bShapeOrPositionChanged = true;
	ChangedParams.bUserTriggered = true;
	BodyComponent->UpdateAll(ChangedParams);
#if WITH_EDITOR
	// Generates the meshes the zone rasterises into its water info texture. The
	// plugin only ever does this for a body being authored, because a placed one
	// has them saved with the map and a generated one does not exist yet.
	BodyComponent->UpdateWaterBodyRenderData();
#endif
	return Body;
}

void AMassifWaterBodyManager::LogWaterBodySummary(int32 TracedCount) const
{
	const double ZoneSizeMeters = ZoneMeters.GetSize().X;
	const int32 Resolution = WaterInfoResolution();
	const double VideoMemoryMegabytes =
		static_cast<double>(Resolution) * Resolution * WaterInfoBytesPerTexel / (1024.0 * 1024.0);
	UE_LOG(LogTemp, Display,
		TEXT("Water bodies: %d rivers drawn of %d traced at order %d and up, %.1f km of water over "
			"%d spline points, in a zone %.0f m across at %d texels "
			"(%.2f m per texel, %.0f MB of video memory)"),
		RiverBodies.Num(), TracedCount, GWaterMinStrahlerOrder.GetValueOnGameThread(),
		DrawnLengthMeters / 1000.0, SplinePointCount, ZoneSizeMeters, Resolution,
		ZoneSizeMeters / Resolution, VideoMemoryMegabytes);

	LogZoneOwnership();
}

/**
 * Whether the zone actually took the bodies on, which is what decides if any of
 * them is drawn: the water mesh is tessellated from the zone's own list, so a
 * body that failed to register is built, placed, and invisible.
 */
void AMassifWaterBodyManager::LogZoneOwnership() const
{
	if (WaterZone == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Water bodies: no water zone was spawned."));
		return;
	}

	int32 OwnedCount = 0;
	WaterZone->ForEachWaterBodyComponent([&OwnedCount](UWaterBodyComponent*)
	{
		++OwnedCount;
		return true;
	});

	int32 MeshedCount = 0;
	int32 ZonedCount = 0;
	for (const TObjectPtr<AMassifRiverBody>& Body : RiverBodies)
	{
		const UWaterBodyComponent* BodyComponent = Body->GetWaterBodyComponent();
		if (BodyComponent->GetWaterZone() != nullptr)
		{
			++ZonedCount;
		}
		if (BodyComponent->ShouldGenerateWaterMeshTile())
		{
			++MeshedCount;
		}
	}

	UE_LOG(LogTemp, Display,
		TEXT("Water zone: owns %d bodies, %d of %d bodies found a zone, %d would generate mesh tiles, "
			"zone bounds %s"),
		OwnedCount, ZonedCount, RiverBodies.Num(), MeshedCount, *WaterZone->GetZoneBounds().ToString());
}
