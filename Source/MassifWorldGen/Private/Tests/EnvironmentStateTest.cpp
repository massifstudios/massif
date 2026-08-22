#include "MassifEnvironmentState.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr double StateTestWorldSideMeters = 768.0;
	constexpr double StateTestCellSizeMeters = 8.0;

	constexpr double ClassifiedWestValue = 1.0;
	constexpr double ClassifiedEastValue = 4.0;

	FVector2d StateTestWorldSize()
	{
		return FVector2d(StateTestWorldSideMeters, StateTestWorldSideMeters);
	}

	FMassifChannelDescriptor MakeDescriptor(
		const TCHAR* Name, EMassifChannelEncoding Encoding, double CellSizeMeters = StateTestCellSizeMeters)
	{
		FMassifChannelDescriptor Descriptor;
		Descriptor.Name = FName(Name);
		Descriptor.Encoding = Encoding;
		Descriptor.CellSizeMeters = CellSizeMeters;
		return Descriptor;
	}

	void FillComponent(
		const FMassifChannelGrid& Grid, TArrayView<double> Values, TFunctionRef<double(FVector2d)> ValueAt)
	{
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				Values[Grid.CellIndex(Column, Row)] = ValueAt(Grid.CellCentreMeters(Column, Row));
			}
		}
	}

	void CommitScalarSlice(
		FMassifEnvironmentState& State, FMassifChannelHandle Channel, int32 TimeSlice,
		TFunctionRef<double(FVector2d)> ValueAt)
	{
		FMassifChannelWriteScope Scope(State, Channel, TimeSlice);
		FillComponent(Scope.GetGrid(), Scope.GetComponent(0), ValueAt);
		Scope.Commit();
	}

	void CommitVectorSlice(
		FMassifEnvironmentState& State, FMassifChannelHandle Channel, int32 TimeSlice,
		TFunctionRef<double(FVector2d)> EastingAt, TFunctionRef<double(FVector2d)> NorthingAt)
	{
		FMassifChannelWriteScope Scope(State, Channel, TimeSlice);
		FillComponent(Scope.GetGrid(), Scope.GetComponent(0), EastingAt);
		FillComponent(Scope.GetGrid(), Scope.GetComponent(1), NorthingAt);
		Scope.Commit();
	}

	/** A ramp whose every cell-centre value is exact in float, so Float32 has nothing to round. */
	double RampValueAt(FVector2d Position)
	{
		return 30.0 + Position.X * 0.0625;
	}

	/** A hillside to a river in one ramp: 10^0 to 10^6 across the world. */
	double DecadeRampValueAt(FVector2d Position)
	{
		const double Fraction = (Position.X + 0.5 * StateTestWorldSideMeters) / StateTestWorldSideMeters;
		return FMath::Pow(10.0, 6.0 * Fraction);
	}

	double LinearFieldAt(FVector2d Position)
	{
		return 100.0 + 0.01 * Position.X - 0.02 * Position.Y;
	}

	/**
	 * A lake surface as the depression fill leaves it: 2000 m with a tenth of a
	 * micrometre of rise a column. A float at 2000 m resolves a tenth of a
	 * millimetre, so the whole ramp collapses onto one value.
	 */
	double FineAltitudeAtColumn(int32 Column)
	{
		const double Rise = Column * 1.0e-7;
		return 2000.0 + Rise;
	}

	void CommitFineAltitudeRamp(FMassifEnvironmentState& State, FMassifChannelHandle Channel)
	{
		FMassifChannelWriteScope Scope(State, Channel);
		const FMassifChannelGrid& Grid = Scope.GetGrid();
		TArrayView<double> Values = Scope.GetComponent(0);
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				Values[Grid.CellIndex(Column, Row)] = FineAltitudeAtColumn(Column);
			}
		}
		Scope.Commit();
	}

	/** The same tenth-of-a-micrometre steps on a per-slice base, for vector and multi-slice channels. */
	double FineComponentAt(double CoordinateMeters, int32 TimeSlice)
	{
		const double Rise = CoordinateMeters * 1.0e-7;
		return 2000.0 + TimeSlice + Rise;
	}

	int32 CountFineCellMismatches(
		const FMassifEnvironmentState& State, FMassifChannelHandle Channel, int32 Column, int32 Row, int32 TimeSlice)
	{
		const FVector2d Centre = State.GetGrid(Channel).CellCentreMeters(Column, Row);
		const bool bEastingExact =
			State.ReadCell(Channel, Column, Row, 0, TimeSlice) == FineComponentAt(Centre.X, TimeSlice);
		const bool bNorthingExact =
			State.ReadCell(Channel, Column, Row, 1, TimeSlice) == FineComponentAt(Centre.Y, TimeSlice);
		return static_cast<int32>(!bEastingExact) + static_cast<int32>(!bNorthingExact);
	}

	int32 CountFineSliceMismatches(
		const FMassifEnvironmentState& State, FMassifChannelHandle Channel, int32 TimeSlice)
	{
		const FMassifChannelGrid& Grid = State.GetGrid(Channel);
		int32 Mismatches = 0;
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				Mismatches += CountFineCellMismatches(State, Channel, Column, Row, TimeSlice);
			}
		}
		return Mismatches;
	}

	double WorstCellError(
		const FMassifEnvironmentState& State, FMassifChannelHandle Channel, TFunctionRef<double(FVector2d)> ValueAt)
	{
		const FMassifChannelGrid& Grid = State.GetGrid(Channel);
		double WorstError = 0.0;
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				const double Expected = ValueAt(Grid.CellCentreMeters(Column, Row));
				WorstError = FMath::Max(WorstError, FMath::Abs(State.ReadCell(Channel, Column, Row) - Expected));
			}
		}
		return WorstError;
	}

	double WorstRelativeCellError(
		const FMassifEnvironmentState& State, FMassifChannelHandle Channel, TFunctionRef<double(FVector2d)> ValueAt)
	{
		const FMassifChannelGrid& Grid = State.GetGrid(Channel);
		double WorstError = 0.0;
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				const double Expected = ValueAt(Grid.CellCentreMeters(Column, Row));
				const double Error = FMath::Abs(State.ReadCell(Channel, Column, Row) - Expected) / Expected;
				WorstError = FMath::Max(WorstError, Error);
			}
		}
		return WorstError;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentStateEncodingTest,
	"Massif.WorldGen.EnvironmentState.Encodings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentStateEncodingTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(StateTestWorldSize());
	const FMassifChannelHandle Uint8Channel =
		State.RegisterChannel(MakeDescriptor(TEXT("Test.RampUint8"), EMassifChannelEncoding::Uint8));
	const FMassifChannelHandle Uint16Channel =
		State.RegisterChannel(MakeDescriptor(TEXT("Test.RampUint16"), EMassifChannelEncoding::Uint16));
	const FMassifChannelHandle Float32Channel =
		State.RegisterChannel(MakeDescriptor(TEXT("Test.RampFloat32"), EMassifChannelEncoding::Float32));
	const FMassifChannelHandle LogarithmicChannel =
		State.RegisterChannel(MakeDescriptor(TEXT("Test.RampLogarithmic16"), EMassifChannelEncoding::Logarithmic16));

	for (FMassifChannelHandle Channel : State.GetAllChannels())
	{
		CommitScalarSlice(State, Channel, 0, [](FVector2d Position) { return RampValueAt(Position); });
	}

	const double Span = State.GetChannelMaximum(Uint16Channel) - State.GetChannelMinimum(Uint16Channel);
	const double Uint8Error =
		WorstCellError(State, Uint8Channel, [](FVector2d Position) { return RampValueAt(Position); });
	const double Uint16Error =
		WorstCellError(State, Uint16Channel, [](FVector2d Position) { return RampValueAt(Position); });
	const double Float32Error =
		WorstCellError(State, Float32Channel, [](FVector2d Position) { return RampValueAt(Position); });

	TestTrue(FString::Printf(TEXT("Uint8 holds the ramp to its step (%.4f of %.4f)"), Uint8Error, Span / 255.0),
		Uint8Error <= Span / 255.0);
	TestTrue(FString::Printf(TEXT("Uint16 holds the ramp to its step (%.7f of %.7f)"), Uint16Error, Span / 65535.0),
		Uint16Error <= Span / 65535.0);
	TestTrue(FString::Printf(TEXT("Float32 reads back exactly (%.9f)"), Float32Error), Float32Error == 0.0);

	// The logarithmic step is a step in log space, so its width in metres grows with the value.
	const double LogarithmicSpan = FMath::Loge(1.0 + State.GetChannelMaximum(LogarithmicChannel))
		- FMath::Loge(1.0 + State.GetChannelMinimum(LogarithmicChannel));
	const double LogarithmicTolerance =
		(1.0 + State.GetChannelMaximum(LogarithmicChannel)) * LogarithmicSpan / 65535.0;
	const double LogarithmicError =
		WorstCellError(State, LogarithmicChannel, [](FVector2d Position) { return RampValueAt(Position); });
	TestTrue(FString::Printf(TEXT("Logarithmic16 holds the ramp to its step (%.7f of %.7f)"),
		LogarithmicError, LogarithmicTolerance),
		LogarithmicError <= LogarithmicTolerance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentStateFloat64Test,
	"Massif.WorldGen.EnvironmentState.Float64",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentStateFloat64Test::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(StateTestWorldSize());
	const FMassifChannelHandle Float64Channel =
		State.RegisterChannel(MakeDescriptor(TEXT("Test.AltitudeFloat64"), EMassifChannelEncoding::Float64));
	const FMassifChannelHandle Float32Channel =
		State.RegisterChannel(MakeDescriptor(TEXT("Test.AltitudeFloat32"), EMassifChannelEncoding::Float32));

	CommitFineAltitudeRamp(State, Float64Channel);
	CommitFineAltitudeRamp(State, Float32Channel);

	const FMassifChannelGrid& Grid = State.GetGrid(Float64Channel);
	int32 Float64Mismatches = 0;
	int32 Float32Mismatches = 0;
	for (int32 Column = 0; Column < Grid.Width; ++Column)
	{
		Float64Mismatches +=
			static_cast<int32>(State.ReadCell(Float64Channel, Column, 0) != FineAltitudeAtColumn(Column));
		Float32Mismatches +=
			static_cast<int32>(State.ReadCell(Float32Channel, Column, 0) != FineAltitudeAtColumn(Column));
	}

	TestEqual(TEXT("Float64 reads every altitude back bit-exactly"), Float64Mismatches, 0);
	TestTrue(FString::Printf(TEXT("Float32 cannot hold the same ramp (%d of %d cells differ)"),
		Float32Mismatches, Grid.Width),
		Float32Mismatches > 0);

	// The point of the encoding: the rise a depression fill leaves behind is what
	// the flow routing follows out of a filled lake, and in float there is none.
	int32 Float64StepsWithoutRise = 0;
	int32 Float32StepsWithoutRise = 0;
	for (int32 Column = 1; Column < Grid.Width; ++Column)
	{
		Float64StepsWithoutRise += static_cast<int32>(
			State.ReadCell(Float64Channel, Column, 0) <= State.ReadCell(Float64Channel, Column - 1, 0));
		Float32StepsWithoutRise += static_cast<int32>(
			State.ReadCell(Float32Channel, Column, 0) <= State.ReadCell(Float32Channel, Column - 1, 0));
	}

	TestEqual(TEXT("Every Float64 step still rises above the one before it"), Float64StepsWithoutRise, 0);
	TestEqual(TEXT("In Float32 the whole ramp is flat"), Float32StepsWithoutRise, Grid.Width - 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentStateFloat64ShapesTest,
	"Massif.WorldGen.EnvironmentState.Float64Shapes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentStateFloat64ShapesTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(StateTestWorldSize());
	FMassifChannelDescriptor Descriptor = MakeDescriptor(TEXT("Test.FineFlow"), EMassifChannelEncoding::Float64);
	Descriptor.Type = EMassifChannelType::Vector2;
	Descriptor.TimeSliceCount = 3;
	const FMassifChannelHandle Channel = State.RegisterChannel(Descriptor);

	for (int32 TimeSlice = 0; TimeSlice < Descriptor.TimeSliceCount; ++TimeSlice)
	{
		CommitVectorSlice(State, Channel, TimeSlice,
			[TimeSlice](FVector2d Position) { return FineComponentAt(Position.X, TimeSlice); },
			[TimeSlice](FVector2d Position) { return FineComponentAt(Position.Y, TimeSlice); });
	}

	int32 Mismatches = 0;
	for (int32 TimeSlice = 0; TimeSlice < Descriptor.TimeSliceCount; ++TimeSlice)
	{
		Mismatches += CountFineSliceMismatches(State, Channel, TimeSlice);
	}
	TestEqual(TEXT("Both components of every slice read back bit-exactly"), Mismatches, 0);

	const int64 ExpectedBytes =
		static_cast<int64>(State.GetGrid(Channel).CellCount()) * 2 * Descriptor.TimeSliceCount * 8;
	TestTrue(FString::Printf(TEXT("Float64 holds eight bytes a cell for each component of each slice (%lld of %lld)"),
		State.GetResidentByteCount(), ExpectedBytes),
		State.GetResidentByteCount() == ExpectedBytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentStateLogarithmicRangeTest,
	"Massif.WorldGen.EnvironmentState.LogarithmicRange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentStateLogarithmicRangeTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(StateTestWorldSize());
	const FMassifChannelHandle LogarithmicChannel =
		State.RegisterChannel(MakeDescriptor(TEXT("Test.DecadesLogarithmic16"), EMassifChannelEncoding::Logarithmic16));
	const FMassifChannelHandle LinearChannel =
		State.RegisterChannel(MakeDescriptor(TEXT("Test.DecadesUint16"), EMassifChannelEncoding::Uint16));

	CommitScalarSlice(State, LogarithmicChannel, 0, [](FVector2d Position) { return DecadeRampValueAt(Position); });
	CommitScalarSlice(State, LinearChannel, 0, [](FVector2d Position) { return DecadeRampValueAt(Position); });

	const double LogarithmicError =
		WorstRelativeCellError(State, LogarithmicChannel, [](FVector2d Position) { return DecadeRampValueAt(Position); });
	const double LinearError =
		WorstRelativeCellError(State, LinearChannel, [](FVector2d Position) { return DecadeRampValueAt(Position); });

	TestTrue(FString::Printf(TEXT("Logarithmic16 keeps relative precision over six decades (%.6f)"),
		LogarithmicError),
		LogarithmicError < 0.001);

	// The same range in a linear uint16 has a step several times the whole bottom decade,
	// so the small end collapses onto the minimum.
	TestTrue(FString::Printf(TEXT("Uint16 loses the small end of the same range (%.3f)"), LinearError),
		LinearError > 0.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentStateSamplingTest,
	"Massif.WorldGen.EnvironmentState.Sampling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentStateSamplingTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(StateTestWorldSize());
	const FMassifChannelHandle Channel =
		State.RegisterChannel(MakeDescriptor(TEXT("Test.Sampled"), EMassifChannelEncoding::Float32));
	CommitScalarSlice(State, Channel, 0, [](FVector2d Position)
	{
		return FMath::Sin(Position.X * 0.01) + FMath::Cos(Position.Y * 0.013);
	});

	const FMassifChannelGrid& Grid = State.GetGrid(Channel);

	double WorstCentreDifference = 0.0;
	for (int32 Step = 0; Step < 32; ++Step)
	{
		const int32 Column = 3 + Step * 2;
		const int32 Row = 5 + Step;
		const double Sampled = State.SampleScalar(Channel, Grid.CellCentreMeters(Column, Row));
		WorstCentreDifference =
			FMath::Max(WorstCentreDifference, FMath::Abs(Sampled - State.ReadCell(Channel, Column, Row)));
	}
	TestTrue(FString::Printf(TEXT("Sampling a cell centre reads that cell (worst %.9f)"), WorstCentreDifference),
		WorstCentreDifference < 1.0e-9);

	// A quarter of a cell east and half a cell south of a centre: the blend of the four cells around it.
	constexpr int32 ProbeColumn = 40;
	constexpr int32 ProbeRow = 22;
	const FVector2d Probe = Grid.CellCentreMeters(ProbeColumn, ProbeRow)
		+ FVector2d(0.25 * Grid.CellSizeMeters, -0.5 * Grid.CellSizeMeters);
	const double NorthEdge = FMath::Lerp(
		State.ReadCell(Channel, ProbeColumn, ProbeRow), State.ReadCell(Channel, ProbeColumn + 1, ProbeRow), 0.25);
	const double SouthEdge = FMath::Lerp(
		State.ReadCell(Channel, ProbeColumn, ProbeRow + 1),
		State.ReadCell(Channel, ProbeColumn + 1, ProbeRow + 1), 0.25);
	const double Expected = FMath::Lerp(NorthEdge, SouthEdge, 0.5);
	const double Sampled = State.SampleScalar(Channel, Probe);
	TestTrue(FString::Printf(TEXT("Between centres it is the bilinear blend (%.9f against %.9f)"), Sampled, Expected),
		FMath::Abs(Sampled - Expected) < 1.0e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentStateClampingTest,
	"Massif.WorldGen.EnvironmentState.Clamping",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentStateClampingTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(StateTestWorldSize());
	const FMassifChannelHandle Channel =
		State.RegisterChannel(MakeDescriptor(TEXT("Test.Clamped"), EMassifChannelEncoding::Float32));
	CommitScalarSlice(State, Channel, 0, [](FVector2d Position) { return LinearFieldAt(Position); });

	const FMassifChannelGrid& Grid = State.GetGrid(Channel);

	// Far north-east clamps to the last column of the first row, far south-west to the first column of the last.
	const double NorthEast = State.SampleScalar(Channel, FVector2d(50000.0, 50000.0));
	const double SouthWest = State.SampleScalar(Channel, FVector2d(-50000.0, -50000.0));
	const double NorthEastCell = State.ReadCell(Channel, Grid.Width - 1, 0);
	const double SouthWestCell = State.ReadCell(Channel, 0, Grid.Height - 1);

	TestTrue(FString::Printf(TEXT("North-east of the world clamps to its border (%.6f against %.6f)"),
		NorthEast, NorthEastCell),
		FMath::Abs(NorthEast - NorthEastCell) < 1.0e-9);
	TestTrue(FString::Printf(TEXT("South-west of the world clamps to its border (%.6f against %.6f)"),
		SouthWest, SouthWestCell),
		FMath::Abs(SouthWest - SouthWestCell) < 1.0e-9);
	TestTrue(TEXT("The two borders are not the same value, so nothing wrapped"),
		FMath::Abs(NorthEast - SouthWest) > 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentStateResolutionTest,
	"Massif.WorldGen.EnvironmentState.Resolutions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentStateResolutionTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(StateTestWorldSize());
	const FMassifChannelHandle FineChannel =
		State.RegisterChannel(MakeDescriptor(TEXT("Test.Fine"), EMassifChannelEncoding::Float32, 8.0));
	const FMassifChannelHandle CoarseChannel =
		State.RegisterChannel(MakeDescriptor(TEXT("Test.Coarse"), EMassifChannelEncoding::Float32, 64.0));

	// Bilinear reconstructs a linear field exactly, so any disagreement between
	// the two is the two grids not covering the same world box.
	CommitScalarSlice(State, FineChannel, 0, [](FVector2d Position) { return LinearFieldAt(Position); });
	CommitScalarSlice(State, CoarseChannel, 0, [](FVector2d Position) { return LinearFieldAt(Position); });

	TestEqual(TEXT("The fine channel covers the world at 8 m"), State.GetGrid(FineChannel).Width, 96);
	TestEqual(TEXT("The coarse channel covers the same world at 64 m"), State.GetGrid(CoarseChannel).Width, 12);

	double WorstDifference = 0.0;
	for (int32 Step = 0; Step < 24; ++Step)
	{
		const FVector2d Position(-300.0 + Step * 25.0, 280.0 - Step * 23.0);
		WorstDifference = FMath::Max(WorstDifference,
			FMath::Abs(State.SampleScalar(FineChannel, Position) - State.SampleScalar(CoarseChannel, Position)));
	}
	TestTrue(FString::Printf(TEXT("8 m and 64 m agree at the same world position (worst %.6f)"), WorstDifference),
		WorstDifference < 1.0e-3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentStateClassifiedTest,
	"Massif.WorldGen.EnvironmentState.Classified",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentStateClassifiedTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(StateTestWorldSize());
	FMassifChannelDescriptor Descriptor = MakeDescriptor(TEXT("Test.Classes"), EMassifChannelEncoding::Uint8);
	Descriptor.Type = EMassifChannelType::Classified;
	Descriptor.ColorMap = EMassifChannelColorMap::Categorical;
	const FMassifChannelHandle Channel = State.RegisterChannel(Descriptor);

	// The two classes meet on the X = 0 line, so every sample near it is a chance to interpolate.
	CommitScalarSlice(State, Channel, 0, [](FVector2d Position)
	{
		if (Position.X < 0.0)
		{
			return ClassifiedWestValue;
		}
		return ClassifiedEastValue;
	});

	int32 SamplesBetweenClasses = 0;
	int32 ClassesOutsideTheTwo = 0;
	for (int32 Step = 0; Step < 64; ++Step)
	{
		const FVector2d Position(-20.0 + Step * 0.625, 40.0);
		const double Sampled = State.SampleScalar(Channel, Position);
		const int32 SampledClass = State.SampleClass(Channel, Position);
		SamplesBetweenClasses += static_cast<int32>(
			FMath::Abs(Sampled - ClassifiedWestValue) > 1.0e-6
				&& FMath::Abs(Sampled - ClassifiedEastValue) > 1.0e-6);
		ClassesOutsideTheTwo += static_cast<int32>(SampledClass != 1 && SampledClass != 4);
	}

	TestEqual(TEXT("No sample across the boundary lands between the classes"), SamplesBetweenClasses, 0);
	TestEqual(TEXT("Every class read is one of the two"), ClassesOutsideTheTwo, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentStateVectorTest,
	"Massif.WorldGen.EnvironmentState.Vector",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentStateVectorTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(StateTestWorldSize());
	FMassifChannelDescriptor Descriptor = MakeDescriptor(TEXT("Test.Wind"), EMassifChannelEncoding::Float32);
	Descriptor.Type = EMassifChannelType::Vector2;
	const FMassifChannelHandle Channel = State.RegisterChannel(Descriptor);

	CommitVectorSlice(State, Channel, 0,
		[](FVector2d Position) { return 0.5 + Position.X * 0.001; },
		[](FVector2d Position) { return -20.0 + Position.Y * 0.002; });

	const FMassifChannelGrid& Grid = State.GetGrid(Channel);

	double WorstEastingError = 0.0;
	double WorstNorthingError = 0.0;
	for (int32 Step = 0; Step < 32; ++Step)
	{
		const FVector2d Centre = Grid.CellCentreMeters(7 + Step * 2, 11 + Step);
		const FVector2d Sampled = State.SampleVector(Channel, Centre);
		WorstEastingError = FMath::Max(WorstEastingError, FMath::Abs(Sampled.X - (0.5 + Centre.X * 0.001)));
		WorstNorthingError = FMath::Max(WorstNorthingError, FMath::Abs(Sampled.Y - (-20.0 + Centre.Y * 0.002)));
	}

	TestTrue(FString::Printf(TEXT("Both components round-trip (%.7f and %.7f)"),
		WorstEastingError, WorstNorthingError),
		WorstEastingError < 1.0e-5 && WorstNorthingError < 1.0e-5);

	// Ranges are tracked per component, so one component cannot borrow the other's legend.
	TestTrue(FString::Printf(TEXT("The components keep their own ranges (%.3f and %.3f)"),
		State.GetChannelMaximum(Channel, 0), State.GetChannelMaximum(Channel, 1)),
		State.GetChannelMaximum(Channel, 0) < 1.0 && State.GetChannelMaximum(Channel, 1) < -19.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentStateTimeSliceTest,
	"Massif.WorldGen.EnvironmentState.TimeSlices",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentStateTimeSliceTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(StateTestWorldSize());
	FMassifChannelDescriptor Descriptor = MakeDescriptor(TEXT("Test.Sliced"), EMassifChannelEncoding::Float32);
	Descriptor.TimeSliceCount = 4;
	const FMassifChannelHandle Channel = State.RegisterChannel(Descriptor);

	CommitScalarSlice(State, Channel, 0, [](FVector2d Position) { return 10.0 + Position.X * 0.0625; });
	CommitScalarSlice(State, Channel, 3, [](FVector2d Position) { return 500.0 + Position.Y * 0.0625; });

	const FMassifChannelGrid& Grid = State.GetGrid(Channel);
	const FVector2d Probe = Grid.CellCentreMeters(30, 40);

	const double FirstSlice = State.SampleScalar(Channel, Probe, 0);
	const double LastSlice = State.SampleScalar(Channel, Probe, 3);
	TestTrue(FString::Printf(TEXT("Writing slice 3 leaves slice 0 alone (%.4f)"), FirstSlice),
		FMath::Abs(FirstSlice - (10.0 + Probe.X * 0.0625)) < 1.0e-6);
	TestTrue(FString::Printf(TEXT("Slice 3 holds what was written to it (%.4f)"), LastSlice),
		FMath::Abs(LastSlice - (500.0 + Probe.Y * 0.0625)) < 1.0e-6);
	TestTrue(TEXT("An unwritten slice reads zero"), State.SampleScalar(Channel, Probe, 1) == 0.0);

	// One legend for the whole series, or scrubbing time would rescale the debug view under the cursor.
	TestTrue(FString::Printf(TEXT("The channel range spans every slice (%.3f to %.3f)"),
		State.GetChannelMinimum(Channel), State.GetChannelMaximum(Channel)),
		State.GetChannelMinimum(Channel) < 0.0 && State.GetChannelMaximum(Channel) > 500.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentStateContentHashTest,
	"Massif.WorldGen.EnvironmentState.ContentHash",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentStateContentHashTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState FirstState(StateTestWorldSize());
	const FMassifChannelHandle FirstAlpha =
		FirstState.RegisterChannel(MakeDescriptor(TEXT("Test.Alpha"), EMassifChannelEncoding::Uint16));
	const FMassifChannelHandle FirstBeta =
		FirstState.RegisterChannel(MakeDescriptor(TEXT("Test.Beta"), EMassifChannelEncoding::Uint16));
	CommitScalarSlice(FirstState, FirstAlpha, 0, [](FVector2d Position) { return LinearFieldAt(Position); });
	CommitScalarSlice(FirstState, FirstBeta, 0, [](FVector2d Position) { return RampValueAt(Position); });

	FMassifEnvironmentState SecondState(StateTestWorldSize());
	const FMassifChannelHandle SecondBeta =
		SecondState.RegisterChannel(MakeDescriptor(TEXT("Test.Beta"), EMassifChannelEncoding::Uint16));
	const FMassifChannelHandle SecondAlpha =
		SecondState.RegisterChannel(MakeDescriptor(TEXT("Test.Alpha"), EMassifChannelEncoding::Uint16));
	CommitScalarSlice(SecondState, SecondBeta, 0, [](FVector2d Position) { return RampValueAt(Position); });
	CommitScalarSlice(SecondState, SecondAlpha, 0, [](FVector2d Position) { return LinearFieldAt(Position); });

	TestEqual(TEXT("Registration order cannot change the hash"),
		FirstState.ComputeContentHash(), SecondState.ComputeContentHash());

	FMassifEnvironmentState ThirdState(StateTestWorldSize());
	const FMassifChannelHandle ThirdAlpha =
		ThirdState.RegisterChannel(MakeDescriptor(TEXT("Test.Alpha"), EMassifChannelEncoding::Uint16));
	const FMassifChannelHandle ThirdBeta =
		ThirdState.RegisterChannel(MakeDescriptor(TEXT("Test.Beta"), EMassifChannelEncoding::Uint16));
	CommitScalarSlice(ThirdState, ThirdAlpha, 0, [](FVector2d Position)
	{
		if (Position.X > 100.0 && Position.Y > 100.0)
		{
			return LinearFieldAt(Position) + 5.0;
		}
		return LinearFieldAt(Position);
	});
	CommitScalarSlice(ThirdState, ThirdBeta, 0, [](FVector2d Position) { return RampValueAt(Position); });

	TestNotEqual(TEXT("A changed value changes the hash"),
		FirstState.ComputeContentHash(), ThirdState.ComputeContentHash());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentStateRegistrationTest,
	"Massif.WorldGen.EnvironmentState.Registration",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentStateRegistrationTest::RunTest(const FString& Parameters)
{
	FMassifEnvironmentState State(StateTestWorldSize());
	const FMassifChannelDescriptor Descriptor =
		MakeDescriptor(TEXT("Test.Repeated"), EMassifChannelEncoding::Uint16);

	const FMassifChannelHandle First = State.RegisterChannel(Descriptor);
	const FMassifChannelHandle Second = State.RegisterChannel(Descriptor);

	TestTrue(TEXT("Re-registering an identical descriptor returns the same handle"), First == Second);
	TestEqual(TEXT("...and does not add a second channel"), State.GetChannelCount(), 1);
	TestTrue(TEXT("The channel is found by its name"), State.FindChannel(FName(TEXT("Test.Repeated"))) == First);
	TestFalse(TEXT("An unregistered name has no handle"),
		State.FindChannel(FName(TEXT("Test.Unregistered"))).IsValid());
	TestFalse(TEXT("Nothing is written before a commit"), State.IsChannelWritten(First));
	TestTrue(TEXT("An uncommitted channel holds no bytes"), State.GetResidentByteCount() == 0);

	CommitScalarSlice(State, First, 0, [](FVector2d Position) { return RampValueAt(Position); });

	const int64 ExpectedBytes = static_cast<int64>(State.GetGrid(First).CellCount()) * 2;
	TestTrue(TEXT("A committed uint16 channel holds two bytes a cell"),
		State.GetResidentByteCount() == ExpectedBytes);
	TestTrue(TEXT("The channel reports itself written"), State.IsChannelWritten(First));
	return true;
}
