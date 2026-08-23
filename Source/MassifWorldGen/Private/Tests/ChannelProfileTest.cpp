#include "MassifChannelProfile.h"

#include "MassifElevationSource.h"
#include "MassifRiverNetwork.h"
#include "Misc/AutomationTest.h"

namespace
{
	/**
	 * A prismatic trapezoidal channel running due south, so the section every
	 * station takes is the same one and the normal depth in it is a closed form.
	 *
	 * The channel is cut into a plane that falls south at a fixed slope, and the
	 * whole thing translates down that slope, so what the solve sees at any station
	 * is a trench of one shape at one gradient.
	 */
	class FTrapezoidChannelSource final : public IMassifElevationSource
	{
	public:
		FTrapezoidChannelSource(double InBaseHalfWidthMeters, double InSideSlope, double InBedSlope)
			: BaseHalfWidthMeters(InBaseHalfWidthMeters)
			, SideSlope(InSideSlope)
			, BedSlope(InBedSlope)
		{
		}

		double SampleAltitudeMeters(FVector2d PositionMeters, double SampleSpacingMeters) const override
		{
			const double BedMeters = ReferenceAltitudeMeters + BedSlope * PositionMeters.Y;
			const double AcrossMeters = FMath::Abs(PositionMeters.X);
			if (AcrossMeters <= BaseHalfWidthMeters)
			{
				return BedMeters;
			}
			return BedMeters + (AcrossMeters - BaseHalfWidthMeters) / SideSlope;
		}

		FVector2d GetWorldSizeMeters() const override
		{
			return FVector2d(4000.0, 4000.0);
		}

		double GetFinestSpacingMeters() const override
		{
			return 0.01;
		}

		/** Altitude of the bed at Y = 0, which the reach's surface is set against. */
		static constexpr double ReferenceAltitudeMeters = 1000.0;

		double BaseHalfWidthMeters = 0.0;

		/** Metres across per metre up: 2 is a bank at about 27 degrees. */
		double SideSlope = 0.0;

		double BedSlope = 0.0;
	};

	/** What a test reach is set up with, before the section has anything to say about it. */
	struct FTrapezoidReachSpec
	{
		/**
		 * Where the measured water surface sits over the bed.
		 *
		 * The solve never lowers this — along a real channel the elevation model's
		 * altitude is the top of the water, so the surface is measured and only the
		 * column under it is solved for. A surface set at the bed therefore asks the
		 * solve to cut the whole channel, and one set where the section already
		 * carries the flow asks it to cut nothing.
		 */
		double SurfaceAboveBedMeters = 0.0;

		/** The hydraulic-geometry depth, which bounds how far the bed may be cut. */
		double HydraulicDepthMeters = 0.0;

		double DischargeCubicMetersPerSecond = 0.0;
	};

	/** A reach running due south down the trapezoid, station by station. */
	FMassifRiverReach MakeTrapezoidReach(
		const FTrapezoidChannelSource& Source, const FTrapezoidReachSpec& Spec, int32 NodeCount = 41,
		double SpacingMeters = 4.0)
	{
		FMassifRiverReach Reach;
		Reach.Nodes.Reserve(NodeCount);
		for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
		{
			// Upstream first: the reach starts north and runs south.
			const double NorthingMeters = 0.5 * (NodeCount - 1) * SpacingMeters - NodeIndex * SpacingMeters;

			FMassifRiverNode Node;
			Node.PositionMeters = FVector2d(0.0, NorthingMeters);
			Node.GroundAltitudeMeters = Source.SampleAltitudeMeters(Node.PositionMeters, 1.0);
			Node.WaterSurfaceMeters = Node.GroundAltitudeMeters + Spec.SurfaceAboveBedMeters;
			Node.BedAltitudeMeters = Node.GroundAltitudeMeters;
			Node.DischargeCubicMetersPerSecond = Spec.DischargeCubicMetersPerSecond;
			Node.WidthMeters = 2.0 * Source.BaseHalfWidthMeters;
			Node.DepthMeters = Spec.HydraulicDepthMeters;
			Reach.Nodes.Add(Node);
		}
		return Reach;
	}

	/** Manning's discharge through a trapezoid filled to a given depth: what the solve has to reproduce. */
	double TrapezoidDischarge(
		double BaseHalfWidthMeters, double SideSlope, double DepthMeters, double Roughness, double BedSlope)
	{
		const double BaseWidthMeters = 2.0 * BaseHalfWidthMeters;
		const double AreaSquareMeters = DepthMeters * (BaseWidthMeters + SideSlope * DepthMeters);
		const double PerimeterMeters =
			BaseWidthMeters + 2.0 * DepthMeters * FMath::Sqrt(1.0 + SideSlope * SideSlope);
		const double HydraulicRadiusMeters = AreaSquareMeters / PerimeterMeters;
		return AreaSquareMeters * FMath::Pow(HydraulicRadiusMeters, 2.0 / 3.0) * FMath::Sqrt(BedSlope)
			/ Roughness;
	}

	/** The station in the middle of a reach, clear of the ends the slope window is truncated at. */
	const FMassifChannelProfile& MiddleProfile(const FMassifRiverReach& Reach)
	{
		return Reach.Nodes[Reach.Nodes.Num() / 2].Profile;
	}

	FMassifChannelSolveParams MakeProfileTestParams()
	{
		FMassifChannelSolveParams Params;
		// The trapezoid has no quantisation and no staircase in it, so the slope is
		// exact between neighbours and a window would only reach outside the reach.
		Params.SlopeWindowNodes = 1;
		return Params;
	}

	TArray<FMassifRiverReach> SolveOne(const IMassifElevationSource& Source, FMassifRiverReach&& Reach)
	{
		TArray<FMassifRiverReach> Reaches;
		Reaches.Add(MoveTemp(Reach));
		MassifSolveChannelProfiles(Source, MakeProfileTestParams(), Reaches);
		return Reaches;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelProfileNormalDepthTest,
	"Massif.WorldGen.ChannelProfile.NormalDepth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelProfileNormalDepthTest::RunTest(const FString& Parameters)
{
	// The section already holds the whole flow, so the solve should cut nothing:
	// the discharge handed in is exactly what Manning carries at this depth in
	// this trapezoid, and the depth that comes back is the one it was measured at.
	constexpr double BaseHalfWidthMeters = 3.0;
	constexpr double SideSlope = 2.0;
	constexpr double BedSlope = 0.02;
	constexpr double DepthMeters = 1.2;

	const FMassifChannelSolveParams Params = MakeProfileTestParams();
	const double DischargeCubicMetersPerSecond =
		TrapezoidDischarge(BaseHalfWidthMeters, SideSlope, DepthMeters, Params.Roughness, BedSlope);

	const FTrapezoidChannelSource Source(BaseHalfWidthMeters, SideSlope, BedSlope);
	const TArray<FMassifRiverReach> Solved = SolveOne(
		Source, MakeTrapezoidReach(Source, {DepthMeters, DepthMeters, DischargeCubicMetersPerSecond}));
	const FMassifChannelProfile& Profile = MiddleProfile(Solved[0]);

	AddInfo(FString::Printf(
		TEXT("Q %.2f m3/s, depth %.3f m, width %.2f m, speed %.2f m/s, slope %.4f"),
		DischargeCubicMetersPerSecond, Profile.DepthMeters, Profile.WettedWidthMeters,
		Profile.SpeedMetersPerSecond, Profile.EnergySlope));

	TestTrue(
		FString::Printf(TEXT("The energy slope is the bed slope (%.4f)"), Profile.EnergySlope),
		FMath::IsNearlyEqual(Profile.EnergySlope, BedSlope, 1.0e-4));

	// A section sampled at thirty-three offsets over four channel widths lands the
	// bank breaks between offsets, so the closed form is approached and not hit.
	TestTrue(
		FString::Printf(TEXT("The depth is the normal depth (%.3f against %.3f m)"),
			Profile.DepthMeters, DepthMeters),
		FMath::Abs(Profile.DepthMeters - DepthMeters) < 0.1 * DepthMeters);

	const double ExpectedWidthMeters = 2.0 * (BaseHalfWidthMeters + SideSlope * DepthMeters);
	TestTrue(
		FString::Printf(TEXT("The water reaches the banks (%.2f against %.2f m)"),
			Profile.WettedWidthMeters, ExpectedWidthMeters),
		FMath::Abs(Profile.WettedWidthMeters - ExpectedWidthMeters) < 0.15 * ExpectedWidthMeters);

	TestTrue(
		TEXT("The discharge divided by the area is the speed reported"),
		FMath::IsNearlyEqual(
			Profile.SpeedMetersPerSecond * Profile.FlowAreaSquareMeters, DischargeCubicMetersPerSecond,
			0.05 * DischargeCubicMetersPerSecond));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelProfileConfinementTest,
	"Massif.WorldGen.ChannelProfile.Confinement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelProfileConfinementTest::RunTest(const FString& Parameters)
{
	// The same water through a narrow section and a wide one. This is the whole
	// point of solving on the ground rather than on a power law of the discharge:
	// hydraulic geometry gives both of them one width and one depth.
	constexpr double BedSlope = 0.02;
	constexpr double DischargeCubicMetersPerSecond = 12.0;

	// Surfaces at the same height over each bed, and a ceiling on the cut generous
	// enough that neither section is stopped by it before it can carry the water.
	const FTrapezoidReachSpec Spec{0.4, 2.0, DischargeCubicMetersPerSecond};

	const FTrapezoidChannelSource Narrow(1.5, 0.5, BedSlope);
	const FTrapezoidChannelSource Wide(12.0, 4.0, BedSlope);

	const TArray<FMassifRiverReach> NarrowSolved = SolveOne(Narrow, MakeTrapezoidReach(Narrow, Spec));
	const TArray<FMassifRiverReach> WideSolved = SolveOne(Wide, MakeTrapezoidReach(Wide, Spec));

	const FMassifChannelProfile& NarrowProfile = MiddleProfile(NarrowSolved[0]);
	const FMassifChannelProfile& WideProfile = MiddleProfile(WideSolved[0]);

	AddInfo(FString::Printf(
		TEXT("narrow %.2f m wide, %.2f m deep | wide %.2f m wide, %.2f m deep"),
		NarrowProfile.WettedWidthMeters, NarrowProfile.DepthMeters, WideProfile.WettedWidthMeters,
		WideProfile.DepthMeters));

	TestTrue(
		TEXT("The confined section runs narrower"),
		NarrowProfile.WettedWidthMeters < WideProfile.WettedWidthMeters);
	TestTrue(TEXT("The confined section runs deeper"), NarrowProfile.DepthMeters > WideProfile.DepthMeters);
	TestTrue(
		TEXT("The confined section runs faster"),
		NarrowProfile.SpeedMetersPerSecond > WideProfile.SpeedMetersPerSecond);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelProfileSlopeTest,
	"Massif.WorldGen.ChannelProfile.Slope",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelProfileSlopeTest::RunTest(const FString& Parameters)
{
	// One section at two gradients. Steeper water moves faster, so it needs less
	// room, which is the variation the hydraulic geometry cannot express either.
	constexpr double DischargeCubicMetersPerSecond = 12.0;

	// The surface sits on the bed, so the section starts holding nothing and the
	// whole channel is cut. A surface set high enough that the section already
	// carries the flow would be answered with no cut at either gradient, and the
	// two would come back identical for a reason that is not about slope.
	const FTrapezoidReachSpec Spec{0.0, 2.0, DischargeCubicMetersPerSecond};

	const FTrapezoidChannelSource Gentle(4.0, 2.0, 0.004);
	const FTrapezoidChannelSource Steep(4.0, 2.0, 0.06);

	// Held in named locals: MiddleProfile returns a reference into the array, and
	// the array is what owns the reaches.
	const TArray<FMassifRiverReach> GentleSolved = SolveOne(Gentle, MakeTrapezoidReach(Gentle, Spec));
	const TArray<FMassifRiverReach> SteepSolved = SolveOne(Steep, MakeTrapezoidReach(Steep, Spec));

	const FMassifChannelProfile& GentleProfile = MiddleProfile(GentleSolved[0]);
	const FMassifChannelProfile& SteepProfile = MiddleProfile(SteepSolved[0]);

	AddInfo(FString::Printf(
		TEXT("gentle %.2f m deep at %.2f m/s | steep %.2f m deep at %.2f m/s"),
		GentleProfile.DepthMeters, GentleProfile.SpeedMetersPerSecond, SteepProfile.DepthMeters,
		SteepProfile.SpeedMetersPerSecond));

	TestTrue(TEXT("The steeper reach runs shallower"), SteepProfile.DepthMeters < GentleProfile.DepthMeters);
	TestTrue(
		TEXT("The steeper reach runs faster"),
		SteepProfile.SpeedMetersPerSecond > GentleProfile.SpeedMetersPerSecond);
	TestTrue(
		TEXT("The steeper reach is nearer to breaking white"),
		SteepProfile.FroudeNumber > GentleProfile.FroudeNumber);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelProfileDeterminismTest,
	"Massif.WorldGen.ChannelProfile.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FChannelProfileDeterminismTest::RunTest(const FString& Parameters)
{
	const FTrapezoidChannelSource Source(4.0, 2.0, 0.02);

	const FTrapezoidReachSpec Spec{0.8, 0.8, 9.0};
	const TArray<FMassifRiverReach> First = SolveOne(Source, MakeTrapezoidReach(Source, Spec));
	const TArray<FMassifRiverReach> Second = SolveOne(Source, MakeTrapezoidReach(Source, Spec));

	int32 StationsDisagreeing = 0;
	for (int32 NodeIndex = 0; NodeIndex < First[0].Nodes.Num(); ++NodeIndex)
	{
		const FMassifChannelProfile& Left = First[0].Nodes[NodeIndex].Profile;
		const FMassifChannelProfile& Right = Second[0].Nodes[NodeIndex].Profile;
		StationsDisagreeing += Left.DepthMeters != Right.DepthMeters ? 1 : 0;
		StationsDisagreeing += Left.WettedWidthMeters != Right.WettedWidthMeters ? 1 : 0;
	}

	// Bit-identical, not near: the solve runs a fixed count of bisection steps over
	// a pure function of the section, so anything else is a dependence on how the
	// reaches happened to be split across threads.
	TestEqual(TEXT("The same section solves to the same channel"), StationsDisagreeing, 0);
	return true;
}
