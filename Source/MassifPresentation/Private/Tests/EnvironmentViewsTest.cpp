#include "Debug/MassifChannelStatistics.h"
#include "Debug/MassifDebugChannels.h"
#include "Debug/MassifDebugVectorOverlay.h"
#include "MassifTestEnvironment.h"
#include "Misc/AutomationTest.h"

namespace
{
	int32 TotalBucketCount(const FMassifChannelStatistics& Statistics)
	{
		int32 Total = 0;
		for (int32 Count : Statistics.BucketCounts)
		{
			Total += Count;
		}
		return Total;
	}

	/** Loose enough for a quantised channel's extremes to decode back through a lerp. */
	double RangeTolerance(double Value)
	{
		return FMath::Max(FMath::Abs(Value), 1.0) * 1e-9;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentViewRegistryTest,
	"Massif.Presentation.EnvironmentViews.Registry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentViewRegistryTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	const FMassifEnvironmentState& State = Scheduler->GetState();
	const TArray<FMassifDebugView> Views = MassifDebugChannels::BuildViews(State);

	TestEqual(TEXT("Every registered channel is a view, and the habitat ones follow"),
		Views.Num(), State.GetChannelCount() + static_cast<int32>(EMassifHabitatView::Count));

	for (FMassifChannelHandle Channel : State.GetAllChannels())
	{
		const FName ChannelName = State.GetDescriptor(Channel).Name;
		TestTrue(FString::Printf(TEXT("%s has a view"), *ChannelName.ToString()),
			MassifDebugChannels::FindViewIndex(Views, ChannelName) != INDEX_NONE);
	}

	// The two channels nothing in presentation names: they are selectable because
	// they were registered, not because anything here was taught about them.
	const int32 DriftIndex = MassifDebugChannels::FindViewIndex(Views, MassifTestEnvironment::DriftChannel());
	const int32 PatchIndex = MassifDebugChannels::FindViewIndex(Views, MassifTestEnvironment::PatchChannel());
	TestTrue(TEXT("A channel the code does not name gains a view"), DriftIndex != INDEX_NONE);
	TestTrue(TEXT("A classified channel the code does not name gains a view"), PatchIndex != INDEX_NONE);

	TestTrue(TEXT("The channel views come in name order"), Views[0].Name.Compare(Views[1].Name) < 0);
	TestTrue(TEXT("A vector channel is known to be a direction field"), Views[DriftIndex].IsVectorField());
	TestEqual(TEXT("A view carries the channel's own unit"), Views[DriftIndex].Unit, FString(TEXT("m/s")));
	TestTrue(TEXT("The habitat views are appended after the channels"),
		Views.Last().Source == EMassifDebugViewSource::Habitat);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentViewClassifiedTest,
	"Massif.Presentation.EnvironmentViews.Classified",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentViewClassifiedTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	const FMassifEnvironmentState& State = Scheduler->GetState();
	const TArray<FMassifDebugView> Views = MassifDebugChannels::BuildViews(State);

	const int32 PatchIndex = MassifDebugChannels::FindViewIndex(Views, MassifTestEnvironment::PatchChannel());
	const FMassifDebugView& PatchView = Views[PatchIndex];
	TestTrue(TEXT("A classified channel is drawn as classes"), PatchView.IsClassified());
	TestEqual(TEXT("It lists every class it holds"),
		PatchView.ClassLabels.Num(), MassifTestEnvironment::PatchClassCount);

	// A pixel halfway between two classes must be one of them, never the class that
	// happens to sit between their indices.
	TestTrue(TEXT("A classified view is never filtered"),
		MassifDebugChannels::GetTextureFilter(PatchView) == TF_Nearest);
	TestTrue(TEXT("A measurement still is"),
		MassifDebugChannels::GetTextureFilter(Views[0]) == TF_Bilinear);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentViewHistogramTest,
	"Massif.Presentation.EnvironmentViews.Histogram",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentViewHistogramTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	const FMassifEnvironmentState& State = Scheduler->GetState();

	for (FMassifChannelHandle Channel : State.GetAllChannels())
	{
		const FString ChannelName = State.GetDescriptor(Channel).Name.ToString();
		const FMassifChannelStatistics Statistics = MassifChannelStatistics::MeasureChannel(State, Channel);
		const int32 CellCount = State.GetGrid(Channel).CellCount();

		TestEqual(FString::Printf(TEXT("%s: every cell is measured"), *ChannelName),
			Statistics.SampleCount, CellCount);
		TestEqual(FString::Printf(TEXT("%s: the buckets account for every cell"), *ChannelName),
			TotalBucketCount(Statistics), CellCount);
		TestEqual(FString::Printf(TEXT("%s: the buckets are the coarse strip the panel draws"), *ChannelName),
			Statistics.BucketCounts.Num(), MassifHistogramBucketCount);

		const double Minimum = State.GetChannelMinimum(Channel);
		const double Maximum = State.GetChannelMaximum(Channel);
		TestEqual(FString::Printf(TEXT("%s: the low end is the channel's"), *ChannelName),
			Statistics.Minimum, Minimum, RangeTolerance(Minimum));
		TestEqual(FString::Printf(TEXT("%s: the high end is the channel's"), *ChannelName),
			Statistics.Maximum, Maximum, RangeTolerance(Maximum));
		TestTrue(FString::Printf(TEXT("%s: the mean lies inside the range"), *ChannelName),
			Statistics.Mean >= Minimum - RangeTolerance(Minimum)
				&& Statistics.Mean <= Maximum + RangeTolerance(Maximum));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentVectorOverlayTest,
	"Massif.Presentation.EnvironmentViews.VectorOverlay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentVectorOverlayTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	const FMassifEnvironmentState& State = Scheduler->GetState();
	const TArray<FMassifDebugView> Views = MassifDebugChannels::BuildViews(State);

	const FMassifChannelHandle Channel = MassifDebugVectorOverlay::ChooseChannel(Views, INDEX_NONE);
	TestTrue(TEXT("The overlay finds the registered vector channel with nothing selected"), Channel.IsValid());
	TestTrue(TEXT("... and it is the one the graph registered"),
		State.GetDescriptor(Channel).Name == MassifTestEnvironment::DriftChannel());

	const TArray<FMassifVectorArrow> Arrows =
		MassifDebugVectorOverlay::BuildArrows(State, Channel, FVector2d::ZeroVector, 200.0, 8);
	TestEqual(TEXT("One arrow per lattice point, not per cell"), Arrows.Num(), 64);

	for (const FMassifVectorArrow& Arrow : Arrows)
	{
		TestTrue(TEXT("Arrows are scaled against the strongest on the lattice"),
			Arrow.MagnitudeFraction >= 0.0 && Arrow.MagnitudeFraction <= 1.0);
	}
	return true;
}
