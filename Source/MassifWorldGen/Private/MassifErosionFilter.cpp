// Translation of the Advanced Terrain Erosion Filter and Phacelle Noise,
// copyright (c) 2025 Rune Skovbo Johansen, via the C# conversion by Luke
// Mitchell (2026). Adapted to double-precision world positions and this
// project's integer-cell hashing; the octave loop follows the reference.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "MassifErosionFilter.h"

#include "MassifNoise.h"

namespace
{
	/**
	 * Below this share of full magnitude the interpolated waves are not pushed
	 * back up to 1: fully normalising near-cancelled blends whips them into
	 * loops. The reference's normalization parameter.
	 */
	const float StripeNormalization = 0.5f;

	/**
	 * How fast an octave's own stripe slope saturates its mask. The stripe sine
	 * is 0 on crests and creases, so this is what keeps the finer gullies off
	 * the ridge lines the coarser ones made.
	 */
	const float OctaveSlopeOnset = 2.0f;

	struct FStripeSample
	{
		/** Stripe height in [-1, 1]: 1 on the spur crest, -1 in the gully floor. */
		float Cosine = 0.0f;
		/** Same wave 90 degrees ahead; its sign says which side of the crest the point is on. */
		float Sine = 0.0f;
	};

	float Clamp01(float Value)
	{
		return FMath::Clamp(Value, 0.0f, 1.0f);
	}

	float EaseOut(float Value)
	{
		const float Remaining = 1.0f - Clamp01(Value);
		return 1.0f - Remaining * Remaining;
	}

	/** Pulls Value towards 1 the larger Power is, without ever passing it. */
	float PowerTowardsOne(float Value, float Power)
	{
		return 1.0f - FMath::Pow(1.0f - Clamp01(Value), Power);
	}

	/** Linear ramp whose foot is rounded off over Smoothing, so masks ease in instead of snapping on. */
	float SmoothStart(float Value, float Smoothing)
	{
		if (Smoothing <= UE_SMALL_NUMBER || Value >= Smoothing)
		{
			return Value - 0.5f * FMath::Max(0.0f, Smoothing);
		}
		return 0.5f * Value * Value / Smoothing;
	}

	/**
	 * Phacelle noise: cosine stripes across the downhill direction, phased per
	 * cell and blended over the 4x4 cells around the point with a bell falloff.
	 * Two waves out of step blend into one weaker wave rather than into mush,
	 * and the magnitude is restored towards 1 afterwards, so the result stays a
	 * clean stripe pattern with a well-defined sine.
	 */
	FStripeSample PhacelleNoise(
		FVector2d PositionMeters, FVector2d DownhillDirection, double WavelengthMeters, double CellSizeMeters,
		uint32 Seed)
	{
		const FVector2d AcrossDirection(-DownhillDirection.Y, DownhillDirection.X);
		const double RadiansPerMeter = 2.0 * UE_DOUBLE_PI / WavelengthMeters;
		const double CellSpaceX = PositionMeters.X / CellSizeMeters;
		const double CellSpaceY = PositionMeters.Y / CellSizeMeters;
		const int64 BaseCellX = static_cast<int64>(FMath::FloorToDouble(CellSpaceX));
		const int64 BaseCellY = static_cast<int64>(FMath::FloorToDouble(CellSpaceY));

		float CosineSum = 0.0f;
		float SineSum = 0.0f;
		float WeightSum = 0.0f;
		for (int32 Neighbor = 0; Neighbor < 16; ++Neighbor)
		{
			const int64 CellX = BaseCellX - 1 + (Neighbor & 3);
			const int64 CellY = BaseCellY - 1 + (Neighbor >> 2);

			// Cell point: the lattice point jittered by up to half a cell.
			const FVector2f Jitter = MassifNoise::CellPoint(CellX, CellY, Seed);
			const FVector2d CellPointMeters(
				(static_cast<double>(CellX) + Jitter.X - 0.5) * CellSizeMeters,
				(static_cast<double>(CellY) + Jitter.Y - 0.5) * CellSizeMeters);

			// The difference is the only small quantity out at the world edge,
			// so it is taken in double before anything becomes a float phase.
			const FVector2d FromCellPointMeters = PositionMeters - CellPointMeters;

			// Bell weight over distance in cell units: 1 at the cell point,
			// zero by 1.5 cells, which is the closest a cell point outside the
			// 4x4 block can be. The small bias makes the tail exactly zero.
			const double DistanceCells = FromCellPointMeters.SquaredLength() / (CellSizeMeters * CellSizeMeters);
			const float Weight = FMath::Max(0.0f, FMath::Exp(static_cast<float>(-2.0 * DistanceCells)) - 0.01111f);
			if (Weight <= 0.0f)
			{
				continue;
			}

			const double PhaseRadians = FVector2d::DotProduct(FromCellPointMeters, AcrossDirection) * RadiansPerMeter;
			float Sine = 0.0f;
			float Cosine = 0.0f;
			FMath::SinCos(&Sine, &Cosine, static_cast<float>(PhaseRadians));
			CosineSum += Weight * Cosine;
			SineSum += Weight * Sine;
			WeightSum += Weight;
		}

		if (WeightSum <= UE_SMALL_NUMBER)
		{
			return FStripeSample{};
		}
		const float InterpolatedCosine = CosineSum / WeightSum;
		const float InterpolatedSine = SineSum / WeightSum;
		const float Magnitude = FMath::Max(
			1.0f - StripeNormalization,
			FMath::Sqrt(InterpolatedCosine * InterpolatedCosine + InterpolatedSine * InterpolatedSine));
		return FStripeSample{ InterpolatedCosine / Magnitude, InterpolatedSine / Magnitude };
	}
}

double MassifErosionFilter::EvaluateDisplacementMeters(const FMassifErosionInput& Input, const FMassifErosionSettings& Settings)
{
	const float InitialFadeTarget = Clamp01(Input.ReliefFraction) * 2.0f - 1.0f;
	float FadeTarget = InitialFadeTarget;

	// The input mask decides where the coarsest gullies may cut at all, from
	// the base slope; each octave then multiplies in a mask from its own
	// stripes, so finer gullies keep off the crests the coarser ones made.
	const float InputSlopeOnset = 1.0f / FMath::Max(UE_SMALL_NUMBER, Settings.SlopeReference);
	const float RoundingForInput = FMath::Lerp(
		Settings.CreaseRounding, Settings.RidgeRounding, Clamp01(FadeTarget + 0.5f));
	const float InputSlope = static_cast<float>(Input.SlopeMetersPerMeter.Length());
	float CombinedMask = EaseOut(SmoothStart(InputSlope * InputSlopeOnset, RoundingForInput * InputSlopeOnset));

	// The gradient the gullies orient by. Every octave adds its own stripes'
	// steepest slope, so the finer gullies bend into the coarser ones and
	// branch off them the way a drainage network does.
	FVector2d GullySlope = Input.SlopeMetersPerMeter;

	double DisplacementMeters = 0.0;
	double TotalAmplitudeMeters = 0.0;
	double WavelengthMeters = Settings.LargestGullyWavelengthMeters;
	double DepthMeters = Settings.LargestGullyDepthMeters;
	for (int32 Octave = 0; Octave < Settings.OctaveCount; ++Octave)
	{
		float OctaveWeight = 1.0f;
		if (Octave == Settings.OctaveCount - 1)
		{
			OctaveWeight = Clamp01(Settings.FinestOctaveWeight);
		}

		const double AmplitudeMeters = DepthMeters * 0.5;
		const double CellSizeMeters = WavelengthMeters * FMath::Max(0.25, Settings.CellsPerWavelength);
		const uint32 OctaveSeed = Settings.Seed + static_cast<uint32>(Octave) * 0x9E3779B9u;
		// Ground flat enough to have no direction is masked out anyway, so any
		// fixed fallback will do.
		FVector2d DownhillDirection = GullySlope.GetSafeNormal();
		if (DownhillDirection.IsNearlyZero())
		{
			DownhillDirection = FVector2d(1.0, 0.0);
		}
		const FStripeSample Stripes = PhacelleNoise(
			Input.PositionMeters, DownhillDirection, WavelengthMeters, CellSizeMeters, OctaveSeed);

		// Steer the finer octaves with this octave's steepest stripe slope —
		// the sign of the sine, not its value, so the pull is as strong
		// everywhere as on the wave's steepest part.
		const FVector2d AcrossDirection(-DownhillDirection.Y, DownhillDirection.X);
		const double StripeSlope = 2.0 * UE_DOUBLE_PI * AmplitudeMeters / WavelengthMeters;
		GullySlope -= AcrossDirection * (FMath::Sign(Stripes.Sine) * StripeSlope * Settings.BranchStrength);

		// Fade the stripes towards the fade target where the mask says the
		// ground is too flat, then hand the result down as the next target, so
		// a faded octave still carries the coarser profile through unchanged.
		const float Gullies = Stripes.Cosine * Settings.BranchStrength;
		const float Faded = FMath::Lerp(FadeTarget, Gullies, CombinedMask * OctaveWeight);
		DisplacementMeters += static_cast<double>(Faded) * AmplitudeMeters;
		TotalAmplitudeMeters += AmplitudeMeters;
		FadeTarget = Faded;

		const float RoundingForOctave = FMath::Lerp(
			Settings.CreaseRounding, Settings.RidgeRounding, Clamp01(Stripes.Cosine + 0.5f));
		const float OctaveMask = EaseOut(SmoothStart(
			FMath::Abs(Stripes.Sine) * OctaveSlopeOnset, RoundingForOctave * OctaveSlopeOnset));
		CombinedMask = PowerTowardsOne(CombinedMask, Settings.DetailPersistence) * OctaveMask;

		WavelengthMeters *= FMath::Clamp(Settings.WavelengthGain, 0.05, 0.95);
		DepthMeters *= FMath::Clamp(Settings.DepthGain, 0.0, 1.0);
	}

	// What a point with no gullies at all would have been displaced by: the
	// initial fade target alone, held over every octave.
	return DisplacementMeters - static_cast<double>(InitialFadeTarget) * TotalAmplitudeMeters;
}
