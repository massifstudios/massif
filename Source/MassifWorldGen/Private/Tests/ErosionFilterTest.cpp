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

	FMassifErosionInput MakeSlopeInput(float PositionX, float PositionY)
	{
		FMassifErosionInput Input;
		Input.PositionMeters = FVector2f(PositionX, PositionY);
		Input.SlopeMetersPerMeter = FVector2f(0.5f, 0.2f);
		Input.ReliefFraction = 0.5f;
		return Input;
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
	for (float ReliefFraction = 0.0f; ReliefFraction <= 1.0f; ReliefFraction += 0.25f)
	{
		FMassifErosionInput FlatInput;
		FlatInput.PositionMeters = FVector2f(137.0f, -412.0f);
		FlatInput.ReliefFraction = ReliefFraction;
		const float Displacement = MassifErosionFilter::EvaluateDisplacementMeters(FlatInput, Settings);
		TestEqual(FString::Printf(TEXT("Flat ground at relief %.2f is left alone"), ReliefFraction), Displacement, 0.0f, 0.01f);
	}

	// A slope must be gullied: finite, inside the depth the settings allow, and
	// carrying both cuts and the spurs between them.
	float LowestDisplacement = TNumericLimits<float>::Max();
	float HighestDisplacement = TNumericLimits<float>::Lowest();
	for (int32 Step = 0; Step < 512; ++Step)
	{
		const float PositionX = static_cast<float>(Step) * 7.3f;
		const float Displacement = MassifErosionFilter::EvaluateDisplacementMeters(MakeSlopeInput(PositionX, 250.0f), Settings);
		if (!FMath::IsFinite(Displacement) || FMath::Abs(Displacement) > TotalDepthMeters)
		{
			AddError(FString::Printf(TEXT("Displacement %.2f m out of bounds at x = %.1f"), Displacement, PositionX));
			return false;
		}
		LowestDisplacement = FMath::Min(LowestDisplacement, Displacement);
		HighestDisplacement = FMath::Max(HighestDisplacement, Displacement);
	}
	TestTrue(FString::Printf(TEXT("Slope is cut into gullies (%.1f m deepest)"), LowestDisplacement), LowestDisplacement < -10.0f);
	TestTrue(FString::Printf(TEXT("Slope keeps spurs between them (%.1f m highest)"), HighestDisplacement), HighestDisplacement > 10.0f);

	// No grid and no iteration, so the result must depend on nothing but the
	// point's own inputs.
	const FMassifErosionInput Sample = MakeSlopeInput(913.0f, -77.0f);
	const float FirstEvaluation = MassifErosionFilter::EvaluateDisplacementMeters(Sample, Settings);
	const float SecondEvaluation = MassifErosionFilter::EvaluateDisplacementMeters(Sample, Settings);
	TestEqual(TEXT("Same point evaluates the same"), FirstEvaluation, SecondEvaluation);
	TestNotEqual(TEXT("Different seed cuts a different pattern"),
		FirstEvaluation, MassifErosionFilter::EvaluateDisplacementMeters(Sample, MakeFilterTestSettings(99)));

	// A gully bends the flow the next octave lays its stripes along, and that
	// octave reads its phase from a pivot up to a wavelength away, so too much
	// bend tears the pattern into one-cell cliffs. Sampling at the heightfield's
	// own spacing catches that: 8 m over 2 m is already a 76 degree wall, which
	// no gully flank should reach.
	const float StepMeters = 2.0f;
	float MaxNeighborDelta = 0.0f;
	for (int32 Step = 0; Step < 2048; ++Step)
	{
		const float PositionX = static_cast<float>(Step) * StepMeters;
		const float Here = MassifErosionFilter::EvaluateDisplacementMeters(MakeSlopeInput(PositionX, -1500.0f), Settings);
		const float Next = MassifErosionFilter::EvaluateDisplacementMeters(MakeSlopeInput(PositionX + StepMeters, -1500.0f), Settings);
		MaxNeighborDelta = FMath::Max(MaxNeighborDelta, FMath::Abs(Next - Here));
	}
	TestTrue(FString::Printf(TEXT("Gully flanks stay below a cliff (%.1f m over %.0f m)"), MaxNeighborDelta, StepMeters),
		MaxNeighborDelta < 8.0f);

	return true;
}
