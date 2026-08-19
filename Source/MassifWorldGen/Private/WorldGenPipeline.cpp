#include "WorldGenPipeline.h"

#include "GpuErosion.h"
#include "MassifNoise.h"
#include "MassifSeed.h"

namespace
{
	struct FHeightfieldSampler
	{
		FWorldGenConfig Config;
		uint32 UpliftSeed = 0;
		uint32 RegionSeed = 0;
		uint32 ValleySeed = 0;
		uint32 RidgeSeed = 0;
		uint32 DetailSeed = 0;
		uint32 WarpSeedX = 0;
		uint32 WarpSeedY = 0;

		explicit FHeightfieldSampler(const FWorldGenConfig& InConfig)
			: Config(InConfig)
		{
			const FMassifSeed Seed(static_cast<uint32>(InConfig.MasterSeed));
			UpliftSeed = Seed.MakeChannelSeed(TEXT("WorldGen.Uplift"));
			RegionSeed = Seed.MakeChannelSeed(TEXT("WorldGen.Region"));
			ValleySeed = Seed.MakeChannelSeed(TEXT("WorldGen.Valley"));
			RidgeSeed = Seed.MakeChannelSeed(TEXT("WorldGen.Ridge"));
			DetailSeed = Seed.MakeChannelSeed(TEXT("WorldGen.Detail"));
			WarpSeedX = Seed.MakeChannelSeed(TEXT("WorldGen.WarpX"));
			WarpSeedY = Seed.MakeChannelSeed(TEXT("WorldGen.WarpY"));
		}

		/** UnitX/UnitY in [0, 1] across the map; returns altitude in meters. */
		float SampleAltitude(float UnitX, float UnitY) const
		{
			const FVector2f Warped = DomainWarp(UnitX, UnitY);

			const float Ridged = MassifNoise::RidgedFractal(
				Warped.X * Config.RidgeFrequency, Warped.Y * Config.RidgeFrequency,
				RidgeSeed, Config.RidgeOctaveCount, 2.0f, 0.5f, Config.RidgeSharpness);

			const float ValleyFloor = 1.0f - FMath::Clamp(Config.ValleyDepth, 0.0f, 1.0f);
			const float Relief = MassifMask(Warped) * (ValleyFloor + (1.0f - ValleyFloor) * Ridged) * ValleyNetwork(Warped);
			// Smoothstep widens valley floors and rounds summits while keeping mid-slope contrast.
			const float Rounded = Relief * Relief * (3.0f - 2.0f * Relief);
			// Above 1 the profile leaves the lower slopes gentle and steepens with altitude.
			const float Shaped = FMath::Pow(Rounded, Config.SlopeProfileExponent) * EdgeFalloff(UnitX, UnitY);

			return AltitudeWithDetail(Shaped, Warped);
		}

		/**
		 * Multiplier in [0, 1] that cuts the main valleys. The crest lines of a
		 * ridged field are continuous curves, so using them as the carve pattern
		 * gives valleys that thread through the range instead of isolated hollows.
		 */
		float ValleyNetwork(const FVector2f& Warped) const
		{
			const float Threads = MassifNoise::RidgedFractal(
				Warped.X * Config.ValleyNetworkFrequency, Warped.Y * Config.ValleyNetworkFrequency,
				ValleySeed, 3, 2.0f, 0.5f, Config.ValleyNetworkSharpness);
			return FMath::Clamp(1.0f - Config.ValleyNetworkStrength * Threads, 0.0f, 1.0f);
		}

		FVector2f DomainWarp(float UnitX, float UnitY) const
		{
			const float WarpX = MassifNoise::Fractal(UnitX * 3.0f, UnitY * 3.0f, WarpSeedX, 3, 2.0f, 0.5f);
			const float WarpY = MassifNoise::Fractal(UnitX * 3.0f, UnitY * 3.0f, WarpSeedY, 3, 2.0f, 0.5f);
			return FVector2f(UnitX + WarpX * Config.WarpStrength, UnitY + WarpY * Config.WarpStrength);
		}

		/**
		 * Where the range stands above the valley floor, in [0, 1]. Ridged uplift
		 * sampled in an elongated frame gives several parallel chains rather than
		 * one dome; the threshold cuts the ground between them down to zero so
		 * through-valleys separate the massifs.
		 */
		float MassifMask(const FVector2f& Warped) const
		{
			const FVector2f Frame = RangeFrame(Warped);
			const float Chain = MassifNoise::RidgedFractal(
				Frame.X * Config.UpliftFrequency, Frame.Y * Config.UpliftFrequency, UpliftSeed, 2, 2.0f, 0.5f, 1.0f);
			const float AboveThreshold = FMath::Clamp(
				(Chain - Config.MassifThreshold) / FMath::Max(0.05f, 1.0f - Config.MassifThreshold), 0.0f, 1.0f);

			const float RegionNoise = MassifNoise::Fractal(
				Warped.X * Config.RegionFrequency, Warped.Y * Config.RegionFrequency, RegionSeed, 2, 2.0f, 0.5f);
			const float Region = 1.0f - Config.RegionContrast * (0.5f - 0.5f * RegionNoise);
			return AboveThreshold * Region;
		}

		/** Rotates into the range's own axes and compresses along it, so features run as chains. */
		FVector2f RangeFrame(const FVector2f& Warped) const
		{
			const float AxisRadians = FMath::DegreesToRadians(Config.RangeAxisDegrees);
			const float CosAxis = FMath::Cos(AxisRadians);
			const float SinAxis = FMath::Sin(AxisRadians);
			const float AlongAxis = Warped.X * CosAxis + Warped.Y * SinAxis;
			const float AcrossAxis = Warped.Y * CosAxis - Warped.X * SinAxis;
			return FVector2f(AlongAxis * Config.RangeElongation, AcrossAxis);
		}

		float AltitudeWithDetail(float Shaped, const FVector2f& Warped) const
		{
			const float DetailNoise = MassifNoise::Fractal(
				Warped.X * Config.DetailFrequency, Warped.Y * Config.DetailFrequency, DetailSeed, 5, 2.0f, 0.5f);
			// Detail fades out towards the valley floors, so low ground stays smooth and only the tops break up.
			const float DetailMeters = DetailNoise * Config.DetailAmplitudeMeters * (0.15f + 0.85f * Shaped * Shaped);

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

	bool IsFarFromOtherSources(const TArray<FMassifRiverSource>& Sources, int32 CellX, int32 CellY, int32 MinSeparationCells)
	{
		const int32 MinSeparationSquared = MinSeparationCells * MinSeparationCells;
		for (const FMassifRiverSource& Source : Sources)
		{
			const int32 DeltaX = Source.CellX - CellX;
			const int32 DeltaY = Source.CellY - CellY;
			if (DeltaX * DeltaX + DeltaY * DeltaY < MinSeparationSquared)
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Springs feeding the rivers. They sit in the upper slopes rather than on the
	 * crests, are spaced out so their discharge is not wasted on one valley, and
	 * carry far more water than rainfall so they cut real channels.
	 */
	TArray<FMassifRiverSource> PickRiverSources(const FWorldGenConfig& Config, const TArray<float>& HeightMeters)
	{
		TArray<FMassifRiverSource> Sources;
		if (Config.RiverSourceCount <= 0 || Config.RiverSourceRate <= 0.0f)
		{
			return Sources;
		}

		// Measured against the relief actually generated, not the configured
		// limits, which the shaping curves rarely reach.
		float LowestHeight = TNumericLimits<float>::Max();
		float HighestHeight = TNumericLimits<float>::Lowest();
		for (const float Height : HeightMeters)
		{
			LowestHeight = FMath::Min(LowestHeight, Height);
			HighestHeight = FMath::Max(HighestHeight, Height);
		}

		const float AltitudeRange = HighestHeight - LowestHeight;
		const float LowAltitude = LowestHeight + Config.RiverSourceMinAltitudeFraction * AltitudeRange;
		const float HighAltitude = LowestHeight + Config.RiverSourceMaxAltitudeFraction * AltitudeRange;
		const int32 MinSeparationCells = Config.Resolution / FMath::Max(4, Config.RiverSourceCount / 2);
		const int32 MaxAttempts = Config.RiverSourceCount * 512;

		FRandomStream Stream = FMassifSeed(static_cast<uint32>(Config.MasterSeed)).MakeChannelStream(TEXT("WorldGen.RiverSource"));
		Sources.Reserve(Config.RiverSourceCount);
		for (int32 Attempt = 0; Attempt < MaxAttempts && Sources.Num() < Config.RiverSourceCount; ++Attempt)
		{
			const int32 CellX = Stream.RandRange(0, Config.Resolution - 1);
			const int32 CellY = Stream.RandRange(0, Config.Resolution - 1);
			const float Height = HeightMeters[CellY * Config.Resolution + CellX];
			const bool bInBand = Height >= LowAltitude && Height <= HighAltitude;
			if (bInBand && IsFarFromOtherSources(Sources, CellX, CellY, MinSeparationCells))
			{
				Sources.Add(FMassifRiverSource{ CellX, CellY, Config.RiverSourceRate });
			}
		}
		return Sources;
	}
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

	const bool bWantsErosion = Config.HydraulicIterationCount > 0 || Config.ThermalIterationCount > 0;
	if (bWantsErosion && FMassifGpuErosion::CanRun())
	{
		const TArray<FMassifRiverSource> RiverSources = PickRiverSources(Config, Result.HeightMeters);
		const double ErosionStartSeconds = FPlatformTime::Seconds();
		const bool bEroded = FMassifGpuErosion::Run(Config, Result.HeightMeters, RiverSources);
		UE_LOG(LogTemp, Display, TEXT("WorldGen erosion: %s in %.2f s (%d hydraulic, %d thermal iterations, %d river sources)"),
			bEroded ? TEXT("applied") : TEXT("FAILED"), FPlatformTime::Seconds() - ErosionStartSeconds,
			Config.HydraulicIterationCount, Config.ThermalIterationCount, RiverSources.Num());
	}

	return Result;
}
