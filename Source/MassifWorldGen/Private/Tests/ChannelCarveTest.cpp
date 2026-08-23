#include "MassifChannelCarve.h"

#include "MassifRiverNetwork.h"
#include "Misc/AutomationTest.h"
#include "Tests/MassifTestChannel.h"

using namespace MassifTestChannel;

namespace
{
	FMassifChannelCarveParams MakeCarveTestParams()
	{
		FMassifChannelCarveParams Params;
		// Defaults throughout: the carve is what is under test, not a tuning of it.
		return Params;
	}

	/** The first fit, then the carve over it, so what changed is the carve's doing alone. */
	TArray<FMassifRiverReach> CarveOne(
		const IMassifElevationSource& Source, FMassifRiverReach&& Reach, double RockHardness = 0.0,
		EMassifFidelity Fidelity = EMassifFidelity::Standard)
	{
		TArray<FMassifRiverReach> Reaches = SolveProfiles(Source, MoveTemp(Reach));

		FMassifChannelCarveParams Params = MakeCarveTestParams();
		Params.Fidelity = Fidelity;
		const FMassifUniformBedMaterial BedMaterial(RockHardness);
		MassifCarveChannels(BedMaterial, Params, Reaches);
		return Reaches;
	}

	/** Mean thalweg altitude over the stations in a range, which aggradation lifts and incision drops. */
	double MeanThalwegMeters(const FMassifRiverReach& Reach, int32 FirstIndex, int32 LastIndex)
	{
		double TotalMeters = 0.0;
		for (int32 NodeIndex = FirstIndex; NodeIndex <= LastIndex; ++NodeIndex)
		{
			TotalMeters += ThalwegAltitudeMeters(Reach.Nodes[NodeIndex].Profile);
		}
		return TotalMeters / (LastIndex - FirstIndex + 1);
	}

	/** A reach crossing a slope break at its middle station, with the water already in its channel. */
	FReachSpec MakeSlopeBreakSpec()
	{
		return FReachSpec{0.6, 1.0, 14.0, 8.0};
	}

	const FMassifRiverNode& MiddleNode(const FMassifRiverReach& Reach)
	{
		return Reach.Nodes[Reach.Nodes.Num() / 2];
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelCarveDepositionTest,
	"Massif.WorldGen.ChannelCarve.Deposition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelCarveDepositionTest::RunTest(const FString& Parameters)
{
	// Steep above, gentle below. What the steep half hands on is more than the
	// gentle half can carry, so the difference has to come out of the water and
	// onto the bed: the reach aggrades below the break and is left alone above it.
	const FSlopeBreakChannelSource Source(4.0, 2.0, 0.05, 0.004);
	const FReachSpec Spec = MakeSlopeBreakSpec();

	const TArray<FMassifRiverReach> Before = SolveProfiles(Source, MakeStraightReach(Source, Spec));
	const TArray<FMassifRiverReach> After = CarveOne(Source, MakeStraightReach(Source, Spec));

	// The wedge lands where the break is, not at the far end: a reach fed only from
	// its own head starves everything below whatever settles out first, and how far
	// that reaches is a question about supply between reaches rather than about
	// whether the break aggrades.
	const int32 BreakIndex = Before[0].Nodes.Num() / 2;
	const int32 LastIndex = Before[0].Nodes.Num() - 1;
	const int32 WedgeEndIndex = BreakIndex + (LastIndex - BreakIndex) / 4;
	const double AboveBeforeMeters = MeanThalwegMeters(Before[0], 1, BreakIndex - 1);
	const double AboveAfterMeters = MeanThalwegMeters(After[0], 1, BreakIndex - 1);
	const double BelowBeforeMeters = MeanThalwegMeters(Before[0], BreakIndex + 1, WedgeEndIndex);
	const double BelowAfterMeters = MeanThalwegMeters(After[0], BreakIndex + 1, WedgeEndIndex);

	AddInfo(FString::Printf(
		TEXT("above the break %+.3f m, below it %+.3f m"), AboveAfterMeters - AboveBeforeMeters,
		BelowAfterMeters - BelowBeforeMeters));
	TestTrue(
		TEXT("The gentle reach just below the break takes up what the steep one hands it"),
		BelowAfterMeters > BelowBeforeMeters + 0.05);
	TestTrue(
		TEXT("The steep reach above it stays where it was"),
		FMath::Abs(AboveAfterMeters - AboveBeforeMeters) < 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelCarveIncisionTest,
	"Massif.WorldGen.ChannelCarve.Incision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelCarveIncisionTest::RunTest(const FString& Parameters)
{
	// The break the other way up: the lower half can carry more than arrives, and
	// takes the difference out of its own bed. Rock is what decides how fast.
	const FSlopeBreakChannelSource Source(4.0, 2.0, 0.004, 0.05);
	const FReachSpec Spec = MakeSlopeBreakSpec();

	const TArray<FMassifRiverReach> Before = SolveProfiles(Source, MakeStraightReach(Source, Spec));
	const TArray<FMassifRiverReach> Loose = CarveOne(Source, MakeStraightReach(Source, Spec), 0.0);
	const TArray<FMassifRiverReach> Solid = CarveOne(Source, MakeStraightReach(Source, Spec), 1.0);

	const int32 BreakIndex = Before[0].Nodes.Num() / 2;
	const int32 LastIndex = Before[0].Nodes.Num() - 1;
	const double BeforeMeters = MeanThalwegMeters(Before[0], BreakIndex + 1, LastIndex);
	const double LooseMeters = MeanThalwegMeters(Loose[0], BreakIndex + 1, LastIndex);
	const double SolidMeters = MeanThalwegMeters(Solid[0], BreakIndex + 1, LastIndex);

	AddInfo(FString::Printf(
		TEXT("loose bed %+.3f m, solid rock %+.3f m"), LooseMeters - BeforeMeters,
		SolidMeters - BeforeMeters));

	TestTrue(TEXT("The steep reach below the break cuts into its own bed"), LooseMeters < BeforeMeters - 0.05);
	TestTrue(TEXT("Rock is cut into more slowly than valley fill"), SolidMeters > LooseMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelCarveBendTest,
	"Massif.WorldGen.ChannelCarve.Bend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelCarveBendTest::RunTest(const FString& Parameters)
{
	// Dead level ground, so the section at every station starts flat across and any
	// lean it comes out with is the bend's doing. A left turn curves towards
	// positive X, which puts its outer bank on the right, and the bed has to fall
	// that way.
	const FLevelGroundSource Source;
	const FReachSpec Spec{0.5, 1.0, 14.0, 8.0, 0.01};

	// Short, so what the bend does to the section is not buried under what a reach
	// fed only at its own head does to everything far below that head.
	constexpr int32 StationCount = 13;
	const TArray<FMassifRiverReach> Straight =
		CarveOne(Source, MakeStraightReach(Source, Spec, StationCount));
	const TArray<FMassifRiverReach> Left =
		CarveOne(Source, MakeArcReach(Source, Spec, 60.0, true, StationCount));
	const TArray<FMassifRiverReach> Right =
		CarveOne(Source, MakeArcReach(Source, Spec, 60.0, false, StationCount));

	const double StraightMeters = ThalwegOffsetMeters(MiddleNode(Straight[0]).Profile);
	const double LeftMeters = ThalwegOffsetMeters(MiddleNode(Left[0]).Profile);
	const double RightMeters = ThalwegOffsetMeters(MiddleNode(Right[0]).Profile);

	AddInfo(FString::Printf(
		TEXT("deep point: straight %+.2f m, turning left %+.2f m, turning right %+.2f m"), StraightMeters,
		LeftMeters, RightMeters));
	// A left turn curves towards positive X, so its outer bank is the right one and
	// the water is thrown that way.
	TestTrue(TEXT("A straight reach keeps its channel on the line it was traced along"),
		FMath::IsNearlyZero(StraightMeters));
	TestTrue(TEXT("Turning left moves the deep point towards the right bank"), LeftMeters > 0.5);
	TestTrue(TEXT("Turning right moves the deep point towards the left bank"), RightMeters < -0.5);

	// Mirrored geometry has to give a mirrored channel. The bend is the one place
	// the solve tells its two banks apart, so a difference between the two turns
	// beyond a change of sign is the solve preferring a bank.
	//
	// To one offset, not exactly: a section spreading evenly to both sides has to
	// take one of them first, and whichever rule breaks that tie names a bank.
	TestTrue(
		TEXT("The two turns are mirror images of one another"),
		FMath::Abs(LeftMeters + RightMeters) <= MiddleNode(Left[0]).Profile.OffsetSpacingMeters());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelCarveOffTest,
	"Massif.WorldGen.ChannelCarve.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelCarveOffTest::RunTest(const FString& Parameters)
{
	// Off is a level, not an absence: the trench the first fit cut is a channel on
	// any surface, and it has to survive untouched so the fidelity switch is a real
	// before and after.
	const FSlopeBreakChannelSource Source(4.0, 2.0, 0.05, 0.004);
	const FReachSpec Spec = MakeSlopeBreakSpec();

	const TArray<FMassifRiverReach> Before = SolveProfiles(Source, MakeStraightReach(Source, Spec));
	const TArray<FMassifRiverReach> After =
		CarveOne(Source, MakeStraightReach(Source, Spec), 0.0, EMassifFidelity::Off);

	int32 OffsetsMoved = 0;
	for (int32 NodeIndex = 0; NodeIndex < Before[0].Nodes.Num(); ++NodeIndex)
	{
		const FMassifChannelProfile& Left = Before[0].Nodes[NodeIndex].Profile;
		const FMassifChannelProfile& Right = After[0].Nodes[NodeIndex].Profile;
		for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			OffsetsMoved += Left.BedAltitudeMeters[OffsetIndex] != Right.BedAltitudeMeters[OffsetIndex];
		}
	}

	TestEqual(TEXT("Off leaves the first fit's trench exactly as it was"), OffsetsMoved, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelCarveDeterminismTest,
	"Massif.WorldGen.ChannelCarve.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelCarveDeterminismTest::RunTest(const FString& Parameters)
{
	const FSlopeBreakChannelSource Source(4.0, 2.0, 0.004, 0.05);
	const FReachSpec Spec = MakeSlopeBreakSpec();

	const TArray<FMassifRiverReach> First = CarveOne(Source, MakeStraightReach(Source, Spec));
	const TArray<FMassifRiverReach> Second = CarveOne(Source, MakeStraightReach(Source, Spec));

	int32 OffsetsDisagreeing = 0;
	for (int32 NodeIndex = 0; NodeIndex < First[0].Nodes.Num(); ++NodeIndex)
	{
		const FMassifChannelProfile& Left = First[0].Nodes[NodeIndex].Profile;
		const FMassifChannelProfile& Right = Second[0].Nodes[NodeIndex].Profile;
		for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			OffsetsDisagreeing += Left.BedAltitudeMeters[OffsetIndex] != Right.BedAltitudeMeters[OffsetIndex];
		}
	}

	// Bit-identical: a fixed sweep count over a pure function of the section, so
	// anything else is a dependence on how the work was split across threads.
	TestEqual(TEXT("The same reach carves to the same bed"), OffsetsDisagreeing, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelCarveGradedTest,
	"Massif.WorldGen.ChannelCarve.Graded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelCarveGradedTest::RunTest(const FString& Parameters)
{
	// One slope, one section, the same water at every station: nothing along this
	// reach distinguishes one station from the next, so whatever the carve settles
	// on it has to settle on the same thing everywhere and leave the bed straight.
	const FTrapezoidChannelSource Source(4.0, 2.0, 0.03);
	const TArray<FMassifRiverReach> After = CarveOne(Source, MakeStraightReach(Source, FReachSpec{0.9, 0.9, 16.0, 8.0}));

	// Measured as curvature rather than as a drop, so the reach's own gradient is
	// not counted: a straight bed on any slope has none. What this catches is the
	// one thing a gradient cannot explain — a station standing clear of the two
	// beside it, which is a pit or a bar a station wide.
	double WorstMeters = 0.0;
	int32 WorstIndex = INDEX_NONE;
	for (int32 NodeIndex = 1; NodeIndex + 1 < After[0].Nodes.Num(); ++NodeIndex)
	{
		const double MeanOfNeighboursMeters = 0.5
			* (ThalwegAltitudeMeters(After[0].Nodes[NodeIndex - 1].Profile)
				+ ThalwegAltitudeMeters(After[0].Nodes[NodeIndex + 1].Profile));
		const double DepartureMeters = FMath::Abs(
			ThalwegAltitudeMeters(After[0].Nodes[NodeIndex].Profile) - MeanOfNeighboursMeters);
		if (DepartureMeters > WorstMeters)
		{
			WorstMeters = DepartureMeters;
			WorstIndex = NodeIndex;
		}
	}

	AddInfo(FString::Printf(TEXT("worst station %d stands %.4f m off its neighbours"), WorstIndex, WorstMeters));
	TestTrue(
		FString::Printf(TEXT("No station stands %.3f m clear of the two beside it"), WorstMeters),
		WorstMeters < 0.05);
	return true;
}
