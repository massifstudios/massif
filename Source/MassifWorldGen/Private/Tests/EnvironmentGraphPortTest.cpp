#include "MassifElevationSource.h"
#include "MassifEnvironmentBakeGrid.h"
#include "MassifEnvironmentGraph.h"
#include "MassifHeightmap.h"
#include "MassifRiverNetwork.h"
#include "MassifSeed.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 PortTestSeed = 4711;
	constexpr int32 PortTestSide = 96;
	constexpr double PortTestSpacingMeters = 8.0;
	constexpr double PortTestHalfWorldMeters = 0.5 * PortTestSide * PortTestSpacingMeters;

	FVector2d PortSamplePosition(int32 Column, int32 Row)
	{
		return FVector2d(
			-PortTestHalfWorldMeters + (Column + 0.5) * PortTestSpacingMeters,
			PortTestHalfWorldMeters - (Row + 0.5) * PortTestSpacingMeters);
	}

	/** A valley with a hollow in its flank, so every stage has something to do. */
	TSharedRef<const FMassifHeightmap> MakePortTestHeightmap()
	{
		TArray<double> Altitudes;
		Altitudes.Reserve(PortTestSide * PortTestSide);
		for (int32 Row = 0; Row < PortTestSide; ++Row)
		{
			for (int32 Column = 0; Column < PortTestSide; ++Column)
			{
				const FVector2d Position = PortSamplePosition(Column, Row);
				const double Valley = 1500.0 + FMath::Abs(Position.X) * 0.5 + Position.Y * 0.1;
				const double HollowDistance = (Position - FVector2d(160.0, 120.0)).Length();
				const double Hollow = 60.0 * FMath::Max(0.0, 1.0 - HollowDistance / 90.0);
				Altitudes.Add(Valley - Hollow);
			}
		}
		return FMassifHeightmap::FromAltitudes(Altitudes, PortTestSide, PortTestSide, PortTestSpacingMeters);
	}

	FMassifEnvironmentBakeParams MakePortTestParams()
	{
		FMassifEnvironmentBakeParams Params;
		Params.CellSizeMeters = PortTestSpacingMeters;
		Params.DropletsPerCell = 0.25;
		Params.MaxDropletSteps = 32;

		// The port is what is being measured, so the soil must be what the droplets
		// left and nothing else. Roots hold soil where vegetation grows, and that is
		// a later milestone's business, not this one's.
		Params.VegetationSoilRetention = 0.0;
		return Params;
	}

	/** What the stages produce when called directly, in the order the bake called them. */
	struct FReferenceSolve
	{
		TArray<double> MeasuredAltitudeMeters;
		TArray<double> FilledAltitudeMeters;
		TArray<float> StandingWaterDepthMeters;
		TArray<float> FlowAccumulationSquareMeters;
		TArray<float> SoilDepthMeters;
		TArray<float> Wetness;
	};

	FReferenceSolve SolveReference(
		const FMassifHeightmap& Heightmap, const FMassifEnvironmentBakeParams& Params, uint32 DropletSeed)
	{
		const FMassifBakeGrid Grid = FMassifBakeGrid::FromHeightmap(Heightmap, Params.CellSizeMeters);

		FReferenceSolve Solve;
		MassifBakeCpu::SampleAltitudes(Heightmap, Grid, Solve.MeasuredAltitudeMeters);
		MassifBakeCpu::FillDepressions(
			Grid, Params, Solve.MeasuredAltitudeMeters, Solve.FilledAltitudeMeters);
		MassifBakeCpu::MeasureStandingWater(
			Solve.MeasuredAltitudeMeters, Solve.FilledAltitudeMeters, Solve.StandingWaterDepthMeters);
		MassifBakeCpu::RouteFlow(
			Grid, Params, Solve.FilledAltitudeMeters, Solve.FlowAccumulationSquareMeters);
		MassifBakeCpu::RunDroplets(
			Grid, Params, DropletSeed, Solve.FilledAltitudeMeters, Solve.SoilDepthMeters);
		MassifBakeCpu::ComputeWetness(
			Grid, Params, Solve.MeasuredAltitudeMeters, Solve.FlowAccumulationSquareMeters,
			Solve.StandingWaterDepthMeters, Solve.Wetness);
		return Solve;
	}

	/** The seed the soil process derives, so the reference droplets land on the same lattice. */
	uint32 PortTestDropletSeed()
	{
		return FMassifSeed(PortTestSeed).MakeChannelSeed(TEXT("Soil.Depth.Droplets"));
	}

	double LargestDifferenceInRow(
		const FMassifEnvironmentState& State, FMassifChannelHandle Channel, int32 Row,
		TArrayView<const double> Reference)
	{
		const FMassifChannelGrid& Grid = State.GetGrid(Channel);

		double Largest = 0.0;
		for (int32 Column = 0; Column < Grid.Width; ++Column)
		{
			const double Difference =
				State.ReadCell(Channel, Column, Row) - Reference[Row * Grid.Width + Column];
			Largest = FMath::Max(Largest, FMath::Abs(Difference));
		}
		return Largest;
	}

	/** Largest gap between a channel and the reference it was ported from. */
	double LargestDifference(
		const FMassifEnvironmentState& State, const FName& ChannelName, TArrayView<const double> Reference)
	{
		const FMassifChannelHandle Channel = State.FindChannel(ChannelName);
		const FMassifChannelGrid& Grid = State.GetGrid(Channel);

		double Largest = 0.0;
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			Largest = FMath::Max(Largest, LargestDifferenceInRow(State, Channel, Row, Reference));
		}
		return Largest;
	}

	TArray<double> AsDoubles(TArrayView<const float> Values)
	{
		TArray<double> Doubles;
		Doubles.Reserve(Values.Num());
		for (float Value : Values)
		{
			Doubles.Add(Value);
		}
		return Doubles;
	}

	/** One step of a channel's quantisation, which is all a uint16 channel can be asked to hold. */
	double QuantisationStep(const FMassifEnvironmentState& State, const FName& ChannelName)
	{
		const FMassifChannelHandle Channel = State.FindChannel(ChannelName);
		const double Span = State.GetChannelMaximum(Channel) - State.GetChannelMinimum(Channel);
		return FMath::Max(Span / 65535.0, UE_DOUBLE_KINDA_SMALL_NUMBER);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentGraphPortTest,
	"Massif.WorldGen.EnvironmentGraph.Port",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentGraphPortTest::RunTest(const FString& Parameters)
{
	// The graph runs the same six solves the bake ran. A port that changes a value
	// is a failed port, so every channel is held against the stage that produced it
	// - exactly where the channel keeps its input exactly, and to one quantisation
	// step where it does not.
	const TSharedRef<const FMassifHeightmap> Heightmap = MakePortTestHeightmap();
	const FMassifEnvironmentBakeParams Params = MakePortTestParams();

	FString Error;
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifBuildEnvironment(
		MakeShared<FMassifHeightmapElevationSource>(Heightmap), FMassifGeoreference(), PortTestSeed, Params,
		Error);
	if (!TestNotNull(TEXT("The environment graph schedules and runs"), Scheduler.Get()))
	{
		AddError(Error);
		return false;
	}

	const FMassifEnvironmentState& State = Scheduler->GetState();
	const FReferenceSolve Reference = SolveReference(*Heightmap, Params, PortTestDropletSeed());

	const FMassifChannelGrid& Grid = State.GetGrid(State.FindChannel(MassifEnvironmentChannels::Altitude()));
	TestEqual(TEXT("The graph grid is the bake grid"), Grid.CellCount(), Reference.MeasuredAltitudeMeters.Num());

	TestEqual(
		TEXT("Altitude is carried exactly"),
		LargestDifference(State, MassifEnvironmentChannels::Altitude(), Reference.MeasuredAltitudeMeters), 0.0);
	TestEqual(
		TEXT("The filled surface is carried exactly"),
		LargestDifference(State, MassifEnvironmentChannels::FilledAltitude(), Reference.FilledAltitudeMeters),
		0.0);
	TestEqual(
		TEXT("Flow accumulation is carried exactly"),
		LargestDifference(
			State, MassifEnvironmentChannels::FlowAccumulation(),
			AsDoubles(Reference.FlowAccumulationSquareMeters)),
		0.0);

	TestTrue(
		TEXT("Standing water is within its quantisation"),
		LargestDifference(
			State, MassifEnvironmentChannels::StandingWaterDepth(),
			AsDoubles(Reference.StandingWaterDepthMeters))
			<= QuantisationStep(State, MassifEnvironmentChannels::StandingWaterDepth()));
	TestTrue(
		TEXT("Soil depth is within its quantisation"),
		LargestDifference(State, MassifEnvironmentChannels::SoilDepth(), AsDoubles(Reference.SoilDepthMeters))
			<= QuantisationStep(State, MassifEnvironmentChannels::SoilDepth()));
	TestTrue(
		TEXT("Wetness is within its quantisation"),
		LargestDifference(State, MassifEnvironmentChannels::Wetness(), AsDoubles(Reference.Wetness))
			<= QuantisationStep(State, MassifEnvironmentChannels::Wetness()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentGraphRiverTest,
	"Massif.WorldGen.EnvironmentGraph.Rivers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentGraphRiverTest::RunTest(const FString& Parameters)
{
	// Rivers are traced from the channels rather than from working buffers, which
	// only works because the altitude channels kept their doubles.
	const TSharedRef<const FMassifHeightmap> Heightmap = MakePortTestHeightmap();
	const FMassifEnvironmentBakeParams Params = MakePortTestParams();

	FString Error;
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifBuildEnvironment(
		MakeShared<FMassifHeightmapElevationSource>(Heightmap), FMassifGeoreference(), PortTestSeed, Params,
		Error);
	if (!TestNotNull(TEXT("The environment graph schedules and runs"), Scheduler.Get()))
	{
		return false;
	}

	const FReferenceSolve Reference = SolveReference(*Heightmap, Params, PortTestDropletSeed());
	const FMassifBakeGrid Grid = FMassifBakeGrid::FromHeightmap(*Heightmap, Params.CellSizeMeters);

	const TSharedRef<const FMassifRiverNetwork> FromGraph =
		MassifExtractRivers(Scheduler->GetState(), Params.Rivers);
	const TSharedRef<const FMassifRiverNetwork> FromReference = MassifBakeCpu::ExtractRivers(
		Grid, Params.Rivers, Reference.MeasuredAltitudeMeters, Reference.FilledAltitudeMeters,
		Reference.FlowAccumulationSquareMeters);

	TestEqual(
		TEXT("The graph traces the same reaches"), FromGraph->GetReaches().Num(),
		FromReference->GetReaches().Num());

	int32 DifferingNodes = 0;
	for (int32 ReachIndex = 0; ReachIndex < FromGraph->GetReaches().Num(); ++ReachIndex)
	{
		DifferingNodes += static_cast<int32>(
			FromGraph->GetReaches()[ReachIndex].Nodes.Num()
			!= FromReference->GetReaches()[ReachIndex].Nodes.Num());
	}
	TestEqual(TEXT("Every reach has the same nodes"), DifferingNodes, 0);
	return true;
}
