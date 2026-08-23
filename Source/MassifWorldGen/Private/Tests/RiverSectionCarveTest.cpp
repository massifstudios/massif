#include "MassifHeightmap.h"
#include "MassifRiverField.h"
#include "MassifRiverNetwork.h"
#include "MassifTerrainField.h"
#include "Misc/AutomationTest.h"

/**
 * What the carve worked into each station's cross-section has to arrive on the
 * ground as the shape it is.
 *
 * The section is the only place a pool against one bank and a bar against the
 * other exist. Reduced to a width and a depth on the way to the terrain they
 * cannot be told from a symmetric ditch of the same size, and the whole sediment
 * solve is spent producing a number the ground never sees.
 */
namespace
{
	constexpr int32 SectionTestSide = 96;
	constexpr double SectionTestSpacingMeters = 4.0;
	constexpr double SectionTestAltitudeMeters = 1200.0;

	/** Dead flat, so anything the terrain does across the channel came from the section. */
	TSharedRef<const FMassifHeightmap> MakeFlatGround()
	{
		TArray<double> Altitudes;
		Altitudes.Init(SectionTestAltitudeMeters, SectionTestSide * SectionTestSide);
		return FMassifHeightmap::FromAltitudes(
			Altitudes, SectionTestSide, SectionTestSide, SectionTestSpacingMeters);
	}

	/** Largest share the leaning section below reaches, so its deepest offset is the full depth. */
	constexpr double LeaningPeakShare = 16.0 / 27.0;

	/** Node spacing and count of the reaches below, which the nose test measures against. */
	constexpr double SectionTestNodeSpacingMeters = 8.0;
	constexpr int32 SectionTestNodeCount = 24;
	constexpr double SectionTestFirstNodeMeters = -92.0;

	/**
	 * A section cut deeper on the right bank than the left, which is what a bend
	 * leaves and what a width and a depth cannot carry.
	 *
	 * Closing on both waterlines and leaning between them: a scour pool against one
	 * bank and a bar against the other, spanning the water and nothing outside it.
	 */
	void SetLeaningProfile(
		FMassifChannelProfile& OutProfile, double HalfSpanMeters, double WettedWidthMeters,
		double DepthMeters)
	{
		OutProfile.HalfSpanMeters = HalfSpanMeters;
		OutProfile.WettedWidthMeters = WettedWidthMeters;

		const double WetHalfSpanMeters = 0.5 * WettedWidthMeters;
		for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			const double AcrossFraction = OutProfile.OffsetMeters(OffsetIndex) / WetHalfSpanMeters;
			const double Share = FMath::Max(1.0 - AcrossFraction * AcrossFraction, 0.0)
				* (0.5 + 0.5 * FMath::Clamp(AcrossFraction, -1.0, 1.0)) / LeaningPeakShare;

			OutProfile.GroundAltitudeMeters[OffsetIndex] = static_cast<float>(SectionTestAltitudeMeters);
			OutProfile.BedAltitudeMeters[OffsetIndex] =
				static_cast<float>(SectionTestAltitudeMeters - DepthMeters * Share);
		}
	}

	/** A channel running east with the same leaning section at every station. */
	FMassifRiverReach MakeLeaningReach(double WidthMeters, double DepthMeters)
	{
		FMassifRiverReach Reach;
		for (int32 NodeIndex = 0; NodeIndex < SectionTestNodeCount; ++NodeIndex)
		{
			FMassifRiverNode Node;
			Node.PositionMeters = FVector2d(
				SectionTestFirstNodeMeters + NodeIndex * SectionTestNodeSpacingMeters, 0.0);
			Node.GroundAltitudeMeters = SectionTestAltitudeMeters;
			Node.WaterSurfaceMeters = SectionTestAltitudeMeters;
			Node.BedAltitudeMeters = SectionTestAltitudeMeters - DepthMeters;
			Node.DischargeCubicMetersPerSecond = 12.0;
			Node.WidthMeters = WidthMeters;
			Node.DepthMeters = DepthMeters;

			// A span of four widths, which is what the profile solve takes.
			SetLeaningProfile(Node.Profile, 2.0 * WidthMeters, WidthMeters, DepthMeters);
			Reach.Nodes.Add(Node);
		}
		return Reach;
	}

	TSharedRef<const FMassifTerrainField> MakeCarvedField(TArray<FMassifRiverReach>&& Reaches)
	{
		const TSharedRef<const FMassifRiverNetwork> Network =
			FMassifRiverNetwork::Create(MoveTemp(Reaches));
		return MakeShared<const FMassifTerrainField>(
			MakeFlatGround(), FMassifRiverField::Build(Network.Get(), MassifDefaultBankReachMeters));
	}

	/** A channel running east at a given northing, cut to a given depth, with no section of its own. */
	FMassifRiverReach MakeParallelReach(double NorthingMeters, double WidthMeters, double DepthMeters)
	{
		FMassifRiverReach Reach;
		for (int32 NodeIndex = 0; NodeIndex < 24; ++NodeIndex)
		{
			FMassifRiverNode Node;
			Node.PositionMeters = FVector2d(-92.0 + NodeIndex * 8.0, NorthingMeters);
			Node.GroundAltitudeMeters = SectionTestAltitudeMeters;
			Node.WaterSurfaceMeters = SectionTestAltitudeMeters;
			Node.BedAltitudeMeters = SectionTestAltitudeMeters - DepthMeters;
			Node.DischargeCubicMetersPerSecond = 12.0;
			Node.WidthMeters = WidthMeters;
			Node.DepthMeters = DepthMeters;
			Reach.Nodes.Add(Node);
		}
		return Reach;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverSectionCarveShapeTest,
	"Massif.WorldGen.RiverSectionCarve.Shape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverSectionCarveShapeTest::RunTest(const FString& Parameters)
{
	constexpr double WidthMeters = 12.0;
	constexpr double DepthMeters = 1.5;
	TArray<FMassifRiverReach> Reaches;
	Reaches.Add(MakeLeaningReach(WidthMeters, DepthMeters));
	const TSharedRef<const FMassifTerrainField> Field = MakeCarvedField(MoveTemp(Reaches));

	// The section's deepest offset is a third of the way towards the right bank,
	// which for a channel running east is the southern side. A parabola fitted to
	// a width and a depth puts it midstream instead, so this fails on anything
	// that reduces the section on the way to the ground.
	double DeepestNorthingMeters = 0.0;
	double DeepestCutMeters = 0.0;
	for (int32 Step = 0; Step <= 96; ++Step)
	{
		const FVector2d PositionMeters(0.0, -24.0 + Step * 0.5);
		const double CutMeters =
			SectionTestAltitudeMeters - Field->SampleHeightMeters(PositionMeters, 0.25);
		if (CutMeters > DeepestCutMeters)
		{
			DeepestCutMeters = CutMeters;
			DeepestNorthingMeters = PositionMeters.Y;
		}
	}

	TestTrue(TEXT("The whole worked depth reaches the ground"), DeepestCutMeters >= DepthMeters - 0.05);
	TestTrue(TEXT("The trench leans the way the section does"), DeepestNorthingMeters < -1.0);

	// Symmetric about the centreline is exactly what the section is not: probed
	// where it leans, half way out to either waterline.
	const double RightBankCutMeters =
		SectionTestAltitudeMeters - Field->SampleHeightMeters(FVector2d(0.0, -3.0), 0.25);
	const double LeftBankCutMeters =
		SectionTestAltitudeMeters - Field->SampleHeightMeters(FVector2d(0.0, 3.0), 0.25);
	TestTrue(TEXT("The banks are not each other's mirror"),
		RightBankCutMeters > LeftBankCutMeters + 0.2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverSectionCarveNoseTest,
	"Massif.WorldGen.RiverSectionCarve.Nose",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverSectionCarveNoseTest::RunTest(const FString& Parameters)
{
	constexpr double WidthMeters = 12.0;
	constexpr double DepthMeters = 1.5;
	TArray<FMassifRiverReach> Reaches;
	Reaches.Add(MakeLeaningReach(WidthMeters, DepthMeters));
	const TSharedRef<const FMassifTerrainField> Field = MakeCarvedField(MoveTemp(Reaches));

	// Two points the same distance from the last station of a reach running east:
	// one straight on downstream, one straight out over the right bank. The first
	// is the middle of a channel that has not finished yet and the second is dry
	// hillside a full span out.
	//
	// Read against the distance to the station rather than across the flow, the
	// section becomes a bowl and the two are the same point of it. A bowl of
	// thirty-three samples arrives on the ground as rings — round every terminus,
	// and round the outside of every bend, where the ground between two spans'
	// perpendiculars belongs to the interior of neither.
	const FVector2d LastStationMeters(
		SectionTestFirstNodeMeters + (SectionTestNodeCount - 1) * SectionTestNodeSpacingMeters, 0.0);
	constexpr double ReachMeters = 10.0;

	const double DownstreamCutMeters = SectionTestAltitudeMeters
		- Field->SampleHeightMeters(LastStationMeters + FVector2d(ReachMeters, 0.0), 0.25);
	const double RightBankCutMeters = SectionTestAltitudeMeters
		- Field->SampleHeightMeters(LastStationMeters + FVector2d(0.0, -ReachMeters), 0.25);

	TestTrue(TEXT("The channel runs on past its last station"), DownstreamCutMeters > 0.4);
	TestTrue(TEXT("The bank a span out is not cut with it"), RightBankCutMeters < 0.05);

	// And it does close, rather than running on at full depth for as far as the
	// index happens to reach.
	const double FarDownstreamCutMeters = SectionTestAltitudeMeters
		- Field->SampleHeightMeters(LastStationMeters + FVector2d(30.0, 0.0), 0.25);
	TestTrue(TEXT("The channel closes into the ground rather than ending on a face"),
		FarDownstreamCutMeters < 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRiverSectionCarveOverlapTest,
	"Massif.WorldGen.RiverSectionCarve.Overlap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRiverSectionCarveOverlapTest::RunTest(const FString& Parameters)
{
	// Two channels of different depths running within a width of each other, which
	// is what every confluence looks like for as far upstream as the two are wide.
	// Cut to whichever channel is nearest, the ground steps between their beds
	// along the line the nearer of the two changes on.
	TArray<FMassifRiverReach> Reaches;
	Reaches.Add(MakeParallelReach(-5.0, 14.0, 2.4));
	Reaches.Add(MakeParallelReach(5.0, 10.0, 0.9));
	const TSharedRef<const FMassifTerrainField> Field = MakeCarvedField(MoveTemp(Reaches));

	constexpr double StepMeters = 0.1;
	double PreviousMeters = Field->SampleHeightMeters(FVector2d(0.0, -30.0), 0.05);
	double LargestStepMeters = 0.0;
	for (int32 Step = 1; Step <= 600; ++Step)
	{
		const FVector2d PositionMeters(0.0, -30.0 + Step * StepMeters);
		const double GroundMeters = Field->SampleHeightMeters(PositionMeters, 0.05);
		LargestStepMeters = FMath::Max(LargestStepMeters, FMath::Abs(GroundMeters - PreviousMeters));
		PreviousMeters = GroundMeters;
	}

	// A tenth of a metre over a tenth of a metre is a bank. The step this is here
	// to catch is the whole difference between two beds, which is 1.5 m.
	TestTrue(TEXT("Two overlapping channels leave one continuous bed"), LargestStepMeters < 0.1);

	// Both are still cut, and the deeper one is not filled in by the shallower.
	const double DeepCutMeters =
		SectionTestAltitudeMeters - Field->SampleHeightMeters(FVector2d(0.0, -5.0), 0.05);
	TestTrue(TEXT("The deeper channel keeps its own bed"), DeepCutMeters >= 2.4 - 0.05);
	return true;
}
