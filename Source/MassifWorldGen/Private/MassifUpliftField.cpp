#include "MassifUpliftField.h"

#include "MassifNoise.h"
#include "MassifSeed.h"
#include "WorldGenConfig.h"

FMassifUpliftField::FMassifUpliftField(const FWorldGenConfig& InConfig, uint32 InSeed)
	: Config(InConfig)
{
	const FMassifSeed Seed(InSeed);
	RangeSeed = Seed.MakeChannelSeed(TEXT("WorldGen.Uplift.Range"));
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
	return Result;
}

double FMassifUpliftField::MaxWaterAltitudeMeters(FVector2d PositionMeters) const
{
	const FMassifUpliftSample Uplift = Sample(PositionMeters);
	return Uplift.CrestAltitudeMeters - Config.MinReliefAboveWaterMeters;
}
