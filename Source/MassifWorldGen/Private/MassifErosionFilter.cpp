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
		double DragMeters = 0.0;
		float FadeTarget = 0.0f;
		float Mask = 1.0f;
		double CutMeters = 0.0;
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
	FVector2d CellPivot(int64 CellX, int64 CellY, double CellSizeMeters, uint32 Seed)
	{
		const FVector2f Jitter = MassifNoise::CellPoint(CellX, CellY, Seed);
		return FVector2d(
			(static_cast<double>(CellX) + Jitter.X) * CellSizeMeters,
			(static_cast<double>(CellY) + Jitter.Y) * CellSizeMeters);
	}

	/**
	 * Direction the stripes are measured along: across the slope, so the gullies
	 * themselves run down it. Ground flat enough to have no direction is masked
	 * out anyway, so any fixed fallback will do.
	 */
	FVector2d AcrossFlowDirection(FVector2d Slope)
	{
		const FVector2d Perpendicular = FVector2d(-Slope.Y, Slope.X).GetSafeNormal();
		if (Perpendicular.IsNearlyZero())
		{
			return FVector2d(1.0, 0.0);
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
		FVector2d PositionMeters, double DragMeters, double WavelengthMeters, double CellSizeMeters,
		uint32 Seed, FMassifMacroSlopeSampler SampleMacroSlope)
	{
		const double CellSpaceX = PositionMeters.X / CellSizeMeters;
		const double CellSpaceY = PositionMeters.Y / CellSizeMeters;
		const int64 LowCellX = static_cast<int64>(FMath::FloorToDouble(CellSpaceX));
		const int64 LowCellY = static_cast<int64>(FMath::FloorToDouble(CellSpaceY));
		const float FadeX = FMath::SmoothStep(0.0f, 1.0f, static_cast<float>(CellSpaceX - static_cast<double>(LowCellX)));
		const float FadeY = FMath::SmoothStep(0.0f, 1.0f, static_cast<float>(CellSpaceY - static_cast<double>(LowCellY)));
		const double RadiansPerMeter = 2.0 * UE_DOUBLE_PI / WavelengthMeters;

		float CosineSum = 0.0f;
		float SineSum = 0.0f;
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			const int32 OffsetX = Corner & 1;
			const int32 OffsetY = Corner >> 1;
			const FVector2d Pivot = CellPivot(LowCellX + OffsetX, LowCellY + OffsetY, CellSizeMeters, Seed);
			const FVector2d AcrossFlow = AcrossFlowDirection(SampleMacroSlope(Pivot));
			const float Weight =
				FMath::Lerp(1.0f - FadeX, FadeX, static_cast<float>(OffsetX)) *
				FMath::Lerp(1.0f - FadeY, FadeY, static_cast<float>(OffsetY));

			// The offset is taken in double before it becomes a phase: the
			// position and the pivot are both large out at the world edge, and
			// their difference is the only small quantity in the expression.
			const double Offset = FVector2d::DotProduct(PositionMeters - Pivot, AcrossFlow) + DragMeters;
			float Sine = 0.0f;
			float Cosine = 0.0f;
			FMath::SinCos(&Sine, &Cosine, static_cast<float>(Offset * RadiansPerMeter));
			CosineSum += Weight * Cosine;
			SineSum += Weight * Sine;
		}

		const float Restore = 1.0f / FMath::Max(0.8f, FMath::Sqrt(CosineSum * CosineSum + SineSum * SineSum));
		return FStripeSample{ CosineSum * Restore, SineSum * Restore };
	}

	void ApplyOctave(
		FErosionState& State, FVector2d PositionMeters, double WavelengthMeters, double DepthMeters,
		float SlopeMask, float OctaveWeight, const FMassifErosionSettings& Settings, uint32 OctaveSeed,
		FMassifMacroSlopeSampler SampleMacroSlope)
	{
		State.Mask = PowerTowardsOne(State.Mask, Settings.DetailPersistence) * SlopeMask;

		const double CellSizeMeters = WavelengthMeters * FMath::Max(0.25, Settings.CellsPerWavelength);
		const FStripeSample Stripes = SampleStripes(
			PositionMeters, State.DragMeters, WavelengthMeters, CellSizeMeters, OctaveSeed, SampleMacroSlope);

		// Ridges sit at +1 and creases at -1, so the profile below cuts nothing
		// on a crest and a full depth in a crease.
		const float Shaped = ApplyRounding(Stripes.Value, Settings.RidgeRounding, Settings.CreaseRounding);
		const float Profile = FMath::Lerp(State.FadeTarget, Shaped, State.Mask * OctaveWeight);
		State.CutMeters += static_cast<double>(Profile - 1.0f) * 0.5 * DepthMeters;
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
		const double DragGain = FMath::Clamp(Settings.WavelengthGain, 0.05, 0.95);
		State.DragMeters = State.DragMeters * DragGain
			+ Stripes.CrossSine * WavelengthMeters * FMath::Max(0.0f, Settings.BranchStrength) * State.Mask * OctaveWeight;
	}
}

double MassifErosionFilter::EvaluateDisplacementMeters(
	const FMassifErosionInput& Input,
	const FMassifErosionSettings& Settings,
	FMassifMacroSlopeSampler SampleMacroSlope)
{
	const float ReliefFraction = FMath::Clamp(Input.ReliefFraction, 0.0f, 1.0f);
	const float SlopeMask = EaseOut(static_cast<float>(Input.SlopeMetersPerMeter.Length()) / FMath::Max(UE_SMALL_NUMBER, Settings.SlopeReference));

	FErosionState State;
	State.FadeTarget = ReliefFraction * 2.0f - 1.0f;

	double WavelengthMeters = Settings.LargestGullyWavelengthMeters;
	double DepthMeters = Settings.LargestGullyDepthMeters;
	double TotalDepthMeters = 0.0;
	for (int32 Octave = 0; Octave < Settings.OctaveCount; ++Octave)
	{
		float OctaveWeight = 1.0f;
		if (Octave == Settings.OctaveCount - 1)
		{
			OctaveWeight = FMath::Clamp(Settings.FinestOctaveWeight, 0.0f, 1.0f);
		}

		const uint32 OctaveSeed = Settings.Seed + static_cast<uint32>(Octave) * 0x9E3779B9u;
		ApplyOctave(State, Input.PositionMeters, WavelengthMeters, DepthMeters, SlopeMask, OctaveWeight, Settings, OctaveSeed, SampleMacroSlope);

		// The full depth, not the weighted one: a faded octave still carries the
		// fade target through at full depth, and only its deviation from that
		// target is weighted. Counting the weighted depth here would leave flat
		// ground displaced.
		TotalDepthMeters += DepthMeters;
		WavelengthMeters *= FMath::Clamp(Settings.WavelengthGain, 0.05, 0.95);
		DepthMeters *= FMath::Clamp(Settings.DepthGain, 0.0, 1.0);
	}

	// What a point with no gullies at all would have been cut by: the fade
	// target alone, held over every octave.
	const double UngulliedCutMeters = (static_cast<double>(ReliefFraction) - 1.0) * TotalDepthMeters;
	return State.CutMeters - UngulliedCutMeters;
}
