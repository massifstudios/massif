#include "MassifTerrainField.h"

#include "MassifErosionFilter.h"
#include "MassifSeed.h"
#include "MassifUpliftField.h"
#include "RiverNetwork.h"

namespace
{
	/**
	 * Samples the filter has to put across one gully before it is worth cutting
	 * at all. Below this the octave only aliases into the samples that carry it.
	 */
	const double SamplesPerGully = 4.0;

	/** Spacing of the orientation lattice. Coarse on purpose: gully direction should follow the mountain, not the metre. */
	const double OrientationSpacingMeters = 192.0;

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

	float CubicInterpolate(float Before, float Low, float High, float After, float Fraction)
	{
		const float Slope = 0.5f * (High - Before);
		const float NextSlope = 0.5f * (After - Low);
		const float Delta = High - Low;
		const float Cubic = Slope + NextSlope - 2.0f * Delta;
		const float Quadratic = 3.0f * Delta - 2.0f * Slope - NextSlope;
		return Low + Fraction * (Slope + Fraction * (Quadratic + Fraction * Cubic));
	}
}

FMassifTerrainField::FMassifTerrainField(const FWorldGenConfig& InConfig)
	: Config(InConfig)
{
	const FMassifSeed Seed(static_cast<uint32>(Config.MasterSeed));
	Uplift = MakeUnique<FMassifUpliftField>(Config, Seed.MakeChannelSeed(TEXT("WorldGen.Uplift")));
	RiverNetwork = MakeUnique<FRiverNetwork>(FRiverNetwork::Build(Config, *Uplift));
	BuildOrientationLattice();
}

FMassifTerrainField::~FMassifTerrainField() = default;

int32 FMassifTerrainField::ClampOrientationNode(int32 NodeIndex) const
{
	return FMath::Clamp(NodeIndex, 0, OrientationNodesPerSide - 1);
}

void FMassifTerrainField::BuildOrientationLattice()
{
	OrientationSpacingMeters = ::OrientationSpacingMeters;
	OrientationOriginMeters = Config.WorldSizeMeters * 0.5;
	OrientationNodesPerSide = FMath::Max(4, FMath::CeilToInt32(Config.WorldSizeMeters / OrientationSpacingMeters) + 1);

	const int32 NodeCount = OrientationNodesPerSide * OrientationNodesPerSide;
	TArray<double> LatticeHeights;
	LatticeHeights.SetNumUninitialized(NodeCount);

	ParallelFor(OrientationNodesPerSide, [this, &LatticeHeights](int32 NodeY)
	{
		const double PositionY = -OrientationOriginMeters + NodeY * OrientationSpacingMeters;
		for (int32 NodeX = 0; NodeX < OrientationNodesPerSide; ++NodeX)
		{
			const double PositionX = -OrientationOriginMeters + NodeX * OrientationSpacingMeters;
			LatticeHeights[NodeY * OrientationNodesPerSide + NodeX] =
				RiverNetwork->Sample(FVector2d(PositionX, PositionY), Config, *Uplift).HeightMeters;
		}
	});

	OrientationSlope.SetNumUninitialized(NodeCount);
	ParallelFor(OrientationNodesPerSide, [this, &LatticeHeights](int32 NodeY)
	{
		for (int32 NodeX = 0; NodeX < OrientationNodesPerSide; ++NodeX)
		{
			const int32 EastIndex = NodeY * OrientationNodesPerSide + ClampOrientationNode(NodeX + 1);
			const int32 WestIndex = NodeY * OrientationNodesPerSide + ClampOrientationNode(NodeX - 1);
			const int32 NorthIndex = ClampOrientationNode(NodeY + 1) * OrientationNodesPerSide + NodeX;
			const int32 SouthIndex = ClampOrientationNode(NodeY - 1) * OrientationNodesPerSide + NodeX;

			const double RunX = (ClampOrientationNode(NodeX + 1) - ClampOrientationNode(NodeX - 1)) * OrientationSpacingMeters;
			const double RunY = (ClampOrientationNode(NodeY + 1) - ClampOrientationNode(NodeY - 1)) * OrientationSpacingMeters;

			OrientationSlope[NodeY * OrientationNodesPerSide + NodeX] = FVector2f(
				static_cast<float>((LatticeHeights[EastIndex] - LatticeHeights[WestIndex]) / FMath::Max(RunX, OrientationSpacingMeters)),
				static_cast<float>((LatticeHeights[NorthIndex] - LatticeHeights[SouthIndex]) / FMath::Max(RunY, OrientationSpacingMeters)));
		}
	});
}

FVector2d FMassifTerrainField::SampleOrientationSlope(FVector2d PositionMeters) const
{
	const double LatticeX = (PositionMeters.X + OrientationOriginMeters) / OrientationSpacingMeters;
	const double LatticeY = (PositionMeters.Y + OrientationOriginMeters) / OrientationSpacingMeters;
	const int32 BaseX = FMath::FloorToInt32(FMath::Clamp(LatticeX, -1.0, static_cast<double>(OrientationNodesPerSide)));
	const int32 BaseY = FMath::FloorToInt32(FMath::Clamp(LatticeY, -1.0, static_cast<double>(OrientationNodesPerSide)));
	const float FractionX = static_cast<float>(FMath::Clamp(LatticeX - BaseX, 0.0, 1.0));
	const float FractionY = static_cast<float>(FMath::Clamp(LatticeY - BaseY, 0.0, 1.0));

	// Bicubic: a lerp between lattice nodes is continuous in value but not in
	// gradient, and the seams show as creases along the gullies laid over it.
	float RowsX[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float RowsY[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (int32 Row = 0; Row < 4; ++Row)
	{
		const int32 RowBase = ClampOrientationNode(BaseY - 1 + Row) * OrientationNodesPerSide;
		const FVector2f Tap0 = OrientationSlope[RowBase + ClampOrientationNode(BaseX - 1)];
		const FVector2f Tap1 = OrientationSlope[RowBase + ClampOrientationNode(BaseX)];
		const FVector2f Tap2 = OrientationSlope[RowBase + ClampOrientationNode(BaseX + 1)];
		const FVector2f Tap3 = OrientationSlope[RowBase + ClampOrientationNode(BaseX + 2)];
		RowsX[Row] = CubicInterpolate(Tap0.X, Tap1.X, Tap2.X, Tap3.X, FractionX);
		RowsY[Row] = CubicInterpolate(Tap0.Y, Tap1.Y, Tap2.Y, Tap3.Y, FractionX);
	}

	return FVector2d(
		CubicInterpolate(RowsX[0], RowsX[1], RowsX[2], RowsX[3], FractionY),
		CubicInterpolate(RowsY[0], RowsY[1], RowsY[2], RowsY[3], FractionY));
}

int32 FMassifTerrainField::OctavesForSpacing(double SampleSpacingMeters, float& OutFinestWeight) const
{
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

double FMassifTerrainField::SampleHeightMeters(FVector2d PositionMeters, double SampleSpacingMeters) const
{
	const FRiverTerrainSample Macro = RiverNetwork->Sample(PositionMeters, Config, *Uplift);

	float FinestWeight = 1.0f;
	FMassifErosionSettings Settings = MakeErosionSettings(Config, FMassifSeed(static_cast<uint32>(Config.MasterSeed)).MakeChannelSeed(TEXT("WorldGen.ErosionFilter")));
	Settings.OctaveCount = OctavesForSpacing(SampleSpacingMeters, FinestWeight);
	Settings.FinestOctaveWeight = FinestWeight;

	FMassifErosionInput ErosionInput;
	ErosionInput.PositionMeters = PositionMeters;
	ErosionInput.SlopeMetersPerMeter = SampleOrientationSlope(PositionMeters);
	ErosionInput.ReliefFraction = Macro.ReliefFraction;

	auto SampleSlope = [this](FVector2d PivotMeters)
	{
		return SampleOrientationSlope(PivotMeters);
	};

	const double Altitude = Macro.HeightMeters + MassifErosionFilter::EvaluateDisplacementMeters(ErosionInput, Settings, SampleSlope);
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
