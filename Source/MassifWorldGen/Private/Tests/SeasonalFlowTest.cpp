#include "MassifChannelFlow.h"
#include "MassifRiverField.h"
#include "MassifRiverNetwork.h"
#include "MassifChannelProfile.h"
#include "MassifTestChannel.h"
#include "Misc/AutomationTest.h"

/**
 * A river carries different water in June than in February, and everything
 * visible follows from how much of its own section that water fills.
 *
 * The bed is not part of it. A channel's shape is the work of every year it has
 * seen, so a season that moved the bed would be a season that re-cut the valley
 * twice a year.
 */
namespace
{
	using MassifTestChannel::FReachSpec;
	using MassifTestChannel::MakeStraightReach;

	constexpr int32 MeltSlice = 10;
	constexpr int32 FrozenSlice = 2;

	/** An alpine year: most of the water leaves the pack in one half-month of the spring. */
	double AlpineYearScale(int32 TimeSlice)
	{
		if (TimeSlice == MeltSlice)
		{
			return 6.0;
		}
		if (TimeSlice == FrozenSlice)
		{
			return 0.2;
		}
		return 1.0;
	}

	TArray<FMassifRiverReach> MakeSeasonalReaches()
	{
		const MassifTestChannel::FTrapezoidChannelSource Valley(4.0, 3.0, 0.02);

		FReachSpec Spec;
		Spec.WidthMeters = 12.0;
		Spec.HydraulicDepthMeters = 0.9;
		Spec.DischargeCubicMetersPerSecond = 14.0;
		Spec.SurfaceAboveGroundMeters = 0.0;

		TArray<FMassifRiverReach> Reaches;
		Reaches.Add(MakeStraightReach(Valley, Spec));
		MassifSolveChannelProfiles(Valley, MassifTestChannel::MakeProfileTestParams(), Reaches);
		return Reaches;
	}

	const FMassifRiverNode& MiddleNode(const TArray<FMassifRiverReach>& Reaches)
	{
		return Reaches[0].Nodes[Reaches[0].Nodes.Num() / 2];
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSeasonalFlowRisesAndFallsTest,
	"Massif.WorldGen.SeasonalFlow.RisesAndFalls",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSeasonalFlowRisesAndFallsTest::RunTest(const FString& Parameters)
{
	TArray<FMassifRiverReach> Reaches = MakeSeasonalReaches();
	FMassifChannelFlowParams Params;
	MassifSolveChannelFlow(Params, Reaches);

	const double AnnualSurfaceMeters = MiddleNode(Reaches).WaterSurfaceMeters;
	const double AnnualWidthMeters = MiddleNode(Reaches).WidthMeters;

	MassifSolveSeasonalFlow(
		Params, [](FVector2d, int32 TimeSlice) { return AlpineYearScale(TimeSlice); }, Reaches);

	const FMassifStationSeason& Season = MiddleNode(Reaches).Season;
	TestTrue(TEXT("The melt stands higher than the annual mean"),
		Season.WaterSurfaceMeters[MeltSlice] > AnnualSurfaceMeters + 0.05);
	TestTrue(TEXT("The frozen half-month stands lower"),
		Season.WaterSurfaceMeters[FrozenSlice] < AnnualSurfaceMeters - 0.02);

	// The water withdraws across the section as well as down it. A surface that
	// dropped while the wetted edge stayed put would be drawn below its own bank.
	TestTrue(TEXT("A river in flood is wider than one in retreat"),
		Season.HalfWidthMeters[MeltSlice] > Season.HalfWidthMeters[FrozenSlice]);
	TestTrue(TEXT("The melt runs faster than the freeze"),
		Season.SpeedMetersPerSecond[MeltSlice] > Season.SpeedMetersPerSecond[FrozenSlice]);

	// The slices are variations on the annual answer, which has to be what the
	// reaches are left standing at.
	TestEqual(TEXT("The annual surface survives the year being solved over it"),
		MiddleNode(Reaches).WaterSurfaceMeters, AnnualSurfaceMeters, 1.0e-9);
	TestEqual(TEXT("So does the annual width"),
		MiddleNode(Reaches).WidthMeters, AnnualWidthMeters, 1.0e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSeasonalFlowLeavesTheBedAloneTest,
	"Massif.WorldGen.SeasonalFlow.LeavesTheBedAlone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSeasonalFlowLeavesTheBedAloneTest::RunTest(const FString& Parameters)
{
	TArray<FMassifRiverReach> Reaches = MakeSeasonalReaches();
	FMassifChannelFlowParams Params;
	MassifSolveChannelFlow(Params, Reaches);

	TArray<float> BedBeforeMeters;
	for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
	{
		BedBeforeMeters.Add(MiddleNode(Reaches).Profile.BedAltitudeMeters[OffsetIndex]);
	}

	MassifSolveSeasonalFlow(
		Params, [](FVector2d, int32 TimeSlice) { return AlpineYearScale(TimeSlice); }, Reaches);

	int32 OffsetsMoved = 0;
	for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
	{
		OffsetsMoved +=
			MiddleNode(Reaches).Profile.BedAltitudeMeters[OffsetIndex] != BedBeforeMeters[OffsetIndex];
	}

	TestEqual(TEXT("A year of seasons leaves the bed exactly where the carve left it"), OffsetsMoved, 0);

	// And so the trench the terrain reads is the same whichever slice is drawn.
	const TSharedRef<const FMassifRiverNetwork> Network = FMassifRiverNetwork::Create(MoveTemp(Reaches));
	const TSharedRef<const FMassifRiverField> Field =
		FMassifRiverField::Build(Network.Get(), MassifDefaultBankReachMeters);
	const FMassifRiverNode& MidstreamNode = Network->GetReaches()[0].Nodes[20];
	const double GroundMeters = Field->SampleChannelGroundMeters(
		MidstreamNode.PositionMeters, MidstreamNode.GroundAltitudeMeters);
	TestTrue(TEXT("The trench is there whatever the season"),
		GroundMeters < MidstreamNode.GroundAltitudeMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSeasonalFlowFlatYearTest,
	"Massif.WorldGen.SeasonalFlow.FlatYear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSeasonalFlowFlatYearTest::RunTest(const FString& Parameters)
{
	// A world with no weather solved for it has to come out with a flat year
	// rather than with no year: every slice is the annual answer, and a slider
	// over it changes nothing instead of emptying the rivers.
	TArray<FMassifRiverReach> Reaches = MakeSeasonalReaches();
	FMassifChannelFlowParams Params;
	MassifSolveChannelFlow(Params, Reaches);

	const double AnnualSurfaceMeters = MiddleNode(Reaches).WaterSurfaceMeters;
	MassifSolveSeasonalFlow(Params, [](FVector2d, int32) { return 1.0; }, Reaches);

	// To a tenth of a millimetre, not to the bit. A slice is the surface solve run
	// once more from the annual answer, and that solve is a fixed point reached
	// from wherever the surface already is rather than a closed form, so applying
	// it again to its own output moves it by the distance it had left to travel.
	int32 SlicesDisagreeing = 0;
	for (int32 TimeSlice = 0; TimeSlice < MassifSeasonalSliceCount; ++TimeSlice)
	{
		SlicesDisagreeing += FMath::IsNearlyEqual(
			MiddleNode(Reaches).Season.WaterSurfaceMeters[TimeSlice], AnnualSurfaceMeters, 1.0e-4) ? 0 : 1;
	}

	TestEqual(TEXT("An unsolved year is the annual answer in every half-month"), SlicesDisagreeing, 0);
	return true;
}
