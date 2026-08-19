#include "MassifTerrainField.h"

#include "MassifErosionFilter.h"
#include "MassifFluvialBake.h"
#include "MassifSeed.h"
#include "MassifUpliftField.h"

namespace
{
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
	FluvialBake = MakeUnique<FMassifFluvialBake>(FMassifFluvialBake::Build(Config, *Uplift));
}

FMassifTerrainField::~FMassifTerrainField() = default;

const FMassifFluvialBake& FMassifTerrainField::GetFluvialBake() const
{
	return *FluvialBake;
}

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

float FMassifTerrainField::ComputeReliefFraction(FVector2d PositionMeters, double GroundHeightMeters) const
{
	const double DrainWaterMeters = FluvialBake->SampleDrainWaterElevationMeters(PositionMeters);
	const double CrestAltitudeMeters = Uplift->Sample(PositionMeters).CrestAltitudeMeters;
	const double AvailableMeters = FMath::Max(1.0, CrestAltitudeMeters - DrainWaterMeters);
	return static_cast<float>(FMath::Clamp((GroundHeightMeters - DrainWaterMeters) / AvailableMeters, 0.0, 1.0));
}

double FMassifTerrainField::SampleHeightMeters(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	const double MacroHeightMeters = FluvialBake->SampleHeightMeters(PositionMeters);
	const double CarvedHeightMeters = MacroHeightMeters
		- FluvialBake->SampleChannelCarveMeters(PositionMeters, MacroHeightMeters, Config);

	float FinestWeight = 1.0f;
	FMassifErosionSettings Settings = MakeErosionSettings(Config, FMassifSeed(static_cast<uint32>(Config.MasterSeed)).MakeChannelSeed(TEXT("WorldGen.ErosionFilter")));
	Settings.OctaveCount = OctavesForSpacing(SampleSpacingMeters, FinestWeight);
	Settings.FinestOctaveWeight = FinestWeight;

	FMassifErosionInput ErosionInput;
	ErosionInput.PositionMeters = PositionMeters;
	ErosionInput.SlopeMetersPerMeter = FluvialBake->SampleGradientMetersPerMeter(PositionMeters);
	ErosionInput.ReliefFraction = ComputeReliefFraction(PositionMeters, CarvedHeightMeters);

	auto SampleSlope = [this](FVector2d PivotMeters)
	{
		return FluvialBake->SampleGradientMetersPerMeter(PivotMeters);
	};

	const double Altitude = CarvedHeightMeters + MassifErosionFilter::EvaluateDisplacementMeters(ErosionInput, Settings, SampleSlope);
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
