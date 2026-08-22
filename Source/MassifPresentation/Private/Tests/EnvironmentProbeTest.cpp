#include "Debug/MassifEnvironmentProbe.h"
#include "MassifTestEnvironment.h"
#include "Misc/AutomationTest.h"

namespace
{
	/** Off a cell centre on every grid in the fixture, so an interpolated read differs from a nearest one. */
	const FVector2d ProbePositionMeters(137.5, -211.25);

	const FVector2d TransectStartMeters(-380.0, -120.0);
	const FVector2d TransectEndMeters(420.0, 260.0);

	void CheckReadingMatchesDirectSample(
		FAutomationTestBase& Test,
		const FMassifEnvironmentState& State,
		const FMassifProbeReading& Reading,
		FVector2d PositionMeters)
	{
		const FMassifChannelHandle Channel = State.FindChannel(Reading.Name);
		const FString ChannelName = Reading.Name.ToString();
		if (Reading.Type == EMassifChannelType::Vector2)
		{
			Test.TestTrue(FString::Printf(TEXT("%s matches a direct vector sample"), *ChannelName),
				Reading.VectorValue == State.SampleVector(Channel, PositionMeters)
					&& Reading.ScalarValue == Reading.VectorValue.Length());
			return;
		}
		if (Reading.Type == EMassifChannelType::Classified)
		{
			Test.TestEqual(FString::Printf(TEXT("%s matches a direct class sample"), *ChannelName),
				Reading.ClassValue, State.SampleClass(Channel, PositionMeters));
			return;
		}
		Test.TestTrue(FString::Printf(TEXT("%s matches a direct sample"), *ChannelName),
			Reading.ScalarValue == State.SampleScalar(Channel, PositionMeters));
	}

	void CheckTransectEndsMatchDirectSamples(
		FAutomationTestBase& Test, const FMassifEnvironmentState& State, const FMassifTransect& Line,
		int32 ChannelColumn)
	{
		if (Line.ChannelTypes[ChannelColumn] != EMassifChannelType::Scalar)
		{
			return;
		}

		const FMassifChannelHandle Channel = State.FindChannel(Line.ChannelNames[ChannelColumn]);
		Test.TestTrue(TEXT("The first sample is a direct sample at the start"),
			Line.Samples[0].Values[ChannelColumn] == State.SampleScalar(Channel, TransectStartMeters));
		Test.TestTrue(TEXT("The last sample is a direct sample at the end"),
			Line.Samples.Last().Values[ChannelColumn] == State.SampleScalar(Channel, TransectEndMeters));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentProbeReadingTest,
	"Massif.Presentation.EnvironmentProbe.Readings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentProbeReadingTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	const FMassifEnvironmentState& State = Scheduler->GetState();
	const FMassifProbeResult Probe = MassifEnvironmentProbe::Probe(State, ProbePositionMeters);

	TestEqual(TEXT("Every registered channel is read"), Probe.Readings.Num(), State.GetChannelCount());
	TestTrue(TEXT("The probe keeps the point it was taken at"), Probe.PositionMeters == ProbePositionMeters);
	TestFalse(TEXT("A bare state probe has no habitat"), Probe.Habitat.IsSet());

	for (const FMassifProbeReading& Reading : Probe.Readings)
	{
		const FMassifChannelHandle Channel = State.FindChannel(Reading.Name);
		TestTrue(FString::Printf(TEXT("%s reports the channel's type"), *Reading.Name.ToString()),
			Channel.IsValid() && Reading.Type == State.GetDescriptor(Channel).Type);
		CheckReadingMatchesDirectSample(*this, State, Reading, ProbePositionMeters);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentProbeClassifiedTest,
	"Massif.Presentation.EnvironmentProbe.Classified",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentProbeClassifiedTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	const FMassifEnvironmentState& State = Scheduler->GetState();
	const FMassifChannelHandle Patches = State.FindChannel(MassifTestEnvironment::PatchChannel());
	const FMassifChannelGrid& Grid = State.GetGrid(Patches);

	// Exactly on the edge between two cells holding different classes. A class
	// halfway between rock and soil is the scree that sits between them in the
	// enum, which is a reading nothing ever wrote.
	const FVector2d LeftCentre = Grid.CellCentreMeters(2, 3);
	const FVector2d RightCentre = Grid.CellCentreMeters(3, 3);
	const FVector2d EdgeMeters = (LeftCentre + RightCentre) * 0.5;

	const int32 LeftClass = State.SampleClass(Patches, LeftCentre);
	const int32 RightClass = State.SampleClass(Patches, RightCentre);
	TestTrue(TEXT("The two cells hold different classes"), LeftClass != RightClass);

	const FMassifProbeResult Probe = MassifEnvironmentProbe::Probe(State, EdgeMeters);
	const FMassifProbeReading* Reading = Probe.Readings.FindByPredicate([](const FMassifProbeReading& Candidate)
	{
		return Candidate.Name == MassifTestEnvironment::PatchChannel();
	});
	TestTrue(TEXT("The classified channel was probed"), Reading != nullptr);
	TestTrue(TEXT("The probe answers with one of the two classes"),
		Reading->ClassValue == LeftClass || Reading->ClassValue == RightClass);
	TestTrue(TEXT("The scalar reading of a class is that class, not a blend"),
		Reading->ScalarValue == static_cast<double>(Reading->ClassValue));

	const FMassifTransect Line = MassifEnvironmentProbe::Transect(State, LeftCentre, RightCentre, 9);
	const int32 PatchColumn = Line.ChannelNames.IndexOfByKey(MassifTestEnvironment::PatchChannel());
	for (const FMassifTransectSample& Sample : Line.Samples)
	{
		const double Value = Sample.Values[PatchColumn];
		TestTrue(TEXT("A transect of a classified channel never interpolates"),
			Value == FMath::RoundToDouble(Value));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentTransectTest,
	"Massif.Presentation.EnvironmentProbe.Transect",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentTransectTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	const FMassifEnvironmentState& State = Scheduler->GetState();

	const FMassifTransect Line =
		MassifEnvironmentProbe::Transect(State, TransectStartMeters, TransectEndMeters, 37);
	TestEqual(TEXT("The line is sampled as many times as was asked"), Line.Samples.Num(), 37);
	TestEqual(TEXT("Every registered channel is a column"), Line.ChannelNames.Num(), State.GetChannelCount());
	TestTrue(TEXT("It starts where it was told to"),
		Line.Samples[0].PositionMeters == TransectStartMeters);
	TestTrue(TEXT("It ends where it was told to"),
		Line.Samples.Last().PositionMeters == TransectEndMeters);
	TestEqual(TEXT("Distance runs from nothing to the length of the line"),
		Line.Samples.Last().DistanceMeters, (TransectEndMeters - TransectStartMeters).Length(), 1e-9);

	for (int32 ChannelColumn = 0; ChannelColumn < Line.ChannelNames.Num(); ++ChannelColumn)
	{
		CheckTransectEndsMatchDirectSamples(*this, State, Line, ChannelColumn);
	}

	for (const FMassifTransectSample& Sample : Line.Samples)
	{
		TestEqual(TEXT("Every row carries every channel"), Sample.Values.Num(), Line.ChannelNames.Num());
	}

	const FMassifTransect Default =
		MassifEnvironmentProbe::Transect(State, TransectStartMeters, TransectEndMeters);
	TestEqual(TEXT("The default is 64 samples"),
		Default.Samples.Num(), MassifEnvironmentProbe::DefaultTransectSampleCount);
	return true;
}
