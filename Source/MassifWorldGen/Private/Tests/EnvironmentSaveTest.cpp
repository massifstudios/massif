#include "MassifEnvironmentSaveSection.h"
#include "MassifEnvironmentState.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
	constexpr double BakeTestWorldSideMeters = 1024.0;

	FVector2d BakeTestWorldSize()
	{
		return FVector2d(BakeTestWorldSideMeters, BakeTestWorldSideMeters);
	}

	FMassifChannelDescriptor MakeChannel(
		const TCHAR* Name, EMassifChannelEncoding Encoding, double CellSizeMeters, int32 TimeSliceCount = 1)
	{
		FMassifChannelDescriptor Descriptor;
		Descriptor.Name = FName(Name);
		Descriptor.Encoding = Encoding;
		Descriptor.CellSizeMeters = CellSizeMeters;
		Descriptor.TimeSliceCount = TimeSliceCount;

		// Text the process owns rather than the file: what a bake must get back from
		// the graph it is read into, and never from disk.
		Descriptor.Unit = TEXT("m");
		Descriptor.Description = TEXT("A test channel");
		Descriptor.ColorMap = EMassifChannelColorMap::Magma;
		return Descriptor;
	}

	FMassifPointSetDescriptor MakePointSet()
	{
		FMassifPointSetDescriptor Descriptor;
		Descriptor.Name = FName(TEXT("Test.Blocks"));
		Descriptor.TileSizeMeters = 512.0;
		Descriptor.RadiusRangeMeters = FVector2d(0.25, 8.0);
		Descriptor.ClassNames = { TEXT("Small"), TEXT("Large") };
		return Descriptor;
	}

	/** The three encodings a bake has to survive: quantised, float and raw doubles. */
	void RegisterTestChannels(FMassifEnvironmentState& State)
	{
		State.RegisterChannel(MakeChannel(TEXT("Test.Wetness"), EMassifChannelEncoding::Uint16, 8.0));
		State.RegisterChannel(MakeChannel(TEXT("Test.Altitude"), EMassifChannelEncoding::Float64, 8.0));
		State.RegisterChannel(MakeChannel(TEXT("Test.Snow"), EMassifChannelEncoding::Uint16, 64.0, 24));
	}

	void CommitSlice(
		FMassifEnvironmentState& State, FMassifChannelHandle Channel, int32 TimeSlice, double Bias)
	{
		FMassifChannelWriteScope Scope(State, Channel, TimeSlice);
		const FMassifChannelGrid& Grid = Scope.GetGrid();
		TArrayView<double> Values = Scope.GetComponent(0);
		for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
		{
			Values[CellIndex] = Bias + 0.5 * (CellIndex % Grid.Width) - 0.25 * (CellIndex / Grid.Width);
		}
		Scope.Commit();
	}

	void SolveChannel(FMassifEnvironmentState& State, const TCHAR* Name, double Bias)
	{
		const FMassifChannelHandle Channel = State.FindChannel(FName(Name));
		const int32 SliceCount = State.GetDescriptor(Channel).TimeSliceCount;
		for (int32 TimeSlice = 0; TimeSlice < SliceCount; ++TimeSlice)
		{
			CommitSlice(State, Channel, TimeSlice, Bias + TimeSlice);
		}
	}

	void SolvePoints(FMassifEnvironmentState& State)
	{
		const FMassifPointSetHandle PointSet = State.FindPointSet(FName(TEXT("Test.Blocks")));

		FMassifPointSetWriteScope Scope(State, PointSet);
		TArrayView<FMassifEnvironmentPoint> Slots = Scope.Reserve(64);
		for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
		{
			Slots[SlotIndex].PositionMeters = FVector3d(SlotIndex * 4.0 - 400.0, SlotIndex * 2.0 - 300.0, 1500.0);
			Slots[SlotIndex].RadiusMeters = 0.5 + SlotIndex * 0.05;
			Slots[SlotIndex].RotationDegrees = FVector3d(SlotIndex, 0.0, 0.0);
			Slots[SlotIndex].ClassIndex = SlotIndex % 2;
		}
		Scope.Commit();
	}

	/** A world as a graph leaves it: every channel registered, every one written. */
	TSharedRef<FMassifEnvironmentState> MakeSolvedState()
	{
		const TSharedRef<FMassifEnvironmentState> State =
			MakeShared<FMassifEnvironmentState>(BakeTestWorldSize());
		RegisterTestChannels(*State);
		State->RegisterPointSet(MakePointSet());

		SolveChannel(*State, TEXT("Test.Wetness"), 10.0);
		SolveChannel(*State, TEXT("Test.Altitude"), 2000.0);
		SolveChannel(*State, TEXT("Test.Snow"), 1.0);
		SolvePoints(*State);
		return State;
	}

	/** The same graph before it has run: what a load reads a bake back into. */
	TSharedRef<FMassifEnvironmentState> MakeScheduledState()
	{
		const TSharedRef<FMassifEnvironmentState> State =
			MakeShared<FMassifEnvironmentState>(BakeTestWorldSize());
		RegisterTestChannels(*State);
		State->RegisterPointSet(MakePointSet());
		return State;
	}

	TArray<uint8> WriteBake(FMassifEnvironmentState& State)
	{
		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes);
		State.Serialize(Writer);
		return Bytes;
	}

	TSharedRef<FMassifEnvironmentState> ReadBake(const TArray<uint8>& Bytes)
	{
		FMemoryReader Reader(Bytes);
		const TSharedRef<FMassifEnvironmentState> Carrier =
			MakeShared<FMassifEnvironmentState>(FVector2d::ZeroVector);
		Carrier->Serialize(Reader);
		return Carrier;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeRoundTripTest,
	"Massif.WorldGen.EnvironmentBake.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeRoundTripTest::RunTest(const FString& Parameters)
{
	// The whole point of the bake: a world read back off disk is the world that was
	// written, not one close to it. Everything downstream — the rivers, the carve,
	// the habitat the trees stood in — is derived from these channels, so a bake
	// that is nearly right is a world that is quietly different.
	const TSharedRef<FMassifEnvironmentState> Solved = MakeSolvedState();
	const TArray<uint8> Bytes = WriteBake(Solved.Get());

	const TSharedRef<FMassifEnvironmentState> Restored = MakeScheduledState();
	FString RestoreError;
	if (!TestTrue(TEXT("A bake reads back into the graph that wrote it"),
		Restored->TakeSolvedPayloadsFrom(ReadBake(Bytes).Get(), RestoreError)))
	{
		AddError(RestoreError);
		return false;
	}

	TestEqual(TEXT("The restored world hashes the same"),
		Restored->ComputeContentHash(), Solved->ComputeContentHash());
	TestEqual(TEXT("It holds the same bytes"),
		Restored->GetResidentByteCount(), Solved->GetResidentByteCount());

	const FMassifChannelHandle Snow = Restored->FindChannel(FName(TEXT("Test.Snow")));
	TestEqual(TEXT("A seasonal channel keeps every slice"),
		Restored->GetDescriptor(Snow).TimeSliceCount, 24);
	TestEqual(TEXT("Its last slice reads what was solved"),
		Restored->SampleScalar(Snow, FVector2d(100.0, -60.0), 23),
		Solved->SampleScalar(Snow, FVector2d(100.0, -60.0), 23));

	// Unit, description and colour map are pointers to text a process owns. They
	// cannot come off disk, so a restored channel has to have kept the graph's.
	TestEqual(TEXT("The channel keeps the graph's unit"),
		FString(Restored->GetDescriptor(Snow).Unit), FString(TEXT("m")));
	TestTrue(TEXT("The channel keeps the graph's colour map"),
		Restored->GetDescriptor(Snow).ColorMap == EMassifChannelColorMap::Magma);

	const FMassifPointSetHandle Blocks = Restored->FindPointSet(FName(TEXT("Test.Blocks")));
	TestEqual(TEXT("The point set keeps its objects"), Restored->GetPointCount(Blocks), 64);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeRefusalTest,
	"Massif.WorldGen.EnvironmentBake.RefusesADifferentGraph",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeRefusalTest::RunTest(const FString& Parameters)
{
	// A bake is a cache of a pure function, so the cost of refusing one is the
	// solve and nothing else — and the cost of accepting a wrong one is a world
	// half solved by a graph that has since changed.
	const TSharedRef<FMassifEnvironmentState> Solved = MakeSolvedState();
	const TArray<uint8> Bytes = WriteBake(Solved.Get());

	const TSharedRef<FMassifEnvironmentState> Gained =
		MakeShared<FMassifEnvironmentState>(BakeTestWorldSize());
	RegisterTestChannels(Gained.Get());
	Gained->RegisterPointSet(MakePointSet());
	Gained->RegisterChannel(MakeChannel(TEXT("Test.Avalanche"), EMassifChannelEncoding::Uint16, 64.0));

	FString GainedError;
	TestFalse(TEXT("A graph that has gained a channel refuses the bake"),
		Gained->TakeSolvedPayloadsFrom(ReadBake(Bytes).Get(), GainedError));
	TestTrue(TEXT("It says what did not line up"), GainedError.Contains(TEXT("channels")));

	const TSharedRef<FMassifEnvironmentState> Resized =
		MakeShared<FMassifEnvironmentState>(BakeTestWorldSize());
	Resized->RegisterChannel(MakeChannel(TEXT("Test.Wetness"), EMassifChannelEncoding::Uint16, 16.0));
	Resized->RegisterChannel(MakeChannel(TEXT("Test.Altitude"), EMassifChannelEncoding::Float64, 8.0));
	Resized->RegisterChannel(MakeChannel(TEXT("Test.Snow"), EMassifChannelEncoding::Uint16, 64.0, 24));
	Resized->RegisterPointSet(MakePointSet());

	FString ResizedError;
	TestFalse(TEXT("A channel solved at another cell size refuses the bake"),
		Resized->TakeSolvedPayloadsFrom(ReadBake(Bytes).Get(), ResizedError));
	TestTrue(TEXT("It names the channel that disagreed"),
		ResizedError.Contains(TEXT("Test.Wetness")));

	const TSharedRef<FMassifEnvironmentState> Narrower =
		MakeShared<FMassifEnvironmentState>(FVector2d(512.0, 512.0));
	RegisterTestChannels(Narrower.Get());
	Narrower->RegisterPointSet(MakePointSet());

	FString NarrowerError;
	TestFalse(TEXT("A smaller world refuses the bake"),
		Narrower->TakeSolvedPayloadsFrom(ReadBake(Bytes).Get(), NarrowerError));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentBakeSectionTest,
	"Massif.WorldGen.EnvironmentBake.Section",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentBakeSectionTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMassifEnvironmentState> Solved = MakeSolvedState();

	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	FMassifEnvironmentSaveSection WriteSection(Solved);
	FString WriteError;
	TestTrue(TEXT("The section writes the state it was given"), WriteSection.Serialize(Writer, WriteError));

	FMemoryReader Reader(Bytes);
	FMassifEnvironmentSaveSection ReadSection;
	FString ReadError;
	TestTrue(TEXT("The section reads a state of its own"), ReadSection.Serialize(Reader, ReadError));

	const TSharedPtr<FMassifEnvironmentState> Carrier = ReadSection.GetState();
	if (!TestTrue(TEXT("It produced a carrier"), Carrier.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("The carrier holds every channel"),
		Carrier->GetChannelCount(), Solved->GetChannelCount());

	FMassifEnvironmentSaveSection Empty;
	FMemoryWriter EmptyWriter(Bytes);
	FString EmptyError;
	TestFalse(TEXT("A section with nothing to write says so"), Empty.Serialize(EmptyWriter, EmptyError));
	return true;
}
