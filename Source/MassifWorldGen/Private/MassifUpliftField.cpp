#include "MassifUpliftField.h"

#include "MassifNoise.h"
#include "MassifSeed.h"
#include "WorldGenConfig.h"

FMassifUpliftField::FMassifUpliftField(const FWorldGenConfig& InConfig, uint32 InSeed)
	: Config(InConfig)
{
	const FMassifSeed Seed(InSeed);
	RangeSeed = Seed.MakeChannelSeed(TEXT("WorldGen.Uplift.Range"));
	PeakSeed = Seed.MakeChannelSeed(TEXT("WorldGen.Uplift.Peaks"));
	WarpSeedX = Seed.MakeChannelSeed(TEXT("WorldGen.Uplift.WarpX"));
	WarpSeedY = Seed.MakeChannelSeed(TEXT("WorldGen.Uplift.WarpY"));
}

/** Ranges built on unwarped noise run in straight bands; the warp gives them bends and spurs. */
FVector2d FMassifUpliftField::WarpedPosition(FVector2d PositionMeters) const
{
	const double WarpCycles = 1.0 / FMath::Max(1.0f, Config.RangeWarpWavelengthMeters);
	const float WarpX = MassifNoise::Fractal(
		PositionMeters.X * WarpCycles, PositionMeters.Y * WarpCycles, WarpSeedX, 3, 2.0, 0.5f);
	const float WarpY = MassifNoise::Fractal(
		PositionMeters.X * WarpCycles, PositionMeters.Y * WarpCycles, WarpSeedY, 3, 2.0, 0.5f);

	return PositionMeters + FVector2d(WarpX, WarpY) * Config.RangeWarpMeters;
}

FMassifUpliftSample FMassifUpliftField::Sample(FVector2d PositionMeters) const
{
	const FVector2d Warped = WarpedPosition(PositionMeters);
	const double RangeCycles = 1.0 / FMath::Max(1.0f, Config.RangeSpacingMeters);

	const float Ridged = MassifNoise::RidgedFractal(
		Warped.X * RangeCycles, Warped.Y * RangeCycles,
		RangeSeed, Config.RangeOctaveCount, 2.0, 0.5f, Config.RangeSharpness);

	FMassifUpliftSample Result;
	Result.RangeFraction = FMath::SmoothStep(0.0f, 1.0f, Ridged);
	Result.CrestAltitudeMeters = FMath::Lerp(
		static_cast<double>(Config.BasinCrestAltitudeMeters),
		static_cast<double>(Config.PeakCrestAltitudeMeters),
		static_cast<double>(Result.RangeFraction));

	// Peak-scale structure on top of the range envelope. The range noise is
	// smoothstepped, so its own fine octaves die out exactly on the crests;
	// without this term every summit region tops off flat at the crest altitude.
	const double PeakCycles = 1.0 / FMath::Max(1.0f, Config.PeakWavelengthMeters);
	const float PeakNoise = MassifNoise::RidgedFractal(
		Warped.X * PeakCycles, Warped.Y * PeakCycles, PeakSeed, 2, 2.0, 0.5f, 1.8f);
	Result.CrestAltitudeMeters +=
		(static_cast<double>(PeakNoise) - 0.5) * 2.0 * Config.PeakReliefMeters * Result.RangeFraction;
	return Result;
}
