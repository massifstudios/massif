#include "MassifEnvironmentBakeGrid.h"
#include "MassifEnvironmentGraph.h"
#include "MassifLithologyProcesses.h"
#include "MassifPrecipitationProcesses.h"
#include "MassifSnowProcesses.h"
#include "MassifTestEnvironment.h"
#include "MassifHeightmap.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 BakeTestSeed = 4711;
	constexpr int32 BakeTestSide = 96;
	constexpr double BakeTestSpacingMeters = 8.0;
	constexpr double BakeTestHalfWorldMeters = 0.5 * BakeTestSide * BakeTestSpacingMeters;

	/** Easting and northing of a sample of the test heightmap, origin at the centre. */
	FVector2d TestSamplePosition(int32 Column, int32 Row)
	{
		return FVector2d(
			-BakeTestHalfWorldMeters + (Column + 0.5) * BakeTestSpacingMeters,
			BakeTestHalfWorldMeters - (Row + 0.5) * BakeTestSpacingMeters);
	}

	TSharedRef<const FMassifHeightmap> MakeBakeHeightmap(TFunctionRef<double(FVector2d)> AltitudeAt)
	{
		TArray<double> Altitudes;
		Altitudes.Reserve(BakeTestSide * BakeTestSide);
		for (int32 Row = 0; Row < BakeTestSide; ++Row)
		{
			for (int32 Column = 0; Column < BakeTestSide; ++Column)
			{
				Altitudes.Add(AltitudeAt(TestSamplePosition(Column, Row)));
			}
		}
		return FMassifHeightmap::FromAltitudes(Altitudes, BakeTestSide, BakeTestSide, BakeTestSpacingMeters);
	}

	/** A V-shaped valley on the X = 0 axis, falling south so everything it collects leaves the world. */
	TSharedRef<const FMassifHeightmap> MakeValley()
	{
		return MakeBakeHeightmap([](FVector2d Position)
		{
			return 1500.0 + FMath::Abs(Position.X) * 0.5 + Position.Y * 0.1;
		});
	}

	/** A plane falling due south, so uphill from anywhere is due north and nowhere else. */
	TSharedRef<const FMassifHeightmap> MakeSouthFacingSlope()
	{
		return MakeBakeHeightmap([](FVector2d Position)
		{
			return 1500.0 + Position.Y * 2.0;
		});
	}

	/** The same slope with a round pit punched into the middle of it: one hollow, and nothing else to fill. */
	TSharedRef<const FMassifHeightmap> MakePittedSlope(double PitDepthMeters, double PitRadiusMeters)
	{
		return MakeBakeHeightmap([PitDepthMeters, PitRadiusMeters](FVector2d Position)
		{
			const double PlaneAltitude = 1500.0 + Position.Y * 0.1;
			const double DistanceMeters = Position.Length();
			if (DistanceMeters >= PitRadiusMeters)
			{
				return PlaneAltitude;
			}
			const double Depth = PitDepthMeters * (1.0 - DistanceMeters / PitRadiusMeters);
			return PlaneAltitude - Depth;
		});
	}

	FMassifEnvironmentBakeParams MakeTestParams()
	{
		FMassifEnvironmentBakeParams Params;
		Params.CellSizeMeters = BakeTestSpacingMeters;
		return Params;
	}

	/** Highest of the eight neighbours' drops, negative where every neighbour is higher. */
	double SteepestDropMeters(const FMassifBakeGrid& Grid, TArrayView<const double> Filled, int32 Column, int32 Row)
	{
		double SteepestMeters = -TNumericLimits<double>::Max();
		const double CellAltitude = Filled[Grid.Index(Column, Row)];
		for (int32 Direction = 0; Direction < MassifBakeNeighbours::Count; ++Direction)
		{
			const int32 NeighbourColumn = Column + MassifBakeNeighbours::OffsetX[Direction];
			const int32 NeighbourRow = Row + MassifBakeNeighbours::OffsetY[Direction];
			if (Grid.Contains(NeighbourColumn, NeighbourRow))
			{
				SteepestMeters = FMath::Max(
					SteepestMeters, CellAltitude - Filled[Grid.Index(NeighbourColumn, NeighbourRow)]);
			}
		}
		return SteepestMeters;
	}

	double SampleChannel(
		const FMassifEnvironmentState& State, const FName& ChannelName, FVector2d PositionMeters,
		int32 TimeSlice = 0)
	{
		return State.SampleScalar(State.FindChannel(ChannelName), PositionMeters, TimeSlice);
	}

	/** One step of a channel's quantisation, which is all a uint16 channel can be asked to hold. */
	double BakeQuantisationStep(const FMassifEnvironmentState& State, const FName& ChannelName)
	{
		const FMassifChannelHandle Channel = State.FindChannel(ChannelName);
		const double Span = State.GetChannelMaximum(Channel) - State.GetChannelMinimum(Channel);
		return FMath::Max(Span / 65535.0, UE_DOUBLE_KINDA_SMALL_NUMBER);
	}

	double SumOverYear(const FMassifEnvironmentState& State, const FName& ChannelName, FVector2d PositionMeters)
	{
		const FMassifChannelHandle Channel = State.FindChannel(ChannelName);

		double Total = 0.0;
		for (int32 TimeSlice = 0; TimeSlice < State.GetDescriptor(Channel).TimeSliceCount; ++TimeSlice)
		{
			Total += State.SampleScalar(Channel, PositionMeters, TimeSlice);
		}
		return Total;
	}

	/** Catchment the stand-in routing hands every cell, in square metres. */
	constexpr double WaterBalanceCatchmentSquareMeters = 5000.0;

	FMassifChannelDescriptor MakeTestChannel(const FName& Name, const TCHAR* Unit, int32 TimeSliceCount = 1)
	{
		FMassifChannelDescriptor Descriptor;
		Descriptor.Name = Name;
		Descriptor.Type = EMassifChannelType::Scalar;
		Descriptor.Encoding = EMassifChannelEncoding::Float32;
		Descriptor.CellSizeMeters = BakeTestSpacingMeters;
		Descriptor.TimeSliceCount = TimeSliceCount;
		Descriptor.Unit = Unit;
		return Descriptor;
	}

	void FillSlice(FMassifEnvironmentContext& Context, const FName& ChannelName, int32 TimeSlice, double Value)
	{
		const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(ChannelName, TimeSlice);
		TArrayView<double> Values = Scope->GetComponent(0);
		for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
		{
			Values[CellIndex] = Value;
		}
		Scope->Commit();
	}

	/**
	 * A catchment reading the same everywhere, standing in for the routed drainage.
	 *
	 * The water balance is about what happens to water that has already been
	 * gathered, so the gathering is the variable that has to be held still.
	 */
	class FUniformCatchmentProcess final : public IMassifEnvironmentProcess
	{
	public:
		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = TEXT("Test.UniformCatchment");
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(
				MakeTestChannel(MassifEnvironmentChannels::FlowAccumulation(), TEXT("m2")));
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity) const override
		{
			FillSlice(
				Context, MassifEnvironmentChannels::FlowAccumulation(), 0, WaterBalanceCatchmentSquareMeters);
		}
	};

	/** Rock that changes at one line of northing: a contact for a spring to break out along. */
	class FSteppedSoilYieldProcess final : public IMassifEnvironmentProcess
	{
	public:
		FSteppedSoilYieldProcess(double InNorthYield, double InSouthYield)
			: NorthYield(InNorthYield)
			, SouthYield(InSouthYield)
		{
		}

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = TEXT("Test.SteppedSoilYield");
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(MakeTestChannel(MassifEnvironmentChannels::SoilYield(), TEXT("0-1")));
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity) const override
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::SoilYield());
			const FMassifChannelGrid& Grid = Scope->GetGrid();

			TArrayView<double> Values = Scope->GetComponent(0);
			for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
			{
				Values[CellIndex] =
					YieldAt(Grid.CellCentreMeters(CellIndex % Grid.Width, CellIndex / Grid.Width));
			}
			Scope->Commit();
		}

	private:
		double YieldAt(FVector2d PositionMeters) const
		{
			if (PositionMeters.Y > 0.0)
			{
				return NorthYield;
			}
			return SouthYield;
		}

		double NorthYield = 0.0;
		double SouthYield = 0.0;
	};

	/** Metres lying at a slice of the test year: a pack that builds to midsummer index and melts out again. */
	double TestSnowDepthMeters(int32 TimeSlice)
	{
		if (TimeSlice <= 12)
		{
			return 0.5 * TimeSlice;
		}
		return 0.5 * (24 - TimeSlice);
	}

	/**
	 * A snowpack with every millimetre of the year falling as snow.
	 *
	 * The whole year's water is then held until the pack releases it, which is the
	 * clearest form of the melt pulse the seasonal flow is meant to carry.
	 */
	class FTestSnowpackProcess final : public IMassifEnvironmentProcess
	{
	public:
		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = TEXT("Test.Snowpack");
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(MakeTestChannel(MassifEnvironmentChannels::SnowDepth(), TEXT("m"), 24));
			Declaration.OutputChannels.Add(
				MakeTestChannel(MassifEnvironmentChannels::SnowFraction(), TEXT("0-1"), 24));
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity) const override
		{
			for (int32 TimeSlice = 0; TimeSlice < 24; ++TimeSlice)
			{
				FillSlice(
					Context, MassifEnvironmentChannels::SnowDepth(), TimeSlice, TestSnowDepthMeters(TimeSlice));
				FillSlice(Context, MassifEnvironmentChannels::SnowFraction(), TimeSlice, 1.0);
			}
		}
	};

	/**
	 * The water balance over a stand-in catchment, and whatever else the caller adds.
	 *
	 * Deliberately not the standard graph: with no soil, no rock and no weather
	 * registered, every optional input the balance reads is absent, which is the
	 * arrangement those inputs exist to support.
	 */
	TUniquePtr<FMassifEnvironmentScheduler> SolveWaterBalance(
		TSharedRef<const FMassifHeightmap> Heightmap, const FMassifEnvironmentBakeParams& Params,
		const TArray<TSharedRef<const IMassifEnvironmentProcess>>& ExtraProcesses,
		EMassifFidelity Fidelity = EMassifFidelity::Standard)
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifHeightmapElevationSource>(MoveTemp(Heightmap)), FMassifGeoreference(),
			BakeTestSeed);
		MassifRegisterAltitudeProcess(*Scheduler, Params);
		Scheduler->RegisterProcess(MakeShared<FUniformCatchmentProcess>());
		for (const TSharedRef<const IMassifEnvironmentProcess>& Process : ExtraProcesses)
		{
			Scheduler->RegisterProcess(Process);
		}
		MassifRegisterWaterBalanceProcesses(*Scheduler, Params);

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Infiltration(), Fidelity);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Springs(), Fidelity);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::SeasonalFlow(), Fidelity);
		Scheduler->Execute();
		return Scheduler;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeDrainageTest,
	"Massif.WorldGen.EnvironmentBake.Drainage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeDrainageTest::RunTest(const FString& Parameters)
{
	// The pit is the only hollow, so if the fill works every cell of the world
	// ends up with somewhere lower to send its water.
	const TSharedRef<const FMassifHeightmap> Heightmap = MakePittedSlope(40.0, 120.0);
	const FMassifEnvironmentBakeParams Params = MakeTestParams();
	const FMassifBakeGrid Grid = FMassifBakeGrid::FromHeightmap(*Heightmap, Params.CellSizeMeters);

	TArray<double> Measured;
	TArray<double> Filled;
	MassifBakeCpu::SampleAltitudes(*Heightmap, Grid, Measured);
	MassifBakeCpu::FillDepressions(Grid, Params, Measured, Filled);

	int32 CellsBelowMeasured = 0;
	int32 CellsWithNowhereToDrain = 0;
	for (int32 Row = 1; Row < Grid.Height - 1; ++Row)
	{
		for (int32 Column = 1; Column < Grid.Width - 1; ++Column)
		{
			const int32 CellIndex = Grid.Index(Column, Row);
			CellsBelowMeasured += static_cast<int32>(Filled[CellIndex] < Measured[CellIndex]);
			CellsWithNowhereToDrain +=
				static_cast<int32>(SteepestDropMeters(Grid, Filled, Column, Row) <= 0.0);
		}
	}

	TestEqual(TEXT("The filled surface never dips below the measured one"), CellsBelowMeasured, 0);
	TestEqual(TEXT("Every cell has a lower neighbour to drain into"), CellsWithNowhereToDrain, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeStandingWaterTest,
	"Massif.WorldGen.EnvironmentBake.StandingWater",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeStandingWaterTest::RunTest(const FString& Parameters)
{
	constexpr double PitDepthMeters = 40.0;
	const TUniquePtr<FMassifEnvironmentScheduler> Pitted =
		MassifSolveTestEnvironment(MakePittedSlope(PitDepthMeters, 120.0), BakeTestSeed, MakeTestParams());
	const FMassifEnvironmentState& PittedState = Pitted->GetState();
	const FMassifChannelHandle PittedWater =
		PittedState.FindChannel(MassifEnvironmentChannels::StandingWaterDepth());

	const double PitWaterMeters = PittedState.SampleScalar(PittedWater, FVector2d::ZeroVector);
	TestTrue(FString::Printf(TEXT("Water stands in the pit (%.1f m in a %.0f m hollow)"),
		PitWaterMeters, PitDepthMeters),
		PitWaterMeters > 0.5 * PitDepthMeters && PitWaterMeters <= PitDepthMeters + 1.0);

	const double SlopeWaterMeters = PittedState.SampleScalar(PittedWater, FVector2d(250.0, 250.0));
	TestTrue(FString::Printf(TEXT("None stands on the open slope (%.3f m)"), SlopeWaterMeters),
		SlopeWaterMeters < 0.05);

	// A clean valley has no hollow anywhere, so nothing at all should pond in it.
	const TUniquePtr<FMassifEnvironmentScheduler> Valley =
		MassifSolveTestEnvironment(MakeValley(), BakeTestSeed, MakeTestParams());
	const double DeepestValleyWaterMeters = Valley->GetState().GetChannelMaximum(
		Valley->GetState().FindChannel(MassifEnvironmentChannels::StandingWaterDepth()));
	TestTrue(FString::Printf(TEXT("A valley that drains has no standing water (max %.3f m)"),
		DeepestValleyWaterMeters),
		DeepestValleyWaterMeters < 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeFlowTest,
	"Massif.WorldGen.EnvironmentBake.Flow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeFlowTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHeightmap> Heightmap = MakeValley();
	const FMassifEnvironmentBakeParams Params = MakeTestParams();
	const FMassifBakeGrid Grid = FMassifBakeGrid::FromHeightmap(*Heightmap, Params.CellSizeMeters);

	TArray<double> Measured;
	TArray<double> Filled;
	TArray<float> Accumulation;
	MassifBakeCpu::SampleAltitudes(*Heightmap, Grid, Measured);
	MassifBakeCpu::FillDepressions(Grid, Params, Measured, Filled);
	MassifBakeCpu::RouteFlow(Grid, Params, Filled, Accumulation);

	const double CellAreaSquareMeters = Grid.CellAreaSquareMeters();
	const double WorldAreaSquareMeters = CellAreaSquareMeters * Grid.CellCount();

	double LargestSquareMeters = 0.0;
	int32 CellsHoldingLessThanTheirOwnArea = 0;
	for (float CellAccumulation : Accumulation)
	{
		LargestSquareMeters = FMath::Max(LargestSquareMeters, static_cast<double>(CellAccumulation));
		CellsHoldingLessThanTheirOwnArea +=
			static_cast<int32>(CellAccumulation < CellAreaSquareMeters * 0.999);
	}

	TestEqual(TEXT("Every cell drains at least its own area"), CellsHoldingLessThanTheirOwnArea, 0);
	TestTrue(FString::Printf(TEXT("No cell drains more than the world (%.0f of %.0f m2)"),
		LargestSquareMeters, WorldAreaSquareMeters),
		LargestSquareMeters <= WorldAreaSquareMeters * 1.001);

	// The valley axis gathers both flanks; the ridge at the world edge gathers a
	// strip. If these came out the same, the routing would not be routing.
	const int32 MiddleRow = Grid.Height - 8;
	const double AxisSquareMeters = Accumulation[Grid.Index(Grid.Width / 2, MiddleRow)];
	const double FlankSquareMeters = Accumulation[Grid.Index(4, MiddleRow)];
	TestTrue(FString::Printf(TEXT("The valley axis carries far more than its flank (%.0f against %.0f m2)"),
		AxisSquareMeters, FlankSquareMeters),
		AxisSquareMeters > FlankSquareMeters * 20.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeWetnessTest,
	"Massif.WorldGen.EnvironmentBake.Wetness",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeWetnessTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Environment =
		MassifSolveTestEnvironment(MakeValley(), BakeTestSeed, MakeTestParams());
	const FMassifEnvironmentState& State = Environment->GetState();
	const FMassifChannelHandle Wetness = State.FindChannel(MassifEnvironmentChannels::Wetness());

	TestTrue(FString::Printf(TEXT("Wetness stays inside 0-1 (%.3f to %.3f)"),
		State.GetChannelMinimum(Wetness), State.GetChannelMaximum(Wetness)),
		State.GetChannelMinimum(Wetness) >= 0.0 && State.GetChannelMaximum(Wetness) <= 1.0);

	// Valley floor against the flank two hundred metres up the same cross-section.
	const double FloorWetness = State.SampleScalar(Wetness, FVector2d(0.0, -200.0));
	const double FlankWetness = State.SampleScalar(Wetness, FVector2d(300.0, -200.0));
	TestTrue(FString::Printf(TEXT("The valley floor is wetter than its flank (%.3f against %.3f)"),
		FloorWetness, FlankWetness),
		FloorWetness > FlankWetness + 0.1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeSoilTest,
	"Massif.WorldGen.EnvironmentBake.Soil",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeSoilTest::RunTest(const FString& Parameters)
{
	const FMassifEnvironmentBakeParams Params = MakeTestParams();
	const TUniquePtr<FMassifEnvironmentScheduler> Environment =
		MassifSolveTestEnvironment(MakeValley(), BakeTestSeed, Params);
	const FMassifEnvironmentState& State = Environment->GetState();
	const FMassifChannelHandle SoilDepth = State.FindChannel(MassifEnvironmentChannels::SoilDepth());

	TestTrue(FString::Printf(TEXT("Soil depth stays inside its band (%.2f to %.2f m)"),
		State.GetChannelMinimum(SoilDepth), State.GetChannelMaximum(SoilDepth)),
		State.GetChannelMinimum(SoilDepth) >= 0.0
			&& State.GetChannelMaximum(SoilDepth) <= Params.MaxSoilDepthMeters);

	// Droplets pick material up where they run fastest and drop it where the
	// ground flattens out, so the valley floor must end up better covered than
	// the flank that feeds it.
	const double FloorSoilMeters = State.SampleScalar(SoilDepth, FVector2d(0.0, -200.0));
	const double FlankSoilMeters = State.SampleScalar(SoilDepth, FVector2d(300.0, -200.0));
	TestTrue(FString::Printf(TEXT("Sediment settles on the valley floor (%.3f m against %.3f m on the flank)"),
		FloorSoilMeters, FlankSoilMeters),
		FloorSoilMeters > FlankSoilMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeDeterminismTest,
	"Massif.WorldGen.EnvironmentBake.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeDeterminismTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHeightmap> Heightmap = MakeValley();
	const FMassifEnvironmentBakeParams Params = MakeTestParams();

	const uint64 FirstHash =
		MassifSolveTestEnvironment(Heightmap, BakeTestSeed, Params)->GetState().ComputeContentHash();
	const uint64 SecondHash =
		MassifSolveTestEnvironment(Heightmap, BakeTestSeed, Params)->GetState().ComputeContentHash();
	const uint64 OtherSeedHash =
		MassifSolveTestEnvironment(Heightmap, BakeTestSeed + 1, Params)->GetState().ComputeContentHash();

	// The droplet pass runs across the thread pool and scatters into shared
	// cells. Equal hashes are what says that scatter is order-independent.
	TestEqual(TEXT("The same seed bakes the same world twice"), FirstHash, SecondHash);
	TestNotEqual(TEXT("A different seed moves the droplets"), FirstHash, OtherSeedHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeSamplingTest,
	"Massif.WorldGen.EnvironmentBake.Sampling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeSamplingTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Environment =
		MassifSolveTestEnvironment(MakeValley(), BakeTestSeed, MakeTestParams());
	const FMassifEnvironmentState& State = Environment->GetState();
	const FMassifChannelHandle Wetness = State.FindChannel(MassifEnvironmentChannels::Wetness());

	TestEqual(TEXT("The bake grid covers the world at the requested cell size"),
		State.GetGrid(Wetness).Width, BakeTestSide);

	// A position query at a cell centre has to answer that cell, or every
	// consumer of the bake is reading it half a cell off the ground it describes.
	double WorstDifference = 0.0;
	for (int32 Step = 0; Step < 32; ++Step)
	{
		const int32 Column = 3 + Step * 2;
		const int32 Row = 5 + Step;
		const FVector2d Position = TestSamplePosition(Column, Row);
		const double Sampled = State.SampleScalar(Wetness, Position);
		const double Cell = State.ReadCell(Wetness, Column, Row);
		WorstDifference = FMath::Max(WorstDifference, FMath::Abs(Sampled - Cell));
	}
	TestTrue(FString::Printf(TEXT("Sampling a cell centre reads that cell (worst %.6f)"), WorstDifference),
		WorstDifference < 1.0e-6);

	// Outside the world the bake clamps rather than reading off the end of its grid.
	const double FarOutside = State.SampleScalar(Wetness, FVector2d(50000.0, -50000.0));
	TestTrue(TEXT("Positions outside the world clamp to its border"), FarOutside >= 0.0 && FarOutside <= 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeInfiltrationTest,
	"Massif.WorldGen.EnvironmentBake.Infiltration",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeInfiltrationTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentBakeParams Params = MakeTestParams();

	// Above the deepest soil the droplets can leave, so infiltration tracks the soil
	// across its whole range instead of saturating on the valley floor, and with the
	// rock held out of it so the soil is the only thing moving.
	Params.FullInfiltrationSoilDepthMeters = 2.0 * Params.MaxSoilDepthMeters;
	Params.BedrockInfiltrationGain = 0.0;

	const TUniquePtr<FMassifEnvironmentScheduler> Environment =
		MassifSolveTestEnvironment(MakeValley(), BakeTestSeed, Params);
	const FMassifEnvironmentState& State = Environment->GetState();

	const FVector2d Floor(0.0, -200.0);
	const FVector2d Flank(300.0, -200.0);
	const double FloorSoilMeters = SampleChannel(State, MassifEnvironmentChannels::SoilDepth(), Floor);
	const double FlankSoilMeters = SampleChannel(State, MassifEnvironmentChannels::SoilDepth(), Flank);
	TestTrue(FString::Printf(TEXT("The valley floor carries the deeper soil (%.3f m against %.3f m)"),
		FloorSoilMeters, FlankSoilMeters),
		FloorSoilMeters > FlankSoilMeters);

	const double FloorShare = SampleChannel(State, MassifEnvironmentChannels::Infiltration(), Floor);
	const double FlankShare = SampleChannel(State, MassifEnvironmentChannels::Infiltration(), Flank);
	TestTrue(FString::Printf(TEXT("Deeper soil soaks up more (%.4f against %.4f)"), FloorShare, FlankShare),
		FloorShare > FlankShare);

	// And by exactly what the soil between them accounts for.
	const double ExpectedGap = Params.SoilInfiltrationGain * (FloorSoilMeters - FlankSoilMeters)
		/ Params.FullInfiltrationSoilDepthMeters;
	const double Tolerance = 2.0 * BakeQuantisationStep(State, MassifEnvironmentChannels::Infiltration())
		+ 2.0 * Params.SoilInfiltrationGain * BakeQuantisationStep(State, MassifEnvironmentChannels::SoilDepth())
			/ Params.FullInfiltrationSoilDepthMeters;
	TestTrue(FString::Printf(TEXT("The gap is the soil's (%.5f against %.5f expected)"),
		FloorShare - FlankShare, ExpectedGap),
		FMath::Abs((FloorShare - FlankShare) - ExpectedGap) <= Tolerance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeSoakedGroundTest,
	"Massif.WorldGen.EnvironmentBake.SoakedGround",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeSoakedGroundTest::RunTest(const FString& Parameters)
{
	// One world twice, differing only in how much of what lands on it soaks away.
	FMassifEnvironmentBakeParams Shedding = MakeTestParams();
	Shedding.SoilInfiltrationGain = 0.0;
	Shedding.BedrockInfiltrationGain = 0.0;
	Shedding.BaselineInfiltration = 0.05;

	FMassifEnvironmentBakeParams Soaking = Shedding;
	Soaking.BaselineInfiltration = 0.85;

	const TUniquePtr<FMassifEnvironmentScheduler> SheddingWorld =
		MassifSolveTestEnvironment(MakeValley(), BakeTestSeed, Shedding);
	const TUniquePtr<FMassifEnvironmentScheduler> SoakingWorld =
		MassifSolveTestEnvironment(MakeValley(), BakeTestSeed, Soaking);

	const FVector2d ValleyPoint(0.0, -200.0);
	const double SheddingFlow = SumOverYear(
		SheddingWorld->GetState(), MassifEnvironmentChannels::SeasonalFlow(), ValleyPoint);
	const double SoakingFlow = SumOverYear(
		SoakingWorld->GetState(), MassifEnvironmentChannels::SeasonalFlow(), ValleyPoint);

	TestTrue(FString::Printf(TEXT("Ground that sheds carries water at all (%.0f m2 over the year)"),
		SheddingFlow),
		SheddingFlow > 0.0);
	TestTrue(FString::Printf(TEXT("Ground that soaks carries less past the same point (%.0f against %.0f m2)"),
		SoakingFlow, SheddingFlow),
		SoakingFlow < SheddingFlow);

	// The loss is the infiltration and nothing else: the two worlds differ by the
	// share that soaked away, and by no other factor the seasons brought in.
	const double ExpectedRatio = (1.0 - Soaking.BaselineInfiltration) / (1.0 - Shedding.BaselineInfiltration);
	TestTrue(FString::Printf(TEXT("And carries the soaked share less (%.4f against %.4f expected)"),
		SoakingFlow / SheddingFlow, ExpectedRatio),
		FMath::Abs(SoakingFlow / SheddingFlow - ExpectedRatio) < 0.02);

	// The routed catchment is an area, not a discharge, so infiltration must leave it
	// exactly where it was.
	TestEqual(TEXT("The catchment area itself is untouched"),
		SampleChannel(SoakingWorld->GetState(), MassifEnvironmentChannels::FlowAccumulation(), ValleyPoint),
		SampleChannel(SheddingWorld->GetState(), MassifEnvironmentChannels::FlowAccumulation(), ValleyPoint));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeBareWaterBalanceTest,
	"Massif.WorldGen.EnvironmentBake.BareWaterBalance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeBareWaterBalanceTest::RunTest(const FString& Parameters)
{
	// No soil process, no rock and no weather in the graph at all: every optional
	// input the water balance declares is absent, and it still has to solve.
	const FMassifEnvironmentBakeParams Params = MakeTestParams();
	const TUniquePtr<FMassifEnvironmentScheduler> Environment =
		SolveWaterBalance(MakeSouthFacingSlope(), Params, {});
	const FMassifEnvironmentState& State = Environment->GetState();

	const FMassifChannelHandle Infiltration = State.FindChannel(MassifEnvironmentChannels::Infiltration());
	TestEqual(TEXT("Bare ground soaks up the baseline and no more"),
		State.GetChannelMaximum(Infiltration), Params.BaselineInfiltration);
	TestEqual(TEXT("...everywhere"), State.GetChannelMinimum(Infiltration), Params.BaselineInfiltration);

	TestEqual(TEXT("Rock nothing knows anything about yields no springs"),
		State.GetChannelMaximum(State.FindChannel(MassifEnvironmentChannels::SpringDischarge())), 0.0);

	// A year nothing is known about is a year whose water arrives evenly.
	const FVector2d SlopePoint(0.0, -100.0);
	const double Expected = WaterBalanceCatchmentSquareMeters * (1.0 - Params.BaselineInfiltration);
	const double Step = BakeQuantisationStep(State, MassifEnvironmentChannels::SeasonalFlow());

	double WorstDifference = 0.0;
	for (int32 TimeSlice = 0; TimeSlice < 24; ++TimeSlice)
	{
		const double SliceFlow =
			SampleChannel(State, MassifEnvironmentChannels::SeasonalFlow(), SlopePoint, TimeSlice);
		WorstDifference = FMath::Max(WorstDifference, FMath::Abs(SliceFlow - Expected));
	}
	TestTrue(FString::Printf(TEXT("Every half-month carries the same %.0f m2 (worst gap %.4f)"),
		Expected, WorstDifference),
		WorstDifference <= Step);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeSpringTest,
	"Massif.WorldGen.EnvironmentBake.Springs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeSpringTest::RunTest(const FString& Parameters)
{
	const FMassifEnvironmentBakeParams Params = MakeTestParams();

	// Permeable rock upslope of the northing zero line, tight rock below it: one
	// contact, on a plane where uphill is due north and nothing else.
	const TUniquePtr<FMassifEnvironmentScheduler> Contact = SolveWaterBalance(
		MakeSouthFacingSlope(), Params, {MakeShared<FSteppedSoilYieldProcess>(0.9, 0.1)});
	const FMassifEnvironmentState& ContactState = Contact->GetState();

	// Far enough below the contact that the comparison reaches across it.
	const FVector2d BelowContact(0.0, -12.0);
	const double Discharge =
		SampleChannel(ContactState, MassifEnvironmentChannels::SpringDischarge(), BelowContact);
	TestTrue(FString::Printf(TEXT("Water comes back out below the contact (%.0f m2)"), Discharge),
		Discharge > 0.0);

	// All of what soaked in above it, since the drop in yield is far past the span
	// that returns everything.
	const double UphillShare = SampleChannel(
		ContactState, MassifEnvironmentChannels::Infiltration(), FVector2d(0.0, 12.0));
	const double Expected = WaterBalanceCatchmentSquareMeters * UphillShare;
	TestTrue(FString::Printf(TEXT("And all of it: %.0f m2 against %.0f soaked in above"), Discharge, Expected),
		FMath::Abs(Discharge - Expected)
			<= 2.0 * BakeQuantisationStep(ContactState, MassifEnvironmentChannels::SpringDischarge()));

	// Nothing to re-emerge from where the rock never changes.
	const TUniquePtr<FMassifEnvironmentScheduler> Uniform = SolveWaterBalance(
		MakeSouthFacingSlope(), Params, {MakeShared<FSteppedSoilYieldProcess>(0.5, 0.5)});
	TestEqual(TEXT("Uniform rock springs nowhere"),
		Uniform->GetState().GetChannelMaximum(
			Uniform->GetState().FindChannel(MassifEnvironmentChannels::SpringDischarge())),
		0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeSeasonalFlowTest,
	"Massif.WorldGen.EnvironmentBake.SeasonalFlow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeSeasonalFlowTest::RunTest(const FString& Parameters)
{
	// Every millimetre of the year falls as snow and leaves as melt, so the flow
	// through a point is the pack releasing and nothing else.
	const FMassifEnvironmentBakeParams Params = MakeTestParams();
	const TUniquePtr<FMassifEnvironmentScheduler> Environment =
		SolveWaterBalance(MakeSouthFacingSlope(), Params, {MakeShared<FTestSnowpackProcess>()});
	const FMassifEnvironmentState& State = Environment->GetState();

	constexpr int32 MeltSlice = 14;
	constexpr int32 DeepWinterSlice = 6;
	const FVector2d SlopePoint(0.0, -100.0);
	const double MeltFlow =
		SampleChannel(State, MassifEnvironmentChannels::SeasonalFlow(), SlopePoint, MeltSlice);
	const double WinterFlow =
		SampleChannel(State, MassifEnvironmentChannels::SeasonalFlow(), SlopePoint, DeepWinterSlice);
	TestTrue(FString::Printf(TEXT("The melt half-months carry more than the deep winter ones (%.0f against %.0f m2)"),
		MeltFlow, WinterFlow),
		MeltFlow > WinterFlow);

	// The pulse redistributes the year's water; it does not invent any. Averaged
	// over the year the flow is the runoff the catchment gives.
	const double Runoff = WaterBalanceCatchmentSquareMeters * (1.0 - Params.BaselineInfiltration);
	const double YearMean = SumOverYear(State, MassifEnvironmentChannels::SeasonalFlow(), SlopePoint) / 24.0;
	TestTrue(FString::Printf(TEXT("The year still averages the runoff (%.1f against %.1f m2)"),
		YearMean, Runoff),
		FMath::Abs(YearMean - Runoff) <= BakeQuantisationStep(State, MassifEnvironmentChannels::SeasonalFlow()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeWaterBalanceOffTest,
	"Massif.WorldGen.EnvironmentBake.WaterBalanceOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeWaterBalanceOffTest::RunTest(const FString& Parameters)
{
	// Off is a level like any other: the channels exist and hold trivial values, so
	// nothing downstream has to ask whether the water balance was solved.
	const FMassifEnvironmentBakeParams Params = MakeTestParams();
	const TUniquePtr<FMassifEnvironmentScheduler> Environment = SolveWaterBalance(
		MakeSouthFacingSlope(), Params, {MakeShared<FSteppedSoilYieldProcess>(0.9, 0.1)},
		EMassifFidelity::Off);
	const FMassifEnvironmentState& State = Environment->GetState();

	const FMassifChannelHandle Infiltration = State.FindChannel(MassifEnvironmentChannels::Infiltration());
	const FMassifChannelHandle Springs = State.FindChannel(MassifEnvironmentChannels::SpringDischarge());
	const FMassifChannelHandle SeasonalFlow = State.FindChannel(MassifEnvironmentChannels::SeasonalFlow());
	TestTrue(TEXT("Infiltration was written"), State.IsChannelWritten(Infiltration));
	TestTrue(TEXT("Spring discharge was written"), State.IsChannelWritten(Springs));
	TestTrue(TEXT("Seasonal flow was written"), State.IsChannelWritten(SeasonalFlow));

	TestEqual(TEXT("Infiltration is the baseline everywhere"),
		State.GetChannelMinimum(Infiltration), Params.BaselineInfiltration);
	TestEqual(TEXT("...and nowhere more, whatever the rock says"),
		State.GetChannelMaximum(Infiltration), Params.BaselineInfiltration);
	TestEqual(TEXT("Nothing springs"), State.GetChannelMaximum(Springs), 0.0);

	// The catchment area, spread evenly over the year and undiminished.
	double WorstDifference = 0.0;
	for (int32 TimeSlice = 0; TimeSlice < 24; ++TimeSlice)
	{
		const double SliceFlow = State.SampleScalar(SeasonalFlow, FVector2d(0.0, -100.0), TimeSlice);
		WorstDifference =
			FMath::Max(WorstDifference, FMath::Abs(SliceFlow - WaterBalanceCatchmentSquareMeters));
	}
	TestTrue(FString::Printf(TEXT("Every half-month carries the whole catchment (worst gap %.4f m2)"),
		WorstDifference),
		WorstDifference <= BakeQuantisationStep(State, MassifEnvironmentChannels::SeasonalFlow()));
	return true;
}
