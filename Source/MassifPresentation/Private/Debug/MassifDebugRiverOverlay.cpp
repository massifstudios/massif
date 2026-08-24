#include "MassifDebugRiverOverlay.h"

#include "DrawDebugHelpers.h"
#include "HAL/IConsoleManager.h"
#include "MassifDebugCamera.h"
#include "MassifRiverNetwork.h"
#include "MassifUnits.h"

namespace
{
	using MassifUnits::MetersToCentimeters;

	TAutoConsoleVariable<int32> CVarRiverOverlay(
		TEXT("massif.Rivers.Overlay"), 0,
		TEXT("Draw the extracted river network: centreline, stations, banks and the water column."),
		ECVF_Cheat);

	TAutoConsoleVariable<float> CVarRiverOverlayRadius(
		TEXT("massif.Rivers.OverlayRadiusMeters"), 1500.0f,
		TEXT("How far from the camera the river overlay is drawn."),
		ECVF_Cheat);

	TAutoConsoleVariable<int32> CVarRiverOverlayMinOrder(
		TEXT("massif.Rivers.OverlayMinOrder"), 1,
		TEXT("Smallest Strahler order the river overlay draws. 1 shows every trickle."),
		ECVF_Cheat);

	TAutoConsoleVariable<int32> CVarRiverOverlayOnTop(
		TEXT("massif.Rivers.OverlayOnTop"), 0,
		TEXT("Draw the overlay through the terrain, so a channel buried in the ground is still visible."),
		ECVF_Cheat);

	TAutoConsoleVariable<float> CVarRiverOverlaySectionRadius(
		TEXT("massif.Rivers.OverlaySectionRadiusMeters"), 0.0f,
		TEXT("How far from the camera the solved cross-sections are drawn. 0 draws none."),
		ECVF_Cheat);

	/** Line width, in centimetres, of the centreline and of everything hung off it. */
	constexpr float CentrelineThickness = 8.0f;
	constexpr float DetailThickness = 3.0f;

	/**
	 * How far above the water surface the centreline is lifted.
	 *
	 * Enough to clear the drawn water sheet and no more: the whole point is to
	 * see the line against the ground it was solved over, so it may not be
	 * floated far enough to stop reading as being in the channel.
	 */
	constexpr double CentrelineLiftMeters = 0.15;

	/** A station's altitude marks, drawn as ticks this long across the flow. */
	constexpr double AltitudeTickMeters = 0.6;

	/** Colours by Strahler order, a headwater first. Beyond the last, the last is used. */
	const FColor OrderColors[] = {
		FColor(120, 200, 255),
		FColor(90, 160, 255),
		FColor(60, 110, 255),
		FColor(150, 90, 255),
		FColor(255, 80, 200),
	};

	const FColor StationColor(255, 255, 255);
	const FColor BankColor(255, 190, 60);
	const FColor BedColor(255, 90, 60);
	const FColor GroundColor(140, 255, 140);
	const FColor ReachEndColor(255, 255, 0);
	const FColor SectionGroundColor(120, 255, 120);
	const FColor SectionBedColor(255, 120, 90);

	FColor ColorOfOrder(int32 StrahlerOrder)
	{
		const int32 LastIndex = UE_ARRAY_COUNT(OrderColors) - 1;
		return OrderColors[FMath::Clamp(StrahlerOrder - 1, 0, LastIndex)];
	}

	uint8 DepthPriority()
	{
		if (CVarRiverOverlayOnTop.GetValueOnGameThread() != 0)
		{
			return SDPG_Foreground;
		}
		return SDPG_World;
	}

	FVector WorldCentimeters(FVector2d PositionMeters, double AltitudeMeters)
	{
		return FVector(PositionMeters.X, PositionMeters.Y, AltitudeMeters) * MetersToCentimeters;
	}

	void DrawSegment(UWorld* World, const FVector& From, const FVector& To, FColor Color, float Thickness)
	{
		DrawDebugLine(World, From, To, Color, false, -1.0f, DepthPriority(), Thickness);
	}

	/**
	 * Downstream at a station, from its neighbours. One-sided at the ends of a
	 * reach, so the first and last stations are still square to the flow.
	 */
	FVector2d FlowDirectionAt(const FMassifRiverReach& Reach, int32 NodeIndex)
	{
		const int32 Before = FMath::Max(NodeIndex - 1, 0);
		const int32 After = FMath::Min(NodeIndex + 1, Reach.Nodes.Num() - 1);
		const FVector2d Along =
			(Reach.Nodes[After].PositionMeters - Reach.Nodes[Before].PositionMeters).GetSafeNormal();
		if (Along.IsNearlyZero())
		{
			return FVector2d(1.0, 0.0);
		}
		return Along;
	}

	FVector2d AcrossFlowAt(const FMassifRiverReach& Reach, int32 NodeIndex)
	{
		const FVector2d Along = FlowDirectionAt(Reach, NodeIndex);
		return FVector2d(-Along.Y, Along.X);
	}

	/**
	 * The three altitudes a station holds, on one upright: the ground the model
	 * measured, the bed cut under it, and the surface the flow solve settled on.
	 * A surface standing over the ground rather than in it is the failure this
	 * makes obvious.
	 */
	void DrawStationColumn(UWorld* World, const FMassifRiverNode& Node, FVector2d AcrossFlow)
	{
		const FVector Surface = WorldCentimeters(Node.PositionMeters, Node.WaterSurfaceMeters);
		const FVector Bed = WorldCentimeters(Node.PositionMeters, Node.BedAltitudeMeters);
		const FVector Ground = WorldCentimeters(Node.PositionMeters, Node.GroundAltitudeMeters);
		const FVector Tick = FVector(AcrossFlow.X, AcrossFlow.Y, 0.0)
			* (0.5 * AltitudeTickMeters * MetersToCentimeters);

		DrawSegment(World, Bed, Surface, BedColor, DetailThickness);
		DrawSegment(World, Ground - Tick, Ground + Tick, GroundColor, DetailThickness);
		DrawSegment(World, Bed - Tick, Bed + Tick, BedColor, DetailThickness);
	}

	/** Where the water reaches at a station, from the section the profile solve fitted. */
	double WettedHalfWidthMeters(const FMassifRiverNode& Node)
	{
		if (Node.Profile.WettedWidthMeters > 0.0)
		{
			return 0.5 * Node.Profile.WettedWidthMeters;
		}
		return 0.5 * Node.WidthMeters;
	}

	void DrawBankSpan(UWorld* World, const FMassifRiverNode& Node, FVector2d AcrossFlow)
	{
		const FVector2d OffsetMeters = AcrossFlow * WettedHalfWidthMeters(Node);
		const FVector Left =
			WorldCentimeters(Node.PositionMeters - OffsetMeters, Node.WaterSurfaceMeters);
		const FVector Right =
			WorldCentimeters(Node.PositionMeters + OffsetMeters, Node.WaterSurfaceMeters);
		DrawSegment(World, Left, Right, BankColor, DetailThickness);
	}

	/**
	 * The ground and the bed across one station, at the offsets the profile solve
	 * actually read them at. A section with no bank in it is a river the solve had
	 * nothing to fit into, and it is only visible here.
	 */
	void DrawCrossSection(UWorld* World, const FMassifRiverNode& Node, FVector2d AcrossFlow)
	{
		const FMassifChannelProfile& Profile = Node.Profile;
		if (!(Profile.HalfSpanMeters > 0.0))
		{
			return;
		}

		const double SpacingMeters =
			2.0 * Profile.HalfSpanMeters / (MassifChannelProfileOffsetCount - 1);
		FVector PreviousGround = FVector::ZeroVector;
		FVector PreviousBed = FVector::ZeroVector;
		for (int32 Offset = 0; Offset < MassifChannelProfileOffsetCount; ++Offset)
		{
			const FVector2d AtMeters = Node.PositionMeters
				+ AcrossFlow * (Offset * SpacingMeters - Profile.HalfSpanMeters);
			const FVector Ground = WorldCentimeters(AtMeters, Profile.GroundAltitudeMeters[Offset]);
			const FVector Bed = WorldCentimeters(AtMeters, Profile.BedAltitudeMeters[Offset]);
			if (Offset > 0)
			{
				DrawSegment(World, PreviousGround, Ground, SectionGroundColor, DetailThickness);
				DrawSegment(World, PreviousBed, Bed, SectionBedColor, DetailThickness);
			}
			PreviousGround = Ground;
			PreviousBed = Bed;
		}
	}

	FVector CentrelinePoint(const FMassifRiverNode& Node)
	{
		return WorldCentimeters(Node.PositionMeters, Node.WaterSurfaceMeters + CentrelineLiftMeters);
	}

	bool IsNear(FVector2d PositionMeters, FVector2d CentreMeters, double RadiusMeters)
	{
		return (PositionMeters - CentreMeters).SquaredLength() <= RadiusMeters * RadiusMeters;
	}

	/** What the overlay is showing this frame, so the toggles can be read off one line. */
	struct FOverlayBudget
	{
		double RadiusMeters = 0.0;
		double SectionRadiusMeters = 0.0;
		int32 MinOrder = 1;
		FVector2d CentreMeters = FVector2d::ZeroVector;
	};

	void DrawStation(
		UWorld* World, const FMassifRiverReach& Reach, int32 NodeIndex, const FOverlayBudget& Budget)
	{
		const FMassifRiverNode& Node = Reach.Nodes[NodeIndex];
		const FVector2d AcrossFlow = AcrossFlowAt(Reach, NodeIndex);

		DrawStationColumn(World, Node, AcrossFlow);
		DrawBankSpan(World, Node, AcrossFlow);
		if (IsNear(Node.PositionMeters, Budget.CentreMeters, Budget.SectionRadiusMeters))
		{
			DrawCrossSection(World, Node, AcrossFlow);
		}
	}

	void DrawReachEnd(UWorld* World, const FMassifRiverNode& Node)
	{
		const FVector At = CentrelinePoint(Node);
		DrawDebugPoint(World, At, 14.0f, ReachEndColor, false, -1.0f, DepthPriority());
	}

	void DrawReach(UWorld* World, const FMassifRiverReach& Reach, const FOverlayBudget& Budget)
	{
		const FColor Color = ColorOfOrder(Reach.StrahlerOrder);
		for (int32 NodeIndex = 0; NodeIndex < Reach.Nodes.Num(); ++NodeIndex)
		{
			const FMassifRiverNode& Node = Reach.Nodes[NodeIndex];
			if (!IsNear(Node.PositionMeters, Budget.CentreMeters, Budget.RadiusMeters))
			{
				continue;
			}

			DrawStation(World, Reach, NodeIndex, Budget);
			DrawDebugPoint(World, CentrelinePoint(Node), 6.0f, StationColor, false, -1.0f, DepthPriority());
			if (NodeIndex > 0)
			{
				DrawSegment(World, CentrelinePoint(Reach.Nodes[NodeIndex - 1]), CentrelinePoint(Node),
					Color, CentrelineThickness);
			}
		}

		DrawReachEnd(World, Reach.Nodes[0]);
		DrawReachEnd(World, Reach.Nodes.Last());
	}

	bool IsReachDrawn(const FMassifRiverReach& Reach, const FOverlayBudget& Budget)
	{
		if (Reach.StrahlerOrder < Budget.MinOrder || Reach.Nodes.Num() < 2)
		{
			return false;
		}

		// Cheap rejection on the ends alone would drop a reach running through the
		// view with both of its confluences outside it.
		for (const FMassifRiverNode& Node : Reach.Nodes)
		{
			if (IsNear(Node.PositionMeters, Budget.CentreMeters, Budget.RadiusMeters))
			{
				return true;
			}
		}
		return false;
	}
}

bool MassifDebugRiverOverlay::IsEnabled()
{
	return CVarRiverOverlay.GetValueOnGameThread() != 0;
}

void MassifDebugRiverOverlay::Draw(UWorld* World, const FMassifRiverNetwork& Rivers)
{
	if (World == nullptr || !IsEnabled())
	{
		return;
	}

	FOverlayBudget Budget;
	Budget.CentreMeters = MassifDebugCamera::GetPositionMeters(World);
	Budget.RadiusMeters = FMath::Max(CVarRiverOverlayRadius.GetValueOnGameThread(), 0.0f);
	Budget.SectionRadiusMeters =
		FMath::Max(CVarRiverOverlaySectionRadius.GetValueOnGameThread(), 0.0f);
	Budget.MinOrder = CVarRiverOverlayMinOrder.GetValueOnGameThread();

	for (const FMassifRiverReach& Reach : Rivers.GetReaches())
	{
		if (IsReachDrawn(Reach, Budget))
		{
			DrawReach(World, Reach, Budget);
		}
	}
}
