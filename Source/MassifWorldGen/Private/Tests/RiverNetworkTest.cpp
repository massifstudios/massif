#include "MassifEnvironmentBakeGrid.h"
#include "MassifHeightmap.h"
#include "MassifRiverNetwork.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr int32 RiverTestSide = 128;
	constexpr double RiverTestSpacingMeters = 8.0;
	constexpr double RiverTestHalfWorldMeters = 0.5 * RiverTestSide * RiverTestSpacingMeters;

	FVector2d RiverTestSamplePosition(int32 Column, int32 Row)
	{
		return FVector2d(
			-RiverTestHalfWorldMeters + (Column + 0.5) * RiverTestSpacingMeters,
			RiverTestHalfWorldMeters - (Row + 0.5) * RiverTestSpacingMeters);
	}

	TSharedRef<const FMassifHeightmap> MakeHeightmap(TFunctionRef<double(FVector2d)> AltitudeAt)
	{
		TArray<double> Altitudes;
		Altitudes.Reserve(RiverTestSide * RiverTestSide);
		for (int32 Row = 0; Row < RiverTestSide; ++Row)
		{
			for (int32 Column = 0; Column < RiverTestSide; ++Column)
			{
				Altitudes.Add(AltitudeAt(RiverTestSamplePosition(Column, Row)));
			}
		}
		return FMassifHeightmap::FromAltitudes(Altitudes, RiverTestSide, RiverTestSide, RiverTestSpacingMeters);
	}

	/**
	 * A single V-shaped valley on the X = 0 axis, falling south. Everything the
	 * world collects runs down one line and leaves over the southern border, so
	 * the network it produces is one river whose shape is known in advance.
	 */
	TSharedRef<const FMassifHeightmap> MakeSingleValley()
	{
		return MakeHeightmap([](FVector2d Position)
		{
			return 1500.0 + FMath::Abs(Position.X) * 0.4 + Position.Y * 0.15;
		});
	}

	/**
	 * Two valleys of equal size meeting halfway down, which is the case Strahler
	 * order is defined by: two channels of the same order make the next one up.
	 */
	TSharedRef<const FMassifHeightmap> MakeConfluence()
	{
		return MakeHeightmap([](FVector2d Position)
		{
			// The two valley floors converge on X = 0 as they run south, so above
			// the meeting point there are two channels and below it one.
			const double Separation = FMath::Max(Position.Y, 0.0) * 0.6;
			const double AcrossLeft = FMath::Abs(Position.X + Separation);
			const double AcrossRight = FMath::Abs(Position.X - Separation);
			return 1500.0 + FMath::Min(AcrossLeft, AcrossRight) * 0.4 + Position.Y * 0.15;
		});
	}

	FMassifRiverParams MakeTestRiverParams()
	{
		FMassifRiverParams Params;
		// The test world is one square kilometre, against four hundred for the real
		// one. At the shipping threshold it would hold no channel at all, and at the
		// shipping runoff the channel it does hold would be a trickle pinned to the
		// minimum width, which is not a case the hydraulic geometry can be read off.
		Params.ChannelInitiationAreaSquareMeters = 1.0e4;
		Params.SpecificRunoffMetersPerSecond = 3.2e-5;
		return Params;
	}

	TSharedRef<const FMassifRiverNetwork> ExtractFrom(
		const FMassifHeightmap& Heightmap, const FMassifRiverParams& RiverParams)
	{
		FMassifEnvironmentBakeParams Params;
		Params.CellSizeMeters = RiverTestSpacingMeters;
		const FMassifBakeGrid Grid = FMassifBakeGrid::FromHeightmap(Heightmap, Params.CellSizeMeters);

		TArray<double> Measured;
		TArray<double> Filled;
		TArray<float> Accumulation;
		MassifBakeCpu::SampleAltitudes(Heightmap, Grid, Measured);
		MassifBakeCpu::FillDepressions(Grid, Params, Measured, Filled);
		MassifBakeCpu::RouteFlow(Grid, Params, Filled, Accumulation);
		return MassifBakeCpu::ExtractRivers(Grid, RiverParams, Measured, Filled, Accumulation);
	}

	bool IsOnWorldBorder(FVector2d PositionMeters)
	{
		const double InsetMeters = RiverTestHalfWorldMeters - RiverTestSpacingMeters;
		return FMath::Abs(PositionMeters.X) >= InsetMeters || FMath::Abs(PositionMeters.Y) >= InsetMeters;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkDeterminismTest,
	"Massif.WorldGen.Rivers.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkDeterminismTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHeightmap> Heightmap = MakeConfluence();
	const FMassifRiverParams Params = MakeTestRiverParams();

	const uint64 FirstHash = ExtractFrom(*Heightmap, Params)->ComputeContentHash();
	const uint64 SecondHash = ExtractFrom(*Heightmap, Params)->ComputeContentHash();
	TestEqual(TEXT("The same terrain traces the same rivers"), FirstHash, SecondHash);

	FMassifRiverParams WiderChannels = Params;
	WiderChannels.ChannelInitiationAreaSquareMeters = Params.ChannelInitiationAreaSquareMeters * 4.0;
	TestNotEqual(TEXT("A different threshold traces different rivers"),
		ExtractFrom(*Heightmap, WiderChannels)->ComputeContentHash(), FirstHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkConnectivityTest,
	"Massif.WorldGen.Rivers.Connectivity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkConnectivityTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifRiverNetwork> Rivers = ExtractFrom(*MakeConfluence(), MakeTestRiverParams());
	if (!TestTrue(TEXT("The confluence world carries rivers"), Rivers->GetReaches().Num() >= 2))
	{
		return false;
	}

	// Every reach has to hand its water on: either to another reach, at a point
	// they share exactly, or over the world border. A reach that ends anywhere
	// else is water disappearing into a hillside.
	TSet<FVector2d> ReachHeads;
	for (const FMassifRiverReach& Reach : Rivers->GetReaches())
	{
		ReachHeads.Add(Reach.Nodes[0].PositionMeters);
	}

	int32 ReachesEndingNowhere = 0;
	for (const FMassifRiverReach& Reach : Rivers->GetReaches())
	{
		const FVector2d EndMeters = Reach.Nodes.Last().PositionMeters;
		const bool bHandedOn = ReachHeads.Contains(EndMeters) || IsOnWorldBorder(EndMeters);
		ReachesEndingNowhere += bHandedOn ? 0 : 1;
	}

	TestEqual(TEXT("Every reach ends at another reach or at the world border"), ReachesEndingNowhere, 0);
	TestTrue(TEXT("Rivers start at channel heads"), Rivers->GetSourceCount() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkWaterSurfaceTest,
	"Massif.WorldGen.Rivers.WaterSurface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkWaterSurfaceTest::RunTest(const FString& Parameters)
{
	const FMassifRiverParams Params = MakeTestRiverParams();
	const TSharedRef<const FMassifRiverNetwork> Rivers = ExtractFrom(*MakeConfluence(), Params);

	int32 NodesFlowingUphill = 0;
	int32 NodesUnderTheirBed = 0;
	for (const FMassifRiverReach& Reach : Rivers->GetReaches())
	{
		for (int32 NodeIndex = 0; NodeIndex < Reach.Nodes.Num(); ++NodeIndex)
		{
			const FMassifRiverNode& Node = Reach.Nodes[NodeIndex];
			NodesUnderTheirBed += Node.WaterSurfaceMeters < Node.BedAltitudeMeters ? 1 : 0;
			if (NodeIndex > 0)
			{
				const double PreviousMeters = Reach.Nodes[NodeIndex - 1].WaterSurfaceMeters;
				NodesFlowingUphill += Node.WaterSurfaceMeters > PreviousMeters ? 1 : 0;
			}
		}
	}

	TestEqual(TEXT("The water surface never rises going downstream"), NodesFlowingUphill, 0);
	TestEqual(TEXT("The water surface never sits below the ground it runs over"), NodesUnderTheirBed, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkChannelShapeTest,
	"Massif.WorldGen.Rivers.ChannelShape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkChannelShapeTest::RunTest(const FString& Parameters)
{
	const FMassifRiverParams Params = MakeTestRiverParams();
	const TSharedRef<const FMassifRiverNetwork> Rivers = ExtractFrom(*MakeSingleValley(), Params);
	if (!TestTrue(TEXT("The valley carries a river"), Rivers->GetReaches().Num() > 0))
	{
		return false;
	}

	// Width times depth times speed is the discharge it was all derived from:
	// the exponents sum to one and the coefficients multiply to one, so a change
	// to one of them that breaks continuity fails here rather than silently
	// producing a river carrying more water than its catchment yields.
	int32 NodesBreakingContinuity = 0;
	FVector2d WidestNodeMeters = FVector2d::ZeroVector;
	double WidestMeters = 0.0;
	for (const FMassifRiverReach& Reach : Rivers->GetReaches())
	{
		for (const FMassifRiverNode& Node : Reach.Nodes)
		{
			const bool bClampedToMinimum =
				Node.WidthMeters <= Params.MinWidthMeters || Node.DepthMeters <= Params.MinDepthMeters;
			if (!bClampedToMinimum)
			{
				const double Carried = Node.WidthMeters * Node.DepthMeters * Node.SpeedMetersPerSecond;
				const double Relative = FMath::Abs(Carried - Node.DischargeCubicMetersPerSecond)
					/ FMath::Max(Node.DischargeCubicMetersPerSecond, UE_DOUBLE_SMALL_NUMBER);
				NodesBreakingContinuity += Relative > 0.02 ? 1 : 0;
			}
			if (Node.WidthMeters > WidestMeters)
			{
				WidestMeters = Node.WidthMeters;
				WidestNodeMeters = Node.PositionMeters;
			}
		}
	}

	TestEqual(TEXT("Width, depth and speed multiply back to the discharge"), NodesBreakingContinuity, 0);
	TestTrue(TEXT("The river is wider than the minimum somewhere"), WidestMeters > Params.MinWidthMeters);
	// Discharge is the catchment above a point, so the widest the channel ever
	// gets is where it leaves the world, having gathered all of it.
	TestTrue(TEXT("The widest channel is at the river's mouth"), IsOnWorldBorder(WidestNodeMeters));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkThresholdTest,
	"Massif.WorldGen.Rivers.Threshold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkThresholdTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHeightmap> Heightmap = MakeConfluence();

	FMassifRiverParams Coarse = MakeTestRiverParams();
	Coarse.ChannelInitiationAreaSquareMeters = 1.0e9;
	TestEqual(TEXT("No catchment in the world reaches the threshold, so there are no rivers"),
		ExtractFrom(*Heightmap, Coarse)->GetReaches().Num(), 0);

	// Halving the catchment a channel needs adds the smaller tributaries above
	// the ones already drawn, so the network can only get longer.
	FMassifRiverParams Fine = MakeTestRiverParams();
	const double CoarseLengthMeters = ExtractFrom(*Heightmap, Fine)->GetTotalLengthMeters();
	Fine.ChannelInitiationAreaSquareMeters *= 0.5;
	TestTrue(TEXT("A lower threshold traces more channel"),
		ExtractFrom(*Heightmap, Fine)->GetTotalLengthMeters() > CoarseLengthMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkStrahlerTest,
	"Massif.WorldGen.Rivers.Strahler",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkStrahlerTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifRiverNetwork> Confluence =
		ExtractFrom(*MakeConfluence(), MakeTestRiverParams());
	TestTrue(TEXT("Two channels meeting make the order above them"), Confluence->GetMaxStrahlerOrder() >= 2);

	int32 ReachesOutOfOrder = 0;
	for (const FMassifRiverReach& Reach : Confluence->GetReaches())
	{
		// A reach beginning at a channel head has nothing above it by definition,
		// so it is order one; anything higher means the order came from somewhere
		// the reach graph does not have an edge for.
		ReachesOutOfOrder += Reach.bStartsAtSource && Reach.StrahlerOrder != 1 ? 1 : 0;
	}
	TestEqual(TEXT("A reach starting at a source is a headwater"), ReachesOutOfOrder, 0);
	return true;
}
