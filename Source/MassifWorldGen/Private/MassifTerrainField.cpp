#include "MassifTerrainField.h"

#include "MassifErosionFilter.h"
#include "MassifNoise.h"
#include "MassifSeed.h"
#include "MassifUpliftField.h"

namespace
{
	/**
	 * Fixed step for the base-surface gradient, so slope cannot depend on how
	 * the caller samples. Deliberately coarse: the uplift envelope carries
	 * ridged noise whose folds kink the gradient direction, and the filter
	 * levers any direction kink into a cliff a phase cell away — the wide
	 * difference low-passes the direction field below gully scale.
	 */
	const double GradientStepMeters = 150.0;

	FMassifErosionSettings MakeErosionSettings(const FWorldGenConfig& Config, uint32 Seed)
	{
		FMassifErosionSettings Settings;
		Settings.OctaveCount = Config.GullyOctaveCount;
		Settings.LargestGullyWavelengthMeters = Config.LargestGullyWavelengthMeters;
		Settings.LargestGullyDepthMeters = Config.LargestGullyDepthMeters;
		Settings.WavelengthGain = Config.GullyWavelengthGain;
		Settings.DepthGain = Config.GullyDepthGain;
		Settings.CellsPerWavelength = Config.GullyCellsPerWavelength;
		Settings.SlopeReference = Config.GullySlopeReference;
		Settings.DetailPersistence = Config.GullyDetailPersistence;
		Settings.BranchStrength = Config.GullyBranchStrength;
		Settings.RidgeRounding = Config.GullyRidgeRounding;
		Settings.CreaseRounding = Config.GullyCreaseRounding;
		Settings.Seed = Seed;
		return Settings;
	}
}

FMassifTerrainField::FMassifTerrainField(const FWorldGenConfig& InConfig)
	: Config(InConfig)
{
	const FMassifSeed Seed(static_cast<uint32>(Config.MasterSeed));
	Uplift = MakeUnique<FMassifUpliftField>(Config, Seed.MakeChannelSeed(TEXT("WorldGen.Uplift")));
	BaseReliefSeed = Seed.MakeChannelSeed(TEXT("WorldGen.BaseRelief"));
	ErosionSeed = Seed.MakeChannelSeed(TEXT("WorldGen.ErosionFilter"));
}

FMassifTerrainField::~FMassifTerrainField() = default;

int32 FMassifTerrainField::OctavesForSpacing(double SampleSpacingMeters, float& OutFinestWeight) const
{
	// Samples the filter has to put across one gully before it is worth cutting
	// at all. Below this the octave only aliases into the samples that carry it.
	const double SamplesPerGully = 4.0;
	const double SmallestUsefulWavelength = FMath::Max(SampleSpacingMeters, UE_DOUBLE_SMALL_NUMBER) * SamplesPerGully;
	const double Gain = FMath::Clamp(static_cast<double>(Config.GullyWavelengthGain), 0.05, 0.95);

	OutFinestWeight = 1.0f;
	double Wavelength = Config.LargestGullyWavelengthMeters;
	for (int32 Octave = 0; Octave < Config.GullyOctaveCount; ++Octave)
	{
		const double NextWavelength = Wavelength * Gain;
		if (NextWavelength < SmallestUsefulWavelength)
		{
			// Fade this last one in over the octave it spans rather than
			// switching it on, so two spacings either side of the cut agree.
			OutFinestWeight = static_cast<float>(FMath::Clamp(
				(Wavelength - SmallestUsefulWavelength) / FMath::Max(Wavelength - NextWavelength, UE_DOUBLE_SMALL_NUMBER), 0.0, 1.0));
			return Octave + 1;
		}
		Wavelength = NextWavelength;
	}
	return Config.GullyOctaveCount;
}

double FMassifTerrainField::SampleBaseHeightMeters(FVector2d PositionMeters) const
{
	// The uplift envelope decides where ranges stand; the fractal decides where
	// the valleys between them run. Biased above the midpoint so ground still
	// reaches the crests after the filter has taken its share.
	const FMassifUpliftSample UpliftSample = Uplift->Sample(PositionMeters);
	const double NoiseScale = 1.0 / FMath::Max(1.0, static_cast<double>(Config.BaseReliefWavelengthMeters));
	const float Noise = MassifNoise::Fractal(
		PositionMeters.X * NoiseScale, PositionMeters.Y * NoiseScale,
		BaseReliefSeed, Config.BaseReliefOctaveCount, 2.0, 0.5f);
	const double ReliefFraction = FMath::Clamp(0.55 + 0.45 * Noise, 0.0, 1.0);
	return FMath::Lerp(static_cast<double>(Config.MinAltitudeMeters), UpliftSample.CrestAltitudeMeters, ReliefFraction);
}

FVector2d FMassifTerrainField::SampleBaseGradient(FVector2d PositionMeters) const
{
	const double West = SampleBaseHeightMeters(PositionMeters - FVector2d(GradientStepMeters, 0.0));
	const double East = SampleBaseHeightMeters(PositionMeters + FVector2d(GradientStepMeters, 0.0));
	const double South = SampleBaseHeightMeters(PositionMeters - FVector2d(0.0, GradientStepMeters));
	const double North = SampleBaseHeightMeters(PositionMeters + FVector2d(0.0, GradientStepMeters));
	return FVector2d(East - West, North - South) / (2.0 * GradientStepMeters);
}

float FMassifTerrainField::ComputeReliefFraction(FVector2d PositionMeters, double GroundHeightMeters) const
{
	const double CrestAltitudeMeters = Uplift->Sample(PositionMeters).CrestAltitudeMeters;
	const double AvailableMeters = FMath::Max(1.0, CrestAltitudeMeters - Config.MinAltitudeMeters);
	return static_cast<float>(FMath::Clamp(
		(GroundHeightMeters - Config.MinAltitudeMeters) / AvailableMeters, 0.0, 1.0));
}

double FMassifTerrainField::SampleHeightMeters(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	const double BaseHeightMeters = SampleBaseHeightMeters(PositionMeters);

	float FinestWeight = 1.0f;
	FMassifErosionSettings Settings = MakeErosionSettings(Config, ErosionSeed);
	Settings.OctaveCount = OctavesForSpacing(SampleSpacingMeters, FinestWeight);
	Settings.FinestOctaveWeight = FinestWeight;

	FMassifErosionInput ErosionInput;
	ErosionInput.PositionMeters = PositionMeters;
	ErosionInput.SlopeMetersPerMeter = SampleBaseGradient(PositionMeters);
	ErosionInput.ReliefFraction = ComputeReliefFraction(PositionMeters, BaseHeightMeters);

	const double Altitude = BaseHeightMeters + MassifErosionFilter::EvaluateDisplacementMeters(ErosionInput, Settings);
	return FMath::Clamp(Altitude, static_cast<double>(Config.MinAltitudeMeters), static_cast<double>(Config.MaxAltitudeMeters));
}

FVector3d FMassifTerrainField::SampleNormal(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	const double Step = FMath::Max(SampleSpacingMeters, UE_DOUBLE_SMALL_NUMBER);
	const double West = SampleHeightMeters(PositionMeters - FVector2d(Step, 0.0), SampleSpacingMeters);
	const double East = SampleHeightMeters(PositionMeters + FVector2d(Step, 0.0), SampleSpacingMeters);
	const double South = SampleHeightMeters(PositionMeters - FVector2d(0.0, Step), SampleSpacingMeters);
	const double North = SampleHeightMeters(PositionMeters + FVector2d(0.0, Step), SampleSpacingMeters);

	const FVector3d Normal(West - East, South - North, 2.0 * Step);
	return Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector3d::UpVector);
}

double FMassifTerrainField::SampleSlopeDegrees(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	const double NormalZ = FMath::Clamp(SampleNormal(PositionMeters, SampleSpacingMeters).Z, -1.0, 1.0);
	return FMath::RadiansToDegrees(FMath::Acos(NormalZ));
}
