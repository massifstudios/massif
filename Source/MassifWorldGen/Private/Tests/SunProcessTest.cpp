#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifSunProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 SunTestSeed = 4711;
	constexpr double SunTestWorldMeters = 4000.0;
	constexpr double SunTestLatitudeDegrees = 47.2;

	/** Half-monthly slices of a representative year, which is what the sun channels hold. */
	constexpr int32 SunTestSliceCount = 24;

	/** The half-months the solstices fall in: slice 11 ends in late June, slice 23 in late December. */
	constexpr int32 SummerSlice = 11;
	constexpr int32 WinterSlice = 23;

	/** Only the sun runs here: the water solve has nothing to say about shading and costs time. */
	TUniquePtr<FMassifEnvironmentScheduler> SolveSun(
		const FMassifAnalyticElevationSource::FParams& TerrainParams, EMassifFidelity Fidelity,
		int32 SunPathStepCount = FMassifSunParams().SunPathStepCount)
	{
		FMassifGeoreference Georeference;
		Georeference.LatitudeDegrees = SunTestLatitudeDegrees;

		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), Georeference, SunTestSeed);

		FMassifEnvironmentBakeParams WaterParams;
		WaterParams.CellSizeMeters = 32.0;
		MassifRegisterAltitudeProcess(*Scheduler, WaterParams);

		FMassifSunParams SunParams;
		SunParams.CellSizeMeters = 64.0;
		SunParams.SunPathStepCount = SunPathStepCount;
		MassifRegisterSunProcesses(*Scheduler, SunParams);

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Insolation(), Fidelity);
		Scheduler->Execute();
		return Scheduler;
	}

	FMassifAnalyticElevationSource::FParams MakeSunTerrain(
		EMassifAnalyticTerrain Shape, double BearingDegrees, double ReliefMeters = 900.0)
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = Shape;
		Params.WorldSizeMeters = SunTestWorldMeters;
		Params.BaseAltitudeMeters = 1500.0;
		Params.ReliefMeters = ReliefMeters;
		Params.BearingDegrees = BearingDegrees;
		return Params;
	}

	/**
	 * Sky view a cone-shaped hollow leaves its own floor.
	 *
	 * The walls rise at the same angle in every direction, so the horizon is that
	 * one angle all the way round and the standard cos-squared form is exact.
	 */
	double ExpectedPitSkyViewFactor(double ReliefMeters)
	{
		const double WallAngleRadians = FMath::Atan(ReliefMeters / (0.5 * SunTestWorldMeters));
		return FMath::Square(FMath::Cos(WallAngleRadians));
	}

	/** Middle of a half-month, which is the day the solve takes that slice's sun path on. */
	double SunTestSliceMidDayOfYear(int32 TimeSlice)
	{
		return (TimeSlice + 0.5) * 365.0 / SunTestSliceCount;
	}

	/**
	 * What level open ground takes over one whole day, in arbitrary but fixed units.
	 *
	 * The closed form of the integral the solve approximates: sin(altitude) over the
	 * hour angles between sunrise and sunset. Written out here rather than borrowed
	 * so that the test is a statement about the sun and not about the code — and it
	 * carries both of the things that make a winter day dark, the low sun in the
	 * declination and the short day in the sunset hour angle.
	 */
	double LevelDayBeam(double LatitudeDegrees, double DayOfYear)
	{
		const double LatitudeRadians = FMath::DegreesToRadians(LatitudeDegrees);
		const double DeclinationRadians =
			FMath::DegreesToRadians(23.44) * FMath::Sin(UE_DOUBLE_TWO_PI * (284.0 + DayOfYear) / 365.0);

		const double CosSunset = FMath::Clamp(
			-FMath::Tan(LatitudeRadians) * FMath::Tan(DeclinationRadians), -1.0, 1.0);
		const double SunsetRadians = FMath::Acos(CosSunset);

		return 2.0 * (SunsetRadians * FMath::Sin(LatitudeRadians) * FMath::Sin(DeclinationRadians)
			+ FMath::Cos(LatitudeRadians) * FMath::Cos(DeclinationRadians) * FMath::Sin(SunsetRadians));
	}

	double SampleSunChannel(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName, FVector2d PositionMeters,
		int32 TimeSlice = 0)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		return State.SampleScalar(State.FindChannel(ChannelName), PositionMeters, TimeSlice);
	}

	double ReadSunCell(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName, int32 Column, int32 Row,
		int32 TimeSlice = 0)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		return State.ReadCell(State.FindChannel(ChannelName), Column, Row, 0, TimeSlice);
	}

	int32 SunSliceCount(const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		return State.GetDescriptor(State.FindChannel(ChannelName)).TimeSliceCount;
	}

	/** The finest difference a uint16 channel can hold, which is the tolerance any readback carries. */
	double SunQuantisationStep(const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		const FMassifChannelHandle Channel = State.FindChannel(ChannelName);
		return (State.GetChannelMaximum(Channel) - State.GetChannelMinimum(Channel)) / 65535.0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunFlatWorldTest,
	"Massif.WorldGen.Sun.FlatWorld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunFlatWorldTest::RunTest(const FString& Parameters)
{
	// Level open ground over a whole growing season is what the channels are
	// measured against, so it has to come out as one on both counts or every
	// reading elsewhere is scaled wrong.
	const TUniquePtr<FMassifEnvironmentScheduler> Flat =
		SolveSun(MakeSunTerrain(EMassifAnalyticTerrain::Flat, 0.0), EMassifFidelity::Standard);

	const double SkyView = SampleSunChannel(*Flat, MassifEnvironmentChannels::SkyViewFactor(), FVector2d::ZeroVector);
	const double Annual =
		SampleSunChannel(*Flat, MassifEnvironmentChannels::AnnualInsolation(), FVector2d::ZeroVector);

	TestTrue(FString::Printf(TEXT("A flat world sees the whole sky (%.4f)"), SkyView), SkyView > 0.999);
	TestTrue(
		FString::Printf(TEXT("A flat world takes a full growing season of sun (%.4f)"), Annual),
		FMath::Abs(Annual - 1.0) < 0.001);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunSeasonTest,
	"Massif.WorldGen.Sun.Season",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunSeasonTest::RunTest(const FString& Parameters)
{
	// The whole point of slicing the channel: at 47 degrees north a December day is
	// both lower and shorter than a June one, and level ground takes a fraction of
	// the sun in one that it takes in the other. Measured against the closed form of
	// the same integral rather than against a remembered number, so the test states
	// what the sun does instead of what the solve last happened to produce.
	const TUniquePtr<FMassifEnvironmentScheduler> Flat =
		SolveSun(MakeSunTerrain(EMassifAnalyticTerrain::Flat, 0.0), EMassifFidelity::Standard);

	TestEqual(
		TEXT("The direct beam is held half-monthly"),
		SunSliceCount(*Flat, MassifEnvironmentChannels::DirectInsolation()), SunTestSliceCount);

	const double Summer = SampleSunChannel(
		*Flat, MassifEnvironmentChannels::DirectInsolation(), FVector2d::ZeroVector, SummerSlice);
	const double Winter = SampleSunChannel(
		*Flat, MassifEnvironmentChannels::DirectInsolation(), FVector2d::ZeroVector, WinterSlice);

	TestTrue(
		FString::Printf(TEXT("Midwinter is darker than midsummer (%.4f against %.4f)"), Winter, Summer),
		Winter < Summer);

	const double ExpectedRatio = LevelDayBeam(SunTestLatitudeDegrees, SunTestSliceMidDayOfYear(SummerSlice))
		/ LevelDayBeam(SunTestLatitudeDegrees, SunTestSliceMidDayOfYear(WinterSlice));
	const double SolvedRatio = Summer / Winter;

	// One per cent covers the finite number of steps the day is integrated at; the
	// ratio itself is the declination's, and the reference the slices are divided
	// through by cancels out of it.
	TestTrue(
		FString::Printf(TEXT("And by the factor the declination asks for (%.3f against %.3f)"),
			SolvedRatio, ExpectedRatio),
		FMath::Abs(SolvedRatio - ExpectedRatio) < 0.01 * ExpectedRatio);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunPitTest,
	"Massif.WorldGen.Sun.Pit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunPitTest::RunTest(const FString& Parameters)
{
	// The floor of a hole has ground all around it above the horizontal, which is
	// exactly the case a local slope-and-aspect estimate cannot see at all.
	const TUniquePtr<FMassifEnvironmentScheduler> Pit =
		SolveSun(MakeSunTerrain(EMassifAnalyticTerrain::Pit, 0.0), EMassifFidelity::Standard);

	const double SkyView = SampleSunChannel(*Pit, MassifEnvironmentChannels::SkyViewFactor(), FVector2d::ZeroVector);
	const double Annual =
		SampleSunChannel(*Pit, MassifEnvironmentChannels::AnnualInsolation(), FVector2d::ZeroVector);

	const double Expected = ExpectedPitSkyViewFactor(900.0);
	TestTrue(
		FString::Printf(TEXT("The pit floor's sky view matches the closed form (%.4f against %.4f)"),
			SkyView, Expected),
		FMath::Abs(SkyView - Expected) < 0.02);
	TestTrue(FString::Printf(TEXT("And it takes less sun than open ground (%.4f)"), Annual), Annual < 0.95);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunRidgeAspectTest,
	"Massif.WorldGen.Sun.RidgeAspect",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunRidgeAspectTest::RunTest(const FString& Parameters)
{
	// A ridge running east-west has a south flank and a north flank, and in the
	// northern hemisphere the south flank takes far more sun — which is what puts
	// the treeline higher on one side of a valley than the other. Steep, because
	// through the growing season at 47 degrees north the noon sun is high and a
	// gentle flank barely separates from its opposite number.
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge =
		SolveSun(MakeSunTerrain(EMassifAnalyticTerrain::Ridge, 90.0, 2400.0), EMassifFidelity::Standard);

	const double SouthFlank =
		SampleSunChannel(*Ridge, MassifEnvironmentChannels::AnnualInsolation(), FVector2d(0.0, -900.0));
	const double NorthFlank =
		SampleSunChannel(*Ridge, MassifEnvironmentChannels::AnnualInsolation(), FVector2d(0.0, 900.0));

	TestTrue(
		FString::Printf(TEXT("The south flank takes more sun than the north (%.3f against %.3f)"),
			SouthFlank, NorthFlank),
		SouthFlank > NorthFlank * 1.2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunWinterAspectTest,
	"Massif.WorldGen.Sun.WinterAspect",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunWinterAspectTest::RunTest(const FString& Parameters)
{
	// The property the whole seasonal channel exists for. Both flanks lose sun
	// between June and December, but the north one loses more of it: the low winter
	// sun never clears its own slope, while the south flank turns to meet it. A
	// single midsummer slice cannot express this at all, and reading it in December
	// melts snow off a north face that in truth sees no sun for months.
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge =
		SolveSun(MakeSunTerrain(EMassifAnalyticTerrain::Ridge, 90.0, 2400.0), EMassifFidelity::Standard);

	const FName& Direct = MassifEnvironmentChannels::DirectInsolation();
	const double SouthSummer = SampleSunChannel(*Ridge, Direct, FVector2d(0.0, -900.0), SummerSlice);
	const double SouthWinter = SampleSunChannel(*Ridge, Direct, FVector2d(0.0, -900.0), WinterSlice);
	const double NorthSummer = SampleSunChannel(*Ridge, Direct, FVector2d(0.0, 900.0), SummerSlice);
	const double NorthWinter = SampleSunChannel(*Ridge, Direct, FVector2d(0.0, 900.0), WinterSlice);

	TestTrue(
		FString::Printf(TEXT("Both flanks are lit in summer (%.4f south, %.4f north)"),
			SouthSummer, NorthSummer),
		SouthSummer > 0.0 && NorthSummer > 0.0);

	// Cross-multiplied rather than divided: a north flank that takes nothing at all
	// in December is the strongest form of the claim, not a division by zero.
	TestTrue(
		FString::Printf(
			TEXT("The north flank keeps the smaller share of its summer (%.4f/%.4f against %.4f/%.4f)"),
			NorthWinter, NorthSummer, SouthWinter, SouthSummer),
		NorthWinter * SouthSummer < SouthWinter * NorthSummer);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunAnnualMeanTest,
	"Massif.WorldGen.Sun.AnnualMean",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunAnnualMeanTest::RunTest(const FString& Parameters)
{
	// The annual channel is not a separate solve: it is the growing-season slices
	// averaged, which is what makes it safe for a treeline to read one number
	// instead of twelve.
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge =
		SolveSun(MakeSunTerrain(EMassifAnalyticTerrain::Ridge, 90.0, 2400.0), EMassifFidelity::Standard);

	const FMassifSunParams SunParams;
	const FMassifEnvironmentState& State = Ridge->GetState();
	const FMassifChannelGrid& Grid =
		State.GetGrid(State.FindChannel(MassifEnvironmentChannels::AnnualInsolation()));
	const int32 Column = Grid.Width / 2;
	const int32 Row = Grid.Height / 4;

	double Mean = 0.0;
	for (int32 TimeSlice = SunParams.GrowingSeasonFirstSlice; TimeSlice <= SunParams.GrowingSeasonLastSlice;
		++TimeSlice)
	{
		Mean += ReadSunCell(*Ridge, MassifEnvironmentChannels::DirectInsolation(), Column, Row, TimeSlice);
	}
	Mean /= SunParams.GrowingSeasonLastSlice - SunParams.GrowingSeasonFirstSlice + 1;

	const double Annual = ReadSunCell(*Ridge, MassifEnvironmentChannels::AnnualInsolation(), Column, Row);
	const double Tolerance = SunQuantisationStep(*Ridge, MassifEnvironmentChannels::DirectInsolation())
		+ SunQuantisationStep(*Ridge, MassifEnvironmentChannels::AnnualInsolation());
	TestTrue(
		FString::Printf(TEXT("The annual channel is the growing-season mean (%.6f against %.6f)"),
			Annual, Mean),
		FMath::Abs(Annual - Mean) <= Tolerance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunHorizonSweptOnceTest,
	"Massif.WorldGen.Sun.HorizonSweptOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunHorizonSweptOnceTest::RunTest(const FString& Parameters)
{
	// The sweep is the expensive part of the bake and the only part of it that is
	// the same on every day of the year, so it must run once per cell and feed all
	// twenty-four slices. That it does is structural rather than measurable:
	// Sun.SkyViewFactor comes straight out of the sweep and holds a single slice, so
	// there is one horizon per cell to be had and no per-slice loop it could sit in.
	const FMassifAnalyticElevationSource::FParams Terrain =
		MakeSunTerrain(EMassifAnalyticTerrain::Pit, 0.0, 2400.0);
	const TUniquePtr<FMassifEnvironmentScheduler> Coarse = SolveSun(Terrain, EMassifFidelity::Standard, 4);
	const TUniquePtr<FMassifEnvironmentScheduler> Fine = SolveSun(Terrain, EMassifFidelity::Standard, 32);

	TestEqual(
		TEXT("The sky view holds one slice, so the sweep has nowhere per-slice to live"),
		SunSliceCount(*Coarse, MassifEnvironmentChannels::SkyViewFactor()), 1);
	TestEqual(
		TEXT("While the beam it feeds is half-monthly"),
		SunSliceCount(*Coarse, MassifEnvironmentChannels::DirectInsolation()), SunTestSliceCount);

	// And what comes out of the sweep does not move when the sun path is integrated
	// eight times as finely, because the sweep never saw the sun path.
	const FMassifChannelGrid& Grid =
		Coarse->GetState().GetGrid(Coarse->GetState().FindChannel(MassifEnvironmentChannels::SkyViewFactor()));

	double LargestDifference = 0.0;
	for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
	{
		const int32 Column = CellIndex % Grid.Width;
		const int32 Row = CellIndex / Grid.Width;
		LargestDifference = FMath::Max(
			LargestDifference,
			FMath::Abs(ReadSunCell(*Coarse, MassifEnvironmentChannels::SkyViewFactor(), Column, Row)
				- ReadSunCell(*Fine, MassifEnvironmentChannels::SkyViewFactor(), Column, Row)));
	}

	TestEqual(
		FString::Printf(TEXT("The sky view does not depend on the sun path (worst cell off by %.9f)"),
			LargestDifference),
		LargestDifference, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunShadowingTest,
	"Massif.WorldGen.Sun.Shadowing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunShadowingTest::RunTest(const FString& Parameters)
{
	// Cheap is the same cosine law without a horizon. Turning the sweep on may only
	// ever take light away, and in a pit it must take a visible amount.
	const FMassifAnalyticElevationSource::FParams Terrain =
		MakeSunTerrain(EMassifAnalyticTerrain::Pit, 0.0, 2400.0);
	const TUniquePtr<FMassifEnvironmentScheduler> Unshadowed = SolveSun(Terrain, EMassifFidelity::Cheap);
	const TUniquePtr<FMassifEnvironmentScheduler> Shadowed = SolveSun(Terrain, EMassifFidelity::Standard);

	const double CheapSkyView =
		SampleSunChannel(*Unshadowed, MassifEnvironmentChannels::SkyViewFactor(), FVector2d::ZeroVector);
	TestTrue(
		FString::Printf(TEXT("Without a sweep the sky is taken as open (%.4f)"), CheapSkyView),
		CheapSkyView > 0.999);

	const double CheapAnnual =
		SampleSunChannel(*Unshadowed, MassifEnvironmentChannels::AnnualInsolation(), FVector2d::ZeroVector);
	const double ShadowedAnnual =
		SampleSunChannel(*Shadowed, MassifEnvironmentChannels::AnnualInsolation(), FVector2d::ZeroVector);
	TestTrue(
		FString::Printf(TEXT("Shadowing only ever removes light (%.4f against %.4f)"),
			ShadowedAnnual, CheapAnnual),
		ShadowedAnnual < CheapAnnual);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunOffTest,
	"Massif.WorldGen.Sun.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunOffTest::RunTest(const FString& Parameters)
{
	// Off still writes the channels, every slice of them, so nothing downstream has
	// to ask whether the sun was solved before reading a half-month out of it.
	const TUniquePtr<FMassifEnvironmentScheduler> Unlit =
		SolveSun(MakeSunTerrain(EMassifAnalyticTerrain::Pit, 0.0), EMassifFidelity::Off);
	const FMassifEnvironmentState& State = Unlit->GetState();

	const FName Names[] = {
		MassifEnvironmentChannels::SkyViewFactor(), MassifEnvironmentChannels::DirectInsolation(),
		MassifEnvironmentChannels::DiffuseInsolation(), MassifEnvironmentChannels::AnnualInsolation()};
	for (const FName& Name : Names)
	{
		const FMassifChannelHandle Channel = State.FindChannel(Name);
		TestTrue(FString::Printf(TEXT("%s exists at Off"), *Name.ToString()), Channel.IsValid());
		TestTrue(
			FString::Printf(TEXT("%s is written at Off"), *Name.ToString()), State.IsChannelWritten(Channel));
		// The channel range spans every slice committed, so one pair of readings
		// covers every cell of every half-month.
		TestEqual(
			FString::Printf(TEXT("%s reads as open sky in its dimmest cell at Off"), *Name.ToString()),
			State.GetChannelMinimum(Channel), 1.0);
		TestEqual(
			FString::Printf(TEXT("%s reads as open sky in its brightest cell at Off"), *Name.ToString()),
			State.GetChannelMaximum(Channel), 1.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunHorizonBearingsTest,
	"Massif.WorldGen.Sun.HorizonBearings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunHorizonBearingsTest::RunTest(const FString& Parameters)
{
	// What Sun.Horizon adds over Sun.SkyViewFactor is *where* the skyline stands,
	// and a channel that got the compass wrong would still report a plausible sky
	// view while putting every shadow at the wrong hour. A tilted plane pins the
	// convention: it descends towards its bearing, so the ground rises towards the
	// opposite one, and the skyline is the plane's own slope there and nothing at
	// all behind it.
	const TUniquePtr<FMassifEnvironmentScheduler> Plane = SolveSun(
		MakeSunTerrain(EMassifAnalyticTerrain::TiltedPlane, 0.0), EMassifFidelity::Standard);

	// Along a constant gradient every sample on the ray returns the same tangent,
	// so the swept maximum is the slope itself rather than an artefact of where the
	// steps happened to land.
	const double SlopeDegrees =
		FMath::RadiansToDegrees(FMath::Atan(900.0 / SunTestWorldMeters));

	const int32 DownhillBearing = 0;
	const int32 UphillBearing = MassifHorizonAzimuthCount / 2;
	const double Uphill = SampleSunChannel(
		*Plane, MassifEnvironmentChannels::Horizon(), FVector2d::ZeroVector, UphillBearing);
	const double Downhill = SampleSunChannel(
		*Plane, MassifEnvironmentChannels::Horizon(), FVector2d::ZeroVector, DownhillBearing);

	TestTrue(
		FString::Printf(TEXT("The skyline uphill is the plane's slope (%.3f against %.3f degrees)"),
			Uphill, SlopeDegrees),
		FMath::Abs(Uphill - SlopeDegrees) <= 0.05);
	TestTrue(
		FString::Printf(TEXT("And downhill there is no skyline at all (%.3f degrees)"), Downhill),
		Downhill <= 0.05);

	// The two extremes must be the antipodal pair, not merely far apart: a bearing
	// axis rotated by a few bins would still separate them.
	int32 HighestBearing = 0;
	int32 LowestBearing = 0;
	double HighestDegrees = -TNumericLimits<double>::Max();
	double LowestDegrees = TNumericLimits<double>::Max();
	for (int32 BearingIndex = 0; BearingIndex < MassifHorizonAzimuthCount; ++BearingIndex)
	{
		const double Degrees = SampleSunChannel(
			*Plane, MassifEnvironmentChannels::Horizon(), FVector2d::ZeroVector, BearingIndex);
		if (Degrees > HighestDegrees)
		{
			HighestDegrees = Degrees;
			HighestBearing = BearingIndex;
		}
		if (Degrees < LowestDegrees)
		{
			LowestDegrees = Degrees;
			LowestBearing = BearingIndex;
		}
	}

	TestEqual(TEXT("The highest skyline is the uphill bearing"), HighestBearing, UphillBearing);
	TestEqual(TEXT("The lowest is the one opposite it"), LowestBearing, DownhillBearing);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunHorizonPitWallTest,
	"Massif.WorldGen.Sun.HorizonPitWall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunHorizonPitWallTest::RunTest(const FString& Parameters)
{
	// A conical pit rises at one angle in every direction, so its floor is the one
	// shape whose whole skyline has a closed form. That pins the magnitude against
	// trigonometry rather than against the code, and cross-checks the sky view
	// factor the same sweep produces.
	constexpr double PitReliefMeters = 900.0;
	const TUniquePtr<FMassifEnvironmentScheduler> Pit = SolveSun(
		MakeSunTerrain(EMassifAnalyticTerrain::Pit, 0.0, PitReliefMeters), EMassifFidelity::Standard);

	const double WallDegrees =
		FMath::RadiansToDegrees(FMath::Atan(PitReliefMeters / (0.5 * SunTestWorldMeters)));

	double LargestError = 0.0;
	for (int32 BearingIndex = 0; BearingIndex < MassifHorizonAzimuthCount; ++BearingIndex)
	{
		const double Degrees = SampleSunChannel(
			*Pit, MassifEnvironmentChannels::Horizon(), FVector2d::ZeroVector, BearingIndex);
		LargestError = FMath::Max(LargestError, FMath::Abs(Degrees - WallDegrees));
	}

	// The floor sits at the world origin and the cells do not, so each bearing
	// sweeps from up to half a cell off centre and reads a wall a little nearer or
	// further than the closed form assumes. That is worth about half a degree here
	// against a signal of twenty-four.
	TestTrue(
		FString::Printf(TEXT("Every bearing off the pit floor reads the wall angle %.3f (worst off by %.3f)"),
			WallDegrees, LargestError),
		LargestError <= 0.75);

	// The sky view is a reduction of the same swept angles, so agreeing with it is
	// what says the two are reading one sweep and not two.
	const double SkyView =
		SampleSunChannel(*Pit, MassifEnvironmentChannels::SkyViewFactor(), FVector2d::ZeroVector);
	const double ExpectedSkyView = ExpectedPitSkyViewFactor(PitReliefMeters);
	TestTrue(
		FString::Printf(TEXT("And the sky view reduces to the same wall (%.4f against %.4f)"),
			SkyView, ExpectedSkyView),
		FMath::Abs(SkyView - ExpectedSkyView) <= 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunHorizonUnsweptIsLevelTest,
	"Massif.WorldGen.Sun.HorizonUnsweptIsLevel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunHorizonUnsweptIsLevelTest::RunTest(const FString& Parameters)
{
	// Off and Cheap both skip the sweep, and both must still write the channel, so
	// that nothing downstream has to ask whether the sun was solved (ARCHITECTURE
	// §5.6.3). The trivial value here is zero rather than the one the fraction
	// channels write: an open sky is nothing standing above level.
	const FMassifAnalyticElevationSource::FParams Terrain =
		MakeSunTerrain(EMassifAnalyticTerrain::Pit, 0.0, 2400.0);
	const EMassifFidelity Unswept[] = {EMassifFidelity::Off, EMassifFidelity::Cheap};

	for (EMassifFidelity Fidelity : Unswept)
	{
		const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = SolveSun(Terrain, Fidelity);
		const FMassifEnvironmentState& State = Scheduler->GetState();
		const FMassifChannelHandle Channel = State.FindChannel(MassifEnvironmentChannels::Horizon());

		TestTrue(TEXT("The horizon is written even where it is not swept"), State.IsChannelWritten(Channel));
		TestEqual(TEXT("Its deepest cell is level"), State.GetChannelMinimum(Channel), 0.0);
		TestEqual(TEXT("And so is its highest"), State.GetChannelMaximum(Channel), 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSunHorizonBearingsSurviveRefinementTest,
	"Massif.WorldGen.Sun.HorizonBearingsSurviveRefinement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSunHorizonBearingsSurviveRefinementTest::RunTest(const FString& Parameters)
{
	// Reference sweeps twice as many azimuths, but a channel's slice count is fixed
	// when the process declares itself, before any fidelity is known. The stored
	// bearings are therefore a subset of the swept ones at both levels: refining
	// sharpens each angle without moving where the bearings sit. If that ever
	// stopped holding, the same bearing index would mean two different compass
	// directions at two fidelities.
	const FMassifAnalyticElevationSource::FParams Terrain =
		MakeSunTerrain(EMassifAnalyticTerrain::Ridge, 90.0, 1800.0);
	const TUniquePtr<FMassifEnvironmentScheduler> Standard = SolveSun(Terrain, EMassifFidelity::Standard);
	const TUniquePtr<FMassifEnvironmentScheduler> Reference = SolveSun(Terrain, EMassifFidelity::Reference);

	TestEqual(
		TEXT("Both hold one slice per stored bearing"),
		SunSliceCount(*Standard, MassifEnvironmentChannels::Horizon()), MassifHorizonAzimuthCount);
	TestEqual(
		TEXT("However finely the sweep ran"),
		SunSliceCount(*Reference, MassifEnvironmentChannels::Horizon()), MassifHorizonAzimuthCount);

	const FVector2d Flank(0.0, -SunTestWorldMeters * 0.25);
	double LargestDifference = 0.0;
	for (int32 BearingIndex = 0; BearingIndex < MassifHorizonAzimuthCount; ++BearingIndex)
	{
		LargestDifference = FMath::Max(
			LargestDifference,
			FMath::Abs(
				SampleSunChannel(*Standard, MassifEnvironmentChannels::Horizon(), Flank, BearingIndex)
				- SampleSunChannel(*Reference, MassifEnvironmentChannels::Horizon(), Flank, BearingIndex)));
	}

	TestTrue(
		FString::Printf(TEXT("And the stored bearings agree across fidelity (worst off by %.4f degrees)"),
			LargestDifference),
		LargestDifference <= 1.0);
	return true;
}
