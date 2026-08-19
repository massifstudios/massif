#include "MassifErosionFilter.h"
#include "Misc/AutomationTest.h"

namespace
{
	FMassifErosionSettings MakeFilterTestSettings(uint32 Seed)
	{
		FMassifErosionSettings Settings;
		Settings.Seed = Seed;
		return Settings;
	}

	double TotalGullyDepthMeters(const FMassifErosionSettings& Settings)
	{
		double TotalMeters = 0.0;
		double DepthMeters = Settings.LargestGullyDepthMeters;
		for (int32 Octave = 0; Octave < Settings.OctaveCount; ++Octave)
		{
			TotalMeters += DepthMeters;
			DepthMeters *= Settings.DepthGain;
		}
		return TotalMeters;
	}

	/**
	 * A macro surface whose gradient turns right through a half circle over a few
	 * hundred meters, as it does along any ridge crest or valley floor. A
	 * constant gradient is not enough to test with: the filter reads its phase
	 * from pivots a cell away, so it is turning gradients that expose whether
	 * that reading tears.
	 */
	FVector2d SampleTurningSlope(FVector2d PositionMeters)
	{
		const double TurnRadians = PositionMeters.X / 400.0 + PositionMeters.Y / 900.0;
		return FVector2d(FMath::Cos(TurnRadians), FMath::Sin(TurnRadians)) * 0.55;
	}

	FMassifErosionInput MakeInputAt(FVector2d PositionMeters)
	{
		FMassifErosionInput Input;
		Input.PositionMeters = PositionMeters;
		Input.SlopeMetersPerMeter = SampleTurningSlope(PositionMeters);
		Input.ReliefFraction = 0.5f;
		return Input;
	}

	double EvaluateAt(FVector2d PositionMeters, const FMassifErosionSettings& Settings)
	{
		return MassifErosionFilter::EvaluateDisplacementMeters(MakeInputAt(PositionMeters), Settings, &SampleTurningSlope);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FErosionFilterTest,
	"Massif.WorldGen.ErosionFilter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FErosionFilterTest::RunTest(const FString& Parameters)
{
	const FMassifErosionSettings Settings = MakeFilterTestSettings(4711);
	const double TotalDepthMeters = TotalGullyDepthMeters(Settings);

	// Ground with no slope has no flow direction to lay gullies across, so it
	// must come back exactly as it went in. This is what keeps river floors at
	// the elevation their graph gives them.
	auto SampleNoSlope = [](FVector2d) { return FVector2d::ZeroVector; };
	for (float ReliefFraction = 0.0f; ReliefFraction <= 1.0f; ReliefFraction += 0.25f)
	{
		FMassifErosionInput FlatInput;
		FlatInput.PositionMeters = FVector2d(137.0, -412.0);
		FlatInput.ReliefFraction = ReliefFraction;
		const double Displacement = MassifErosionFilter::EvaluateDisplacementMeters(FlatInput, Settings, SampleNoSlope);
		TestEqual(FString::Printf(TEXT("Flat ground at relief %.2f is left alone"), ReliefFraction), Displacement, 0.0, 0.01);
	}

	// A slope must be gullied: finite, inside the depth the settings allow, and
	// carrying both cuts and the spurs between them.
	double LowestDisplacement = TNumericLimits<double>::Max();
	double HighestDisplacement = TNumericLimits<double>::Lowest();
	for (int32 Step = 0; Step < 512; ++Step)
	{
		const FVector2d Position(Step * 7.3, 250.0);
		const double Displacement = EvaluateAt(Position, Settings);
		if (!FMath::IsFinite(Displacement) || FMath::Abs(Displacement) > TotalDepthMeters)
		{
			AddError(FString::Printf(TEXT("Displacement %.2f m out of bounds at x = %.1f"), Displacement, Position.X));
			return false;
		}
		LowestDisplacement = FMath::Min(LowestDisplacement, Displacement);
		HighestDisplacement = FMath::Max(HighestDisplacement, Displacement);
	}
	TestTrue(FString::Printf(TEXT("Slope is cut into gullies (%.1f m deepest)"), LowestDisplacement), LowestDisplacement < -10.0f);
	TestTrue(FString::Printf(TEXT("Slope keeps spurs between them (%.1f m highest)"), HighestDisplacement), HighestDisplacement > 10.0f);

	// No grid and no iteration, so the result must depend on nothing but the
	// point's own inputs.
	const FVector2d SamplePosition(913.0, -77.0);
	const double FirstEvaluation = EvaluateAt(SamplePosition, Settings);
	TestEqual(TEXT("Same point evaluates the same"), FirstEvaluation, EvaluateAt(SamplePosition, Settings));
	TestNotEqual(TEXT("Different seed cuts a different pattern"),
		FirstEvaluation, EvaluateAt(SamplePosition, MakeFilterTestSettings(99)));

	// Sampled at the heightfield's own spacing over a gradient that keeps
	// turning, which is where the pattern tears if the stripes are oriented per
	// point instead of per pivot cell. 8 m over 2 m is already a 76 degree wall,
	// which no gully flank should reach.
	const double StepMeters = 2.0;
	double MaxNeighborDelta = 0.0;
	for (int32 StepY = 0; StepY < 64; ++StepY)
	{
		for (int32 StepX = 0; StepX < 512; ++StepX)
		{
			const FVector2d Position(StepX * StepMeters, StepY * StepMeters);
			const double Here = EvaluateAt(Position, Settings);
			MaxNeighborDelta = FMath::Max(MaxNeighborDelta, FMath::Abs(EvaluateAt(Position + FVector2d(StepMeters, 0.0), Settings) - Here));
			MaxNeighborDelta = FMath::Max(MaxNeighborDelta, FMath::Abs(EvaluateAt(Position + FVector2d(0.0, StepMeters), Settings) - Here));
		}
	}
	TestTrue(FString::Printf(TEXT("Gully flanks stay below a cliff (%.1f m over %.0f m)"), MaxNeighborDelta, StepMeters),
		MaxNeighborDelta < 8.0f);

	return true;
}
