#include "MassifEnvironmentBakeGrid.h"
#include "MassifElevationSource.h"
#include "MassifHeightmap.h"
#include "MassifRiverNetwork.h"
#include "MassifTestEnvironment.h"
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

	TSharedRef<const FMassifHeightmap> MakeRiverNetworkHeightmap(TFunctionRef<double(FVector2d)> AltitudeAt)
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
		return MakeRiverNetworkHeightmap([](FVector2d Position)
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
		return MakeRiverNetworkHeightmap([](FVector2d Position)
		{
			// The two valley floors converge as they run south, so above the meeting
			// point there are two channels and below it one.
			//
			// They converge on a sample column rather than on X = 0, which falls
			// between two of them. A valley whose floor lands between columns has
			// two of them at the same altitude, neither draining into the other, and
			// steepest descent then runs a channel down each instead of one down the
			// middle. Real ground is never level across to the last bit; a world
			// written as a formula is.
			const double AxisMeters = 0.5 * RiverTestSpacingMeters;
			const double Separation = FMath::Max(Position.Y, 0.0) * 0.6;
			const double AcrossLeft = FMath::Abs(Position.X - AxisMeters + Separation);
			const double AcrossRight = FMath::Abs(Position.X - AxisMeters - Separation);
			return 1500.0 + FMath::Min(AcrossLeft, AcrossRight) * 0.4 + Position.Y * 0.15;
		});
	}

	FMassifRiverParams MakeTestRiverParams()
	{
		FMassifRiverParams Params;
		// The test world is one square kilometre, against four hundred for the real
		// one. At the shipping threshold it would hold no channel at all.
		Params.ChannelInitiationAreaSquareMeters = 1.0e4;
		return Params;
	}

	FMassifEnvironmentBakeParams MakeTestBakeParams()
	{
		FMassifEnvironmentBakeParams Params;
		Params.CellSizeMeters = RiverTestSpacingMeters;
		// A thousand times the real depth, for the same reason the threshold is a
		// hundredth of the real one: at a metre a year the channel this world holds
		// would be a trickle pinned to the minimum width, which is not a case the
		// hydraulic geometry can be read off.
		Params.FallbackRunoffMillimetersPerYear = 1.01e6;
		return Params;
	}

	TSharedRef<const FMassifRiverNetwork> ExtractFrom(
		TSharedRef<const FMassifHeightmap> Heightmap, const FMassifRiverParams& RiverParams)
	{
		const FMassifEnvironmentBakeParams Params = MakeTestBakeParams();
		const FMassifBakeGrid Grid = FMassifBakeGrid::FromHeightmap(*Heightmap, Params.CellSizeMeters);

		TArray<double> Measured;
		TArray<double> Filled;
		TArray<float> Accumulation;
		TArray<float> Discharge;
		MassifBakeCpu::SampleAltitudes(*Heightmap, Grid, Measured);
		MassifBakeCpu::FillDepressions(Grid, Params, Measured, Filled);
		MassifBakeCpu::RouteFlow(Grid, Params, Filled, Accumulation);
		MassifRouteEvenRunoff(Grid, Params, Filled, Discharge);

		const FMassifHeightmapElevationSource Elevation(MoveTemp(Heightmap));
		const FMassifUniformBedMaterial BedMaterial;
		// A flat year: this is about where the channels go, not about the season.
		const auto EvenYear = [](FVector2d, int32) { return 1.0; };
		return MassifBakeCpu::ExtractRivers(
			Grid, Elevation, BedMaterial, RiverParams, Measured, Filled, Accumulation, Discharge,
			EvenYear);
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

	const uint64 FirstHash = ExtractFrom(Heightmap, Params)->ComputeContentHash();
	const uint64 SecondHash = ExtractFrom(Heightmap, Params)->ComputeContentHash();
	TestEqual(TEXT("The same terrain traces the same rivers"), FirstHash, SecondHash);

	FMassifRiverParams WiderChannels = Params;
	WiderChannels.ChannelInitiationAreaSquareMeters = Params.ChannelInitiationAreaSquareMeters * 4.0;
	TestNotEqual(TEXT("A different threshold traces different rivers"),
		ExtractFrom(Heightmap, WiderChannels)->ComputeContentHash(), FirstHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkConnectivityTest,
	"Massif.WorldGen.Rivers.Connectivity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkConnectivityTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifRiverNetwork> Rivers = ExtractFrom(MakeConfluence(), MakeTestRiverParams());
	if (!TestTrue(TEXT("The confluence world carries rivers"), Rivers->GetReaches().Num() >= 2))
	{
		return false;
	}

	// Every river has to hand its water on: either to another river, at a station
	// they share exactly, or over the world border. A river that ends anywhere
	// else is water disappearing into a hillside.
	//
	// Any station of another river, not its head. A river runs through its own
	// confluences now, so a tributary joins one partway down rather than at a
	// point where a new run begins.
	TMap<FVector2d, int32> RiversAtStation;
	for (const FMassifRiverReach& River : Rivers->GetReaches())
	{
		for (const FMassifRiverNode& Node : River.Nodes)
		{
			RiversAtStation.FindOrAdd(Node.PositionMeters) += 1;
		}
	}

	int32 RiversEndingNowhere = 0;
	for (const FMassifRiverReach& River : Rivers->GetReaches())
	{
		const FVector2d MouthMeters = River.Nodes.Last().PositionMeters;
		// Its own mouth is one of the stations counted, so being shared means two.
		const bool bHandedOn =
			RiversAtStation[MouthMeters] >= 2 || IsOnWorldBorder(MouthMeters);
		if (!bHandedOn)
		{
			++RiversEndingNowhere;
		}
	}

	TestEqual(TEXT("Every river ends in another river or at the world border"), RiversEndingNowhere, 0);
	TestTrue(TEXT("Rivers start at channel heads"), Rivers->GetSourceCount() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkWaterSurfaceTest,
	"Massif.WorldGen.Rivers.WaterSurface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkWaterSurfaceTest::RunTest(const FString& Parameters)
{
	const FMassifRiverParams Params = MakeTestRiverParams();
	const TSharedRef<const FMassifRiverNetwork> Rivers = ExtractFrom(MakeConfluence(), Params);

	int32 NodesFlowingUphill = 0;
	int32 NodesWithoutAColumn = 0;
	int32 NodesWithABedAboveTheGround = 0;
	for (const FMassifRiverReach& Reach : Rivers->GetReaches())
	{
		for (int32 NodeIndex = 0; NodeIndex < Reach.Nodes.Num(); ++NodeIndex)
		{
			const FMassifRiverNode& Node = Reach.Nodes[NodeIndex];
			const double ColumnMeters = Node.WaterSurfaceMeters - Node.BedAltitudeMeters;
			NodesWithoutAColumn += ColumnMeters < Params.MinCarveDepthMeters - 1.0e-9 ? 1 : 0;
			NodesWithABedAboveTheGround += Node.BedAltitudeMeters > Node.GroundAltitudeMeters ? 1 : 0;
			if (NodeIndex > 0)
			{
				const double PreviousMeters = Reach.Nodes[NodeIndex - 1].WaterSurfaceMeters;
				NodesFlowingUphill += Node.WaterSurfaceMeters > PreviousMeters ? 1 : 0;
			}
		}
	}

	TestEqual(TEXT("The water surface never rises going downstream"), NodesFlowingUphill, 0);
	// The two properties the terrain carve stands on: there is always water to
	// draw, and cutting the ground down to the invert never raises it.
	TestEqual(TEXT("Every node has at least the minimum column of water"), NodesWithoutAColumn, 0);
	TestEqual(TEXT("The channel invert never sits above the measured ground"), NodesWithABedAboveTheGround, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkSmoothingTest,
	"Massif.WorldGen.Rivers.Smoothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkSmoothingTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifRiverNetwork> Rivers = ExtractFrom(MakeConfluence(), MakeTestRiverParams());

	// Catchment only grows downstream, so a resampled node between two traced
	// ones cannot carry less water than the one above it or more than the one
	// below. An interpolant that overshoots puts water in the channel that no
	// catchment paid for.
	//
	// The channel's own width is not asserted to grow with it. It used to be,
	// because width was a power of discharge and could do nothing else; it now
	// comes from the section the water is actually in, and a river narrows where
	// its valley does.
	int32 NodesLosingWater = 0;
	for (const FMassifRiverReach& Reach : Rivers->GetReaches())
	{
		for (int32 NodeIndex = 1; NodeIndex < Reach.Nodes.Num(); ++NodeIndex)
		{
			const FMassifRiverNode& Above = Reach.Nodes[NodeIndex - 1];
			const FMassifRiverNode& Below = Reach.Nodes[NodeIndex];
			NodesLosingWater +=
				Below.DischargeCubicMetersPerSecond < Above.DischargeCubicMetersPerSecond - 1.0e-9 ? 1 : 0;
		}
	}

	TestEqual(TEXT("Discharge never falls going downstream"), NodesLosingWater, 0);

	// Resampling is what turns the routing's 45 degree staircase into a line, so
	// a smoothed reach has to carry more nodes than the corners it was reduced to.
	int32 LongestReachNodes = 0;
	for (const FMassifRiverReach& Reach : Rivers->GetReaches())
	{
		LongestReachNodes = FMath::Max(LongestReachNodes, Reach.Nodes.Num());
	}
	TestTrue(TEXT("The trunk is resampled along its length"), LongestReachNodes > 8);

	// Every reach, not only the ones that kept a corner. A reach running straight
	// is simplified to its two ends however long it is, and a station spacing that
	// survives that is one that does not depend on the line having bent.
	//
	// Against twice the spacing rather than the spacing itself: the steps are
	// placed at equal fractions of a cubic, so the one that spans a curve measures
	// a little longer than the chord it was divided on.
	const double MaxSpanMeters = 2.0 * MakeTestRiverParams().SmoothingSpacingMeters;
	double LongestSpanMeters = 0.0;
	for (const FMassifRiverReach& Reach : Rivers->GetReaches())
	{
		for (int32 NodeIndex = 1; NodeIndex < Reach.Nodes.Num(); ++NodeIndex)
		{
			const double SpanMeters =
				(Reach.Nodes[NodeIndex].PositionMeters - Reach.Nodes[NodeIndex - 1].PositionMeters).Length();
			LongestSpanMeters = FMath::Max(LongestSpanMeters, SpanMeters);
		}
	}

	TestTrue(
		FString::Printf(TEXT("No two stations stand %.1f m apart"), LongestSpanMeters),
		LongestSpanMeters <= MaxSpanMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkChannelShapeTest,
	"Massif.WorldGen.Rivers.ChannelShape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkChannelShapeTest::RunTest(const FString& Parameters)
{
	const FMassifRiverParams Params = MakeTestRiverParams();
	const TSharedRef<const FMassifRiverNetwork> Rivers = ExtractFrom(MakeSingleValley(), Params);
	if (!TestTrue(TEXT("The valley carries a river"), Rivers->GetReaches().Num() > 0))
	{
		return false;
	}

	// The section the water passes through times the speed it passes at is the
	// discharge, at every station. Continuity used to be checked as width times
	// depth times speed, which held while all three were powers of the discharge;
	// the channel is now a shape rather than a box, so the area is the thing that
	// has to come back to the flow.
	int32 NodesBreakingContinuity = 0;
	int32 NodesWithoutAColumn = 0;
	FVector2d BiggestNodeMeters = FVector2d::ZeroVector;
	double BiggestDischargeCubicMetersPerSecond = 0.0;
	double WidestMeters = 0.0;
	for (const FMassifRiverReach& Reach : Rivers->GetReaches())
	{
		for (const FMassifRiverNode& Node : Reach.Nodes)
		{
			const double Carried = Node.Profile.FlowAreaSquareMeters * Node.SpeedMetersPerSecond;
			const double Relative = FMath::Abs(Carried - Node.DischargeCubicMetersPerSecond)
				/ FMath::Max(Node.DischargeCubicMetersPerSecond, UE_DOUBLE_SMALL_NUMBER);
			NodesBreakingContinuity += Relative > 0.02 ? 1 : 0;
			NodesWithoutAColumn += Node.DepthMeters < Params.Channels.MinDepthMeters - 1.0e-9 ? 1 : 0;

			WidestMeters = FMath::Max(WidestMeters, Node.WidthMeters);
			if (Node.DischargeCubicMetersPerSecond > BiggestDischargeCubicMetersPerSecond)
			{
				BiggestDischargeCubicMetersPerSecond = Node.DischargeCubicMetersPerSecond;
				BiggestNodeMeters = Node.PositionMeters;
			}
		}
	}

	TestEqual(TEXT("The section and the speed multiply back to the discharge"), NodesBreakingContinuity, 0);
	// What the terrain carve stands on: there is always water to draw.
	TestEqual(TEXT("Every station has at least the minimum column"), NodesWithoutAColumn, 0);
	TestTrue(TEXT("The river is wider than the minimum somewhere"), WidestMeters > Params.MinWidthMeters);
	// Discharge is the catchment above a point, so the most water the channel ever
	// carries is where it leaves the world, having gathered all of it.
	TestTrue(TEXT("The most water is at the river's mouth"), IsOnWorldBorder(BiggestNodeMeters));
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
		ExtractFrom(Heightmap, Coarse)->GetReaches().Num(), 0);

	// Halving the catchment a channel needs adds the smaller tributaries above
	// the ones already drawn, so the network can only get longer.
	FMassifRiverParams Fine = MakeTestRiverParams();
	const double CoarseLengthMeters = ExtractFrom(Heightmap, Fine)->GetTotalLengthMeters();
	Fine.ChannelInitiationAreaSquareMeters *= 0.5;
	TestTrue(TEXT("A lower threshold traces more channel"),
		ExtractFrom(Heightmap, Fine)->GetTotalLengthMeters() > CoarseLengthMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkStrahlerTest,
	"Massif.WorldGen.Rivers.Strahler",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverNetworkStrahlerTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifRiverNetwork> Confluence =
		ExtractFrom(MakeConfluence(), MakeTestRiverParams());
	TestTrue(TEXT("Two channels meeting make the order above them"), Confluence->GetMaxStrahlerOrder() >= 2);

	int32 SourcesOutOfOrder = 0;
	int32 StationsOutOfOrder = 0;
	for (const FMassifRiverReach& River : Confluence->GetReaches())
	{
		// A river beginning at a channel head has nothing above that head by
		// definition, so its first station is order one; anything higher means the
		// order came from somewhere the channel graph has no edge for.
		if (River.bStartsAtSource && River.Nodes[0].StrahlerOrder != 1)
		{
			++SourcesOutOfOrder;
		}

		// And order can only ever rise going down a river, because the only thing
		// that changes it is another channel joining.
		for (int32 NodeIndex = 1; NodeIndex < River.Nodes.Num(); ++NodeIndex)
		{
			if (River.Nodes[NodeIndex].StrahlerOrder < River.Nodes[NodeIndex - 1].StrahlerOrder)
			{
				++StationsOutOfOrder;
			}
		}
	}

	TestEqual(TEXT("A river starting at a source starts as a headwater"), SourcesOutOfOrder, 0);
	TestEqual(TEXT("Order never falls going downstream"), StationsOutOfOrder, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverNetworkJoinedTest,
	"Massif.WorldGen.Rivers.Joined",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

/**
 * Reaches are joined into whole rivers before anything is solved over them, so
 * that the surface solve sweeps a river end to end rather than stopping at every
 * confluence. What that has to produce is runs that start at a channel head and
 * hold no repeated station where two of them were joined.
 */
bool FRiverNetworkJoinedTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifRiverNetwork> Confluence =
		ExtractFrom(MakeConfluence(), MakeTestRiverParams());

	int32 RiversNotFromASource = 0;
	int32 RepeatedStations = 0;
	for (const FMassifRiverReach& River : Confluence->GetReaches())
	{
		// Every junction passes one of its branches on, so the only place a river
		// can begin is a channel head.
		if (!River.bStartsAtSource)
		{
			++RiversNotFromASource;
		}

		for (int32 NodeIndex = 1; NodeIndex < River.Nodes.Num(); ++NodeIndex)
		{
			const FVector2d StepMeters =
				River.Nodes[NodeIndex].PositionMeters - River.Nodes[NodeIndex - 1].PositionMeters;
			if (StepMeters.IsNearlyZero(0.01))
			{
				++RepeatedStations;
			}
		}
	}

	TestTrue(TEXT("The confluence traces rivers"), Confluence->GetReaches().Num() > 0);
	TestEqual(TEXT("Every river begins at a channel head"), RiversNotFromASource, 0);
	TestEqual(TEXT("A junction is not stationed twice"), RepeatedStations, 0);
	return true;
}

