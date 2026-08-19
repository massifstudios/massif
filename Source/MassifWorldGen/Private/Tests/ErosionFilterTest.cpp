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

	float TotalGullyDepthMeters(const FMassifErosionSettings& Settings)
	{
		float TotalMeters = 0.0f;
		float DepthMeters = Settings.LargestGullyDepthMeters;
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
	FVector2f SampleTurningSlope(FVector2f PositionMeters)
	{
		const float TurnRadians = PositionMeters.X / 400.0f + PositionMeters.Y / 900.0f;
		return FVector2f(FMath::Cos(TurnRadians), FMath::Sin(TurnRadians)) * 0.55f;
	}

	FMassifErosionInput MakeInputAt(FVector2f PositionMeters)
	{
		FMassifErosionInput Input;
		Input.PositionMeters = PositionMeters;
		Input.SlopeMetersPerMeter = SampleTurningSlope(PositionMeters);
		Input.ReliefFraction = 0.5f;
		return Input;
	}

	float EvaluateAt(FVector2f PositionMeters, const FMassifErosionSettings& Settings)
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
	const float TotalDepthMeters = TotalGullyDepthMeters(Settings);

	// Ground with no slope has no flow direction to lay gullies across, so it
	// must come back exactly as it went in. This is what keeps river floors at
	// the elevation their graph gives them.
	auto SampleNoSlope = [](FVector2f) { return FVector2f::ZeroVector; };
	for (float ReliefFraction = 0.0f; ReliefFraction <= 1.0f; ReliefFraction += 0.25f)
	{
		FMassifErosionInput FlatInput;
		FlatInput.PositionMeters = FVector2f(137.0f, -412.0f);
		FlatInput.ReliefFraction = ReliefFraction;
		const float Displacement = MassifErosionFilter::EvaluateDisplacementMeters(FlatInput, Settings, SampleNoSlope);
		TestEqual(FString::Printf(TEXT("Flat ground at relief %.2f is left alone"), ReliefFraction), Displacement, 0.0f, 0.01f);
	}

	// A slope must be gullied: finite, inside the depth the settings allow, and
	// carrying both cuts and the spurs between them.
	float LowestDisplacement = TNumericLimits<float>::Max();
	float HighestDisplacement = TNumericLimits<float>::Lowest();
	for (int32 Step = 0; Step < 512; ++Step)
	{
		const FVector2f Position(static_cast<float>(Step) * 7.3f, 250.0f);
		const float Displacement = EvaluateAt(Position, Settings);
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
	const FVector2f SamplePosition(913.0f, -77.0f);
	const float FirstEvaluation = EvaluateAt(SamplePosition, Settings);
	TestEqual(TEXT("Same point evaluates the same"), FirstEvaluation, EvaluateAt(SamplePosition, Settings));
	TestNotEqual(TEXT("Different seed cuts a different pattern"),
		FirstEvaluation, EvaluateAt(SamplePosition, MakeFilterTestSettings(99)));

	// Sampled at the heightfield's own spacing over a gradient that keeps
	// turning, which is where the pattern tears if the stripes are oriented per
	// point instead of per pivot cell. 8 m over 2 m is already a 76 degree wall,
	// which no gully flank should reach.
	const float StepMeters = 2.0f;
	float MaxNeighborDelta = 0.0f;
	for (int32 StepY = 0; StepY < 64; ++StepY)
	{
		for (int32 StepX = 0; StepX < 512; ++StepX)
		{
			const FVector2f Position(static_cast<float>(StepX) * StepMeters, static_cast<float>(StepY) * StepMeters);
			const float Here = EvaluateAt(Position, Settings);
			MaxNeighborDelta = FMath::Max(MaxNeighborDelta, FMath::Abs(EvaluateAt(Position + FVector2f(StepMeters, 0.0f), Settings) - Here));
			MaxNeighborDelta = FMath::Max(MaxNeighborDelta, FMath::Abs(EvaluateAt(Position + FVector2f(0.0f, StepMeters), Settings) - Here));
		}
	}
	TestTrue(FString::Printf(TEXT("Gully flanks stay below a cliff (%.1f m over %.0f m)"), MaxNeighborDelta, StepMeters),
		MaxNeighborDelta < 8.0f);

	return true;
}
