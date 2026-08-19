#include "MassifErosionFilter.h"

#include "MassifNoise.h"

namespace
{
	/** What one octave of stripes reads at a point: height in [-1, 1] and the sine that drags the next octave. */
	struct FStripeSample
	{
		float Value = 0.0f;
		float CrossSine = 0.0f;
	};

	/** Carried from octave to octave: the stripes each one lays depend on what the ones before it cut. */
	struct FErosionState
	{
		/** Sideways displacement the coarser gullies have dragged this point by. */
		float DragMeters = 0.0f;
		float FadeTarget = 0.0f;
		float Mask = 1.0f;
		float CutMeters = 0.0f;
	};

	float EaseOut(float Value)
	{
		const float Remaining = 1.0f - FMath::Clamp(Value, 0.0f, 1.0f);
		return 1.0f - Remaining * Remaining;
	}

	/** Pulls Value towards 1 the larger Power is, without ever passing it. */
	float PowerTowardsOne(float Value, float Power)
	{
		return 1.0f - FMath::Pow(1.0f - FMath::Clamp(Value, 0.0f, 1.0f), Power);
	}

	/**
	 * Cosine stripes are equally round at crest and trough. Blending towards the
	 * triangle wave through the same zero crossings pinches an end into a
	 * straight-sided edge, which is what a water-cut crease looks like.
	 * asin(cos(Phase)) is linear in Phase, so it is that triangle wave exactly.
	 */
	float ApplyRounding(float StripeValue, float RidgeRounding, float CreaseRounding)
	{
		const float Clamped = FMath::Clamp(StripeValue, -1.0f, 1.0f);
		const float Triangle = FMath::FastAsin(Clamped) * (2.0f / UE_PI);
		if (Clamped >= 0.0f)
		{
			return FMath::Lerp(Triangle, Clamped, FMath::Clamp(RidgeRounding, 0.0f, 1.0f));
		}
		return FMath::Lerp(Triangle, Clamped, FMath::Clamp(CreaseRounding, 0.0f, 1.0f));
	}

	/** Stripe phase origin for one cell, jittered so neighbouring cells never line up. */
	FVector2f CellPivot(int32 CellX, int32 CellY, float CellSizeMeters, uint32 Seed)
	{
		const FVector2f Jitter = MassifNoise::CellPoint(CellX, CellY, Seed);
		return FVector2f(
			(static_cast<float>(CellX) + Jitter.X) * CellSizeMeters,
			(static_cast<float>(CellY) + Jitter.Y) * CellSizeMeters);
	}

	/**
	 * Direction the stripes are measured along: across the slope, so the gullies
	 * themselves run down it. Ground flat enough to have no direction is masked
	 * out anyway, so any fixed fallback will do.
	 */
	FVector2f AcrossFlowDirection(FVector2f Slope)
	{
		const FVector2f Perpendicular = FVector2f(-Slope.Y, Slope.X).GetSafeNormal();
		if (Perpendicular.IsNearlyZero())
		{
			return FVector2f(1.0f, 0.0f);
		}
		return Perpendicular;
	}

	/**
	 * The four cells around the point each lay a stripe pattern of the same
	 * wavelength, at their own phase and along their own pivot's flow direction,
	 * and the four are blended. Two sine waves out of step blend into one weaker
	 * sine rather than into mush, so the result stays a clean stripe pattern.
	 *
	 * The blend is only partly normalised back to full depth. Forcing it all the
	 * way makes every gully exactly as deep as every other, which over several
	 * octaves reads as woven fabric rather than ground; leaving some of the
	 * variation in lets gullies fade in and out along their length.
	 */
	FStripeSample SampleStripes(
		FVector2f PositionMeters, float DragMeters, float WavelengthMeters, float CellSizeMeters,
		uint32 Seed, FMassifMacroSlopeSampler SampleMacroSlope)
	{
		const float CellSpaceX = PositionMeters.X / CellSizeMeters;
		const float CellSpaceY = PositionMeters.Y / CellSizeMeters;
		const int32 LowCellX = FMath::FloorToInt32(CellSpaceX);
		const int32 LowCellY = FMath::FloorToInt32(CellSpaceY);
		const float FadeX = FMath::SmoothStep(0.0f, 1.0f, CellSpaceX - static_cast<float>(LowCellX));
		const float FadeY = FMath::SmoothStep(0.0f, 1.0f, CellSpaceY - static_cast<float>(LowCellY));
		const float RadiansPerMeter = 2.0f * UE_PI / WavelengthMeters;

		float CosineSum = 0.0f;
		float SineSum = 0.0f;
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			const int32 OffsetX = Corner & 1;
			const int32 OffsetY = Corner >> 1;
			const FVector2f Pivot = CellPivot(LowCellX + OffsetX, LowCellY + OffsetY, CellSizeMeters, Seed);
			const FVector2f AcrossFlow = AcrossFlowDirection(SampleMacroSlope(Pivot));
			const float Weight =
				FMath::Lerp(1.0f - FadeX, FadeX, static_cast<float>(OffsetX)) *
				FMath::Lerp(1.0f - FadeY, FadeY, static_cast<float>(OffsetY));

			float Sine = 0.0f;
			float Cosine = 0.0f;
			FMath::SinCos(&Sine, &Cosine, (FVector2f::DotProduct(PositionMeters - Pivot, AcrossFlow) + DragMeters) * RadiansPerMeter);
			CosineSum += Weight * Cosine;
			SineSum += Weight * Sine;
		}

		const float Restore = 1.0f / FMath::Max(0.8f, FMath::Sqrt(CosineSum * CosineSum + SineSum * SineSum));
		return FStripeSample{ CosineSum * Restore, SineSum * Restore };
	}

	void ApplyOctave(
		FErosionState& State, FVector2f PositionMeters, float WavelengthMeters, float DepthMeters,
		float SlopeMask, const FMassifErosionSettings& Settings, uint32 OctaveSeed, FMassifMacroSlopeSampler SampleMacroSlope)
	{
		State.Mask = PowerTowardsOne(State.Mask, Settings.DetailPersistence) * SlopeMask;

		const float CellSizeMeters = WavelengthMeters * FMath::Max(0.25f, Settings.CellsPerWavelength);
		const FStripeSample Stripes = SampleStripes(
			PositionMeters, State.DragMeters, WavelengthMeters, CellSizeMeters, OctaveSeed, SampleMacroSlope);

		// Ridges sit at +1 and creases at -1, so the profile below cuts nothing
		// on a crest and a full depth in a crease.
		const float Profile = FMath::Lerp(State.FadeTarget, ApplyRounding(Stripes.Value, Settings.RidgeRounding, Settings.CreaseRounding), State.Mask);
		State.CutMeters += (Profile - 1.0f) * 0.5f * DepthMeters;
		State.FadeTarget = Profile;

		// Dragging the finer gullies sideways bends them into this one. Doing it
		// by displacement rather than by turning their stripes keeps the phase
		// linear in position, so no amount of drag can tear the pattern.
		//
		// The drag already carried is shrunk by the same factor as the
		// wavelength, so what each octave hands on stays a fixed fraction of the
		// wavelength it is handing it to. Accumulating it undiminished instead
		// leaves the finest octave dragged by several of its own wavelengths,
		// which stirs it into swirls rather than bending it.
		const float DragGain = FMath::Clamp(Settings.WavelengthGain, 0.05f, 0.95f);
		State.DragMeters = State.DragMeters * DragGain
			+ Stripes.CrossSine * WavelengthMeters * FMath::Max(0.0f, Settings.BranchStrength) * State.Mask;
	}
}

float MassifErosionFilter::EvaluateDisplacementMeters(
	const FMassifErosionInput& Input,
	const FMassifErosionSettings& Settings,
	FMassifMacroSlopeSampler SampleMacroSlope)
{
	const float ReliefFraction = FMath::Clamp(Input.ReliefFraction, 0.0f, 1.0f);
	const float SlopeMask = EaseOut(Input.SlopeMetersPerMeter.Size() / FMath::Max(UE_SMALL_NUMBER, Settings.SlopeReference));

	FErosionState State;
	State.FadeTarget = ReliefFraction * 2.0f - 1.0f;

	float WavelengthMeters = Settings.LargestGullyWavelengthMeters;
	float DepthMeters = Settings.LargestGullyDepthMeters;
	float TotalDepthMeters = 0.0f;
	for (int32 Octave = 0; Octave < Settings.OctaveCount; ++Octave)
	{
		const uint32 OctaveSeed = Settings.Seed + static_cast<uint32>(Octave) * 0x9E3779B9u;
		ApplyOctave(State, Input.PositionMeters, WavelengthMeters, DepthMeters, SlopeMask, Settings, OctaveSeed, SampleMacroSlope);
		TotalDepthMeters += DepthMeters;
		WavelengthMeters *= FMath::Clamp(Settings.WavelengthGain, 0.05f, 0.95f);
		DepthMeters *= FMath::Clamp(Settings.DepthGain, 0.0f, 1.0f);
	}

	// What a point with no gullies at all would have been cut by: the fade
	// target alone, held over every octave.
	const float UngulliedCutMeters = (ReliefFraction - 1.0f) * TotalDepthMeters;
	return State.CutMeters - UngulliedCutMeters;
}
