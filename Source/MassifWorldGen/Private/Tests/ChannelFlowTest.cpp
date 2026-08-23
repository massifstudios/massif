#include "MassifChannelFlow.h"

#include "MassifRiverNetwork.h"
#include "Misc/AutomationTest.h"
#include "Tests/MassifTestChannel.h"

using namespace MassifTestChannel;

namespace
{
	/** The first fit, then the water put back over it at the given level of the ladder. */
	TArray<FMassifRiverReach> SolveFlowOne(
		const IMassifElevationSource& Source, FMassifRiverReach&& Reach, EMassifFidelity Fidelity)
	{
		TArray<FMassifRiverReach> Reaches = SolveProfiles(Source, MoveTemp(Reach));

		FMassifChannelFlowParams Params;
		Params.Fidelity = Fidelity;
		Params.SlopeWindowNodes = 1;
		MassifSolveChannelFlow(Params, Reaches);
		return Reaches;
	}

	int32 CountStationsRisingDownstream(const FMassifRiverReach& Reach)
	{
		int32 Rising = 0;
		for (int32 NodeIndex = 1; NodeIndex < Reach.Nodes.Num(); ++NodeIndex)
		{
			const double DropMeters =
				Reach.Nodes[NodeIndex - 1].WaterSurfaceMeters - Reach.Nodes[NodeIndex].WaterSurfaceMeters;
			Rising += DropMeters < -1.0e-9;
		}
		return Rising;
	}

	/** Stations in a range whose surface stands strictly above the one below them. */
	int32 CountStationsFalling(const FMassifRiverReach& Reach, int32 FirstIndex, int32 LastIndex)
	{
		int32 Falling = 0;
		for (int32 NodeIndex = FirstIndex; NodeIndex < LastIndex; ++NodeIndex)
		{
			const double DropMeters =
				Reach.Nodes[NodeIndex].WaterSurfaceMeters - Reach.Nodes[NodeIndex + 1].WaterSurfaceMeters;
			Falling += DropMeters > 1.0e-6;
		}
		return Falling;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelFlowNormalDepthTest,
	"Massif.WorldGen.ChannelFlow.NormalDepth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelFlowNormalDepthTest::RunTest(const FString& Parameters)
{
	// A prismatic trapezoid at one gradient, handed exactly what Manning carries in
	// it at a known depth. The surface has to come back that far over the bed, and
	// it has to keep descending at the bed's own slope.
	constexpr double BaseHalfWidthMeters = 3.0;
	constexpr double SideSlope = 2.0;
	constexpr double BedSlope = 0.02;
	constexpr double DepthMeters = 1.2;

	const double DischargeCubicMetersPerSecond = TrapezoidDischarge(
		BaseHalfWidthMeters, SideSlope, DepthMeters, MakeProfileTestParams().Roughness, BedSlope);

	const FTrapezoidChannelSource Source(BaseHalfWidthMeters, SideSlope, BedSlope);
	const FReachSpec Spec{
		DepthMeters, DepthMeters, DischargeCubicMetersPerSecond, 2.0 * BaseHalfWidthMeters};
	const TArray<FMassifRiverReach> Solved =
		SolveFlowOne(Source, MakeStraightReach(Source, Spec), EMassifFidelity::Cheap);

	const FMassifRiverNode& Middle = Solved[0].Nodes[Solved[0].Nodes.Num() / 2];
	const double SolvedDepthMeters = Middle.WaterSurfaceMeters - Middle.GroundAltitudeMeters;

	AddInfo(FString::Printf(
		TEXT("Q %.2f m3/s, surface %.3f m over the bed against %.3f"), DischargeCubicMetersPerSecond,
		SolvedDepthMeters, DepthMeters));

	// A section sampled at thirty-three offsets lands the bank breaks between them,
	// so the closed form is approached and not hit.
	TestTrue(
		TEXT("The surface settles at the normal depth"),
		FMath::Abs(SolvedDepthMeters - DepthMeters) < 0.1 * DepthMeters);
	TestEqual(
		TEXT("The surface never rises going downstream"), CountStationsRisingDownstream(Solved[0]), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelFlowBackwaterTest,
	"Massif.WorldGen.ChannelFlow.Backwater",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelFlowBackwaterTest::RunTest(const FString& Parameters)
{
	// A wide gentle channel pinched to a third of its width halfway down. The pinch
	// cannot pass the water at the depth the reach above it would run at, so the
	// water stands higher upstream of it than that reach's own slope and section
	// would ever produce.
	//
	// The distinction between the two levels is what the ponded stretch looks like:
	// holding each station at least as high as the one below leaves the pool exactly
	// level, and carrying the surface up against friction gives it the slight fall a
	// real pool has.
	const FConstrictedChannelSource Source(9.0, 3.0, 20.0, 2.0, 0.001);
	const FReachSpec Spec{0.5, 1.0, 14.0, 18.0};

	const TArray<FMassifRiverReach> Cheap =
		SolveFlowOne(Source, MakeStraightReach(Source, Spec), EMassifFidelity::Cheap);
	const TArray<FMassifRiverReach> Standard =
		SolveFlowOne(Source, MakeStraightReach(Source, Spec), EMassifFidelity::Standard);

	// The pinch spans the middle five stations of a forty station reach at four
	// metre spacing; the ponded stretch is what lies above it.
	const int32 PinchIndex = Cheap[0].Nodes.Num() / 2 - 6;
	const int32 FirstPondedIndex = PinchIndex / 2;
	const int32 CheapFalling = CountStationsFalling(Cheap[0], FirstPondedIndex, PinchIndex);
	const int32 StandardFalling = CountStationsFalling(Standard[0], FirstPondedIndex, PinchIndex);

	AddInfo(FString::Printf(
		TEXT("of %d ponded spans, %d fall at Cheap and %d at Standard"), PinchIndex - FirstPondedIndex,
		CheapFalling, StandardFalling));

	TestTrue(TEXT("The pool at Cheap is dead level"), CheapFalling == 0);
	TestTrue(TEXT("The pool at Standard falls towards its outlet"), StandardFalling > 0);
	TestEqual(
		TEXT("The surface never rises going downstream"), CountStationsRisingDownstream(Standard[0]), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelFlowOffTest,
	"Massif.WorldGen.ChannelFlow.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelFlowOffTest::RunTest(const FString& Parameters)
{
	// Off keeps the surface the elevation model measured, which along a channel is
	// the top of the water and so is already an answer.
	const FTrapezoidChannelSource Source(4.0, 2.0, 0.02);
	const FReachSpec Spec{0.8, 0.8, 9.0, 8.0};

	const TArray<FMassifRiverReach> Before = SolveProfiles(Source, MakeStraightReach(Source, Spec));
	const TArray<FMassifRiverReach> After =
		SolveFlowOne(Source, MakeStraightReach(Source, Spec), EMassifFidelity::Off);

	int32 SurfacesMoved = 0;
	for (int32 NodeIndex = 0; NodeIndex < Before[0].Nodes.Num(); ++NodeIndex)
	{
		SurfacesMoved += Before[0].Nodes[NodeIndex].WaterSurfaceMeters
			!= After[0].Nodes[NodeIndex].WaterSurfaceMeters;
	}

	TestEqual(TEXT("Off leaves the measured surface exactly where it was"), SurfacesMoved, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelFlowDeterminismTest,
	"Massif.WorldGen.ChannelFlow.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelFlowDeterminismTest::RunTest(const FString& Parameters)
{
	const FSlopeBreakChannelSource Source(4.0, 2.0, 0.05, 0.0004);
	const FReachSpec Spec{0.6, 1.0, 14.0, 8.0};

	const TArray<FMassifRiverReach> First =
		SolveFlowOne(Source, MakeStraightReach(Source, Spec), EMassifFidelity::Standard);
	const TArray<FMassifRiverReach> Second =
		SolveFlowOne(Source, MakeStraightReach(Source, Spec), EMassifFidelity::Standard);

	int32 StationsDisagreeing = 0;
	for (int32 NodeIndex = 0; NodeIndex < First[0].Nodes.Num(); ++NodeIndex)
	{
		StationsDisagreeing += First[0].Nodes[NodeIndex].WaterSurfaceMeters
			!= Second[0].Nodes[NodeIndex].WaterSurfaceMeters;
	}

	// Bit-identical: a fixed pass and step count over a pure function of the
	// sections, so anything else is a dependence on how the work was split up.
	TestEqual(TEXT("The same reach settles at the same surface"), StationsDisagreeing, 0);
	return true;
}
