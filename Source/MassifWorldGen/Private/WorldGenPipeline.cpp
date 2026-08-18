#include "WorldGenPipeline.h"

#include "MassifNoise.h"
#include "MassifSeed.h"

namespace
{
	struct FHeightfieldSampler
	{
		FWorldGenConfig Config;
		uint32 UpliftSeed = 0;
		uint32 RidgeSeed = 0;
		uint32 DetailSeed = 0;
		uint32 WarpSeedX = 0;
		uint32 WarpSeedY = 0;

		explicit FHeightfieldSampler(const FWorldGenConfig& InConfig)
			: Config(InConfig)
		{
			const FMassifSeed Seed(static_cast<uint32>(InConfig.MasterSeed));
			UpliftSeed = Seed.MakeChannelSeed(TEXT("WorldGen.Uplift"));
			RidgeSeed = Seed.MakeChannelSeed(TEXT("WorldGen.Ridge"));
			DetailSeed = Seed.MakeChannelSeed(TEXT("WorldGen.Detail"));
			WarpSeedX = Seed.MakeChannelSeed(TEXT("WorldGen.WarpX"));
			WarpSeedY = Seed.MakeChannelSeed(TEXT("WorldGen.WarpY"));
		}

		/** UnitX/UnitY in [0, 1] across the map; returns altitude in meters. */
		float SampleAltitude(float UnitX, float UnitY) const
		{
			const float WarpX = MassifNoise::Fractal(UnitX * 3.0f, UnitY * 3.0f, WarpSeedX, 3, 2.0f, 0.5f);
			const float WarpY = MassifNoise::Fractal(UnitX * 3.0f, UnitY * 3.0f, WarpSeedY, 3, 2.0f, 0.5f);
			const float WarpedX = UnitX + WarpX * Config.WarpStrength;
			const float WarpedY = UnitY + WarpY * Config.WarpStrength;

			const float UpliftNoise = MassifNoise::Fractal(
				WarpedX * Config.UpliftFrequency, WarpedY * Config.UpliftFrequency, UpliftSeed, 3, 2.0f, 0.5f);
			const float Uplift = FMath::Clamp(0.5f + 0.5f * UpliftNoise, 0.0f, 1.0f);

			const float Ridged = MassifNoise::RidgedFractal(
				WarpedX * Config.RidgeFrequency, WarpedY * Config.RidgeFrequency,
				RidgeSeed, Config.RidgeOctaveCount, 2.0f, 0.5f, Config.RidgeSharpness);

			const float Relief = FMath::Clamp(Uplift * (0.30f + 0.70f * Ridged), 0.0f, 1.0f);
			// Smoothstep widens valley floors and rounds summits while keeping mid-slope contrast.
			const float Shaped = Relief * Relief * (3.0f - 2.0f * Relief) * EdgeFalloff(UnitX, UnitY);

			const float DetailNoise = MassifNoise::Fractal(
				WarpedX * Config.DetailFrequency, WarpedY * Config.DetailFrequency, DetailSeed, 5, 2.0f, 0.5f);
			const float DetailMeters = DetailNoise * Config.DetailAmplitudeMeters * (0.6f + 0.4f * Shaped);

			const float BaseAltitude = Config.MinAltitudeMeters + Shaped * (Config.MaxAltitudeMeters - Config.MinAltitudeMeters);
			return FMath::Clamp(BaseAltitude + DetailMeters, Config.MinAltitudeMeters, Config.MaxAltitudeMeters);
		}

		float EdgeFalloff(float UnitX, float UnitY) const
		{
			const float BorderDistance = FMath::Min(
				FMath::Min(UnitX, 1.0f - UnitX), FMath::Min(UnitY, 1.0f - UnitY));
			const float Falloff = FMath::Clamp(BorderDistance / Config.EdgeFalloffFraction, 0.0f, 1.0f);
			return FMath::SmoothStep(0.0f, 1.0f, Falloff);
		}
	};
}

FWorldGenResult FWorldGenPipeline::Generate(const FWorldGenConfig& Config)
{
	check(Config.Resolution > 1);

	FWorldGenResult Result;
	Result.Resolution = Config.Resolution;
	Result.WorldSizeMeters = Config.WorldSizeMeters;
	Result.MetersPerPixel = Config.WorldSizeMeters / static_cast<float>(Config.Resolution - 1);
	Result.HeightMeters.SetNumUninitialized(Config.Resolution * Config.Resolution);

	const FHeightfieldSampler Sampler(Config);
	const float UnitStep = 1.0f / static_cast<float>(Config.Resolution - 1);
	float* Heights = Result.HeightMeters.GetData();

	ParallelFor(Config.Resolution, [&Sampler, Heights, UnitStep, &Config](int32 PixelY)
	{
		const float UnitY = static_cast<float>(PixelY) * UnitStep;
		float* Row = Heights + PixelY * Config.Resolution;
		for (int32 PixelX = 0; PixelX < Config.Resolution; ++PixelX)
		{
			Row[PixelX] = Sampler.SampleAltitude(static_cast<float>(PixelX) * UnitStep, UnitY);
		}
	});

	return Result;
}
