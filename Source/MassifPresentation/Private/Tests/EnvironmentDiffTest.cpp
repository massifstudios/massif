#include "Debug/MassifEnvironmentDiff.h"
#include "MassifSunProcesses.h"
#include "MassifTestEnvironment.h"
#include "Misc/AutomationTest.h"

namespace
{
	int32 CountChangedChannels(const FMassifFidelityDifference& Difference)
	{
		int32 ChangedCount = 0;
		for (const FMassifChannelDifference& Channel : Difference.Channels)
		{
			ChangedCount += static_cast<int32>(Channel.LargestAbsoluteDifference > 0.0);
		}
		return ChangedCount;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentDiffIdenticalTest,
	"Massif.Presentation.EnvironmentDiff.Identical",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentDiffIdenticalTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();

	// One level against itself has to come out at exactly zero, or nothing the
	// comparison says about two different levels means anything.
	const FMassifFidelityDifference Difference = MassifEnvironmentDiff::Compare(
		*Scheduler, MassifEnvironmentProcesses::Insolation(),
		EMassifFidelity::Standard, EMassifFidelity::Standard);

	TestTrue(TEXT("A level the process implements is comparable"), Difference.bComparable);
	TestEqual(TEXT("Every channel is reported"),
		Difference.Channels.Num(), Scheduler->GetState().GetChannelCount());
	TestTrue(TEXT("Nothing moved"), Difference.IsIdentical());

	for (const FMassifChannelDifference& Channel : Difference.Channels)
	{
		TestTrue(FString::Printf(TEXT("%s: nothing moved anywhere"), *Channel.Name.ToString()),
			Channel.MeanAbsoluteDifference == 0.0 && Channel.LargestAbsoluteDifference == 0.0
				&& Channel.ChangedCellFraction == 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentDiffFidelityTest,
	"Massif.Presentation.EnvironmentDiff.Fidelity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentDiffFidelityTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	const uint64 StartingHash = Scheduler->GetState().ComputeContentHash();

	// The cheap sun skips the horizon sweep, so the two levels genuinely differ:
	// the sweep only ever removes light, and this is what says by how much.
	const FMassifFidelityDifference Difference = MassifEnvironmentDiff::Compare(
		*Scheduler, MassifEnvironmentProcesses::Insolation(),
		EMassifFidelity::Cheap, EMassifFidelity::Standard);

	TestTrue(TEXT("Both levels are implemented"), Difference.bComparable);
	TestFalse(TEXT("The expensive model does something the cheap one did not"), Difference.IsIdentical());
	TestTrue(TEXT("The sun channels moved"), CountChangedChannels(Difference) > 0);

	for (const FMassifChannelDifference& Channel : Difference.Channels)
	{
		TestTrue(FString::Printf(TEXT("%s: a changed fraction is a fraction"), *Channel.Name.ToString()),
			Channel.ChangedCellFraction >= 0.0 && Channel.ChangedCellFraction <= 1.0);
	}

	// A comparison must not be a way to change the world by accident.
	TestTrue(TEXT("The process is left at the level it started at"),
		Scheduler->GetFidelity(MassifEnvironmentProcesses::Insolation()) == EMassifFidelity::Standard);
	TestTrue(TEXT("And the world it started with is back"),
		Scheduler->GetState().ComputeContentHash() == StartingHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEnvironmentDiffUnsupportedTest,
	"Massif.Presentation.EnvironmentDiff.Unsupported",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FEnvironmentDiffUnsupportedTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MassifTestEnvironment::MakeScheduler();
	const uint64 StartingHash = Scheduler->GetState().ComputeContentHash();

	const FMassifFidelityDifference Difference = MassifEnvironmentDiff::Compare(
		*Scheduler, MassifTestEnvironment::ExtraProcess(),
		EMassifFidelity::Standard, EMassifFidelity::Reference);

	TestFalse(TEXT("A level the process does not implement is refused"), Difference.bComparable);
	TestTrue(TEXT("Nothing is reported for a comparison that could not be made"),
		Difference.Channels.IsEmpty());
	TestTrue(TEXT("The world is untouched"), Scheduler->GetState().ComputeContentHash() == StartingHash);
	return true;
}
