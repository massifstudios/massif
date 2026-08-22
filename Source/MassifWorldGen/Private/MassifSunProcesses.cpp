#include "MassifSunProcesses.h"

#include "MassifEnvironmentGraph.h"

namespace MassifEnvironmentChannels
{
	const FName& SkyViewFactor()
	{
		static const FName Name(TEXT("Sun.SkyViewFactor"));
		return Name;
	}

	const FName& DirectInsolation()
	{
		static const FName Name(TEXT("Sun.DirectInsolation"));
		return Name;
	}

	const FName& DiffuseInsolation()
	{
		static const FName Name(TEXT("Sun.DiffuseInsolation"));
		return Name;
	}
}

namespace MassifEnvironmentProcesses
{
	const FName& Insolation()
	{
		static const FName Name(TEXT("Sun.Insolation"));
		return Name;
	}
}

namespace
{
	/** Where the sun stands, in the frame the terrain is in: X east, Y north, Z up. */
	struct FSunPosition
	{
		double AltitudeRadians = 0.0;

		/** Unit vector towards the sun. Meaningless where the altitude is negative. */
		FVector3d Direction = FVector3d::ZAxisVector;
	};

	/** Tilt of the earth's axis against its orbit, which is what gives the year a summer. */
	constexpr double AxialTiltDegrees = 23.44;

	double SolarDeclinationRadians(int32 DayOfYear)
	{
		const double OrbitFraction = UE_DOUBLE_TWO_PI * (284.0 + DayOfYear) / 365.0;
		return FMath::DegreesToRadians(AxialTiltDegrees) * FMath::Sin(OrbitFraction);
	}

	FSunPosition SunAt(double LatitudeRadians, double DeclinationRadians, double HourAngleRadians)
	{
		const double SinAltitude = FMath::Sin(LatitudeRadians) * FMath::Sin(DeclinationRadians)
			+ FMath::Cos(LatitudeRadians) * FMath::Cos(DeclinationRadians) * FMath::Cos(HourAngleRadians);

		FSunPosition Sun;
		Sun.AltitudeRadians = FMath::Asin(FMath::Clamp(SinAltitude, -1.0, 1.0));

		// Azimuth clockwise from north, so the horizontal part is (sin, cos) east-north.
		const double CosAltitude = FMath::Max(FMath::Cos(Sun.AltitudeRadians), UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double CosAzimuth = (FMath::Sin(DeclinationRadians) * FMath::Cos(LatitudeRadians)
			- FMath::Cos(DeclinationRadians) * FMath::Sin(LatitudeRadians) * FMath::Cos(HourAngleRadians))
			/ CosAltitude;

		double AzimuthRadians = FMath::Acos(FMath::Clamp(CosAzimuth, -1.0, 1.0));
		if (HourAngleRadians > 0.0)
		{
			AzimuthRadians = UE_DOUBLE_TWO_PI - AzimuthRadians;
		}

		Sun.Direction = FVector3d(
			CosAltitude * FMath::Sin(AzimuthRadians), CosAltitude * FMath::Cos(AzimuthRadians), SinAltitude);
		return Sun;
	}

	/** Hour angle the sun crosses the horizon at, which is what bounds the day being integrated. */
	double SunsetHourAngleRadians(double LatitudeRadians, double DeclinationRadians)
	{
		const double CosHourAngle = -FMath::Tan(LatitudeRadians) * FMath::Tan(DeclinationRadians);
		if (CosHourAngle <= -1.0)
		{
			// Midnight sun: the whole rotation is daylight.
			return UE_DOUBLE_PI;
		}
		if (CosHourAngle >= 1.0)
		{
			return 0.0;
		}
		return FMath::Acos(CosHourAngle);
	}

	TArray<FSunPosition> SampleSunPath(double LatitudeDegrees, const FMassifSunParams& Params)
	{
		const double LatitudeRadians = FMath::DegreesToRadians(LatitudeDegrees);
		const double DeclinationRadians = SolarDeclinationRadians(Params.RepresentativeDayOfYear);
		const double SunsetRadians = SunsetHourAngleRadians(LatitudeRadians, DeclinationRadians);

		const int32 StepCount = FMath::Max(2, Params.SunPathStepCount);
		TArray<FSunPosition> Path;
		Path.Reserve(StepCount);
		for (int32 Step = 0; Step < StepCount; ++Step)
		{
			// Midpoints of equal slices of the day, so neither end is counted twice.
			const double Fraction = (Step + 0.5) / StepCount;
			const double HourAngleRadians = FMath::Lerp(-SunsetRadians, SunsetRadians, Fraction);
			Path.Add(SunAt(LatitudeRadians, DeclinationRadians, HourAngleRadians));
		}
		return Path;
	}

	/** The terrain the sweep marches over, sampled from the altitude channel at this grid's spacing. */
	class FTerrainWindow
	{
	public:
		FTerrainWindow(const FMassifEnvironmentState& InState, FMassifChannelHandle InAltitude)
			: State(InState)
			, Altitude(InAltitude)
		{
		}

		double AltitudeAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(Altitude, PositionMeters);
		}

		/** Upward normal, differenced across one cell of the grid being solved. */
		FVector3d NormalAt(FVector2d PositionMeters, double SpacingMeters) const
		{
			const double East = AltitudeAt(PositionMeters + FVector2d(SpacingMeters, 0.0));
			const double West = AltitudeAt(PositionMeters - FVector2d(SpacingMeters, 0.0));
			const double North = AltitudeAt(PositionMeters + FVector2d(0.0, SpacingMeters));
			const double South = AltitudeAt(PositionMeters - FVector2d(0.0, SpacingMeters));

			const FVector3d Normal(
				-(East - West) / (2.0 * SpacingMeters), -(North - South) / (2.0 * SpacingMeters), 1.0);
			return Normal.GetSafeNormal();
		}

	private:
		const FMassifEnvironmentState& State;
		FMassifChannelHandle Altitude;
	};

	double SweepOneAzimuth(
		const FTerrainWindow& Terrain, FVector2d PositionMeters, double GroundAltitudeMeters,
		const FMassifSunParams& Params, double AzimuthRadians)
	{
		const FVector2d Step(FMath::Sin(AzimuthRadians), FMath::Cos(AzimuthRadians));
		const int32 StepCount = FMath::Max(1, Params.HorizonStepCount);

		double HighestTangent = 0.0;
		for (int32 StepIndex = 1; StepIndex <= StepCount; ++StepIndex)
		{
			// Steps grow with distance: near ground needs the resolution, a ridge
			// four kilometres out does not.
			const double DistanceMeters = Params.HorizonReachMeters * FMath::Square(StepIndex / double(StepCount));
			const double RiseMeters = Terrain.AltitudeAt(PositionMeters + Step * DistanceMeters)
				- GroundAltitudeMeters;
			HighestTangent = FMath::Max(HighestTangent, RiseMeters / DistanceMeters);
		}
		return FMath::Atan(HighestTangent);
	}

	/**
	 * Highest angle the skyline reaches in each compass direction from a point.
	 *
	 * This is what a ridge kilometres away does to a north cirque: no local
	 * measurement of slope or aspect can see it, and without it a treeline lands
	 * on a contour instead of on a shadow.
	 */
	void SweepHorizon(
		const FTerrainWindow& Terrain, FVector2d PositionMeters, double GroundAltitudeMeters,
		const FMassifSunParams& Params, TArray<double>& OutHorizonRadians)
	{
		const int32 AzimuthCount = FMath::Max(1, Params.HorizonAzimuthCount);
		OutHorizonRadians.SetNumZeroed(AzimuthCount);
		for (int32 AzimuthIndex = 0; AzimuthIndex < AzimuthCount; ++AzimuthIndex)
		{
			OutHorizonRadians[AzimuthIndex] = SweepOneAzimuth(
				Terrain, PositionMeters, GroundAltitudeMeters, Params,
				UE_DOUBLE_TWO_PI * AzimuthIndex / AzimuthCount);
		}
	}

	/** Fraction of the sky hemisphere a point can see, from the swept horizon. */
	double SkyViewFactorFrom(TArrayView<const double> HorizonRadians)
	{
		if (HorizonRadians.IsEmpty())
		{
			return 1.0;
		}

		double Sum = 0.0;
		for (double Angle : HorizonRadians)
		{
			Sum += FMath::Square(FMath::Cos(Angle));
		}
		return Sum / FMath::Max(1, HorizonRadians.Num());
	}

	bool IsSunAboveHorizon(const FSunPosition& Sun, TArrayView<const double> HorizonRadians)
	{
		if (HorizonRadians.IsEmpty())
		{
			return true;
		}

		// Azimuth back out of the direction, to pick the sweep it falls in.
		const double AzimuthRadians = FMath::Atan2(Sun.Direction.X, Sun.Direction.Y);
		const double Wrapped = FMath::Fmod(AzimuthRadians + UE_DOUBLE_TWO_PI, UE_DOUBLE_TWO_PI);
		const int32 Index = FMath::Clamp(
			FMath::RoundToInt32(Wrapped / UE_DOUBLE_TWO_PI * HorizonRadians.Num()), 0, HorizonRadians.Num() - 1)
			% HorizonRadians.Num();
		return Sun.AltitudeRadians > HorizonRadians[Index];
	}

	/**
	 * One step of the day, added to what this ground takes and to what level open
	 * ground at the same latitude takes over the same step. The ratio of the two is
	 * what the channel holds, so no radiative constant has to be believed.
	 */
	void AccumulateStep(
		const FSunPosition& Sun, FVector3d Normal, TArrayView<const double> HorizonRadians,
		double& ReceivedEnergy, double& LevelEnergy)
	{
		if (Sun.AltitudeRadians <= 0.0)
		{
			return;
		}

		LevelEnergy += FMath::Sin(Sun.AltitudeRadians);
		if (IsSunAboveHorizon(Sun, HorizonRadians))
		{
			ReceivedEnergy += FMath::Max(0.0, FVector3d::DotProduct(Normal, Sun.Direction));
		}
	}

	/** Reference sweeps and integrates twice as finely; the model itself is the same. */
	FMassifSunParams RefineForFidelity(const FMassifSunParams& Params, EMassifFidelity Fidelity)
	{
		if (Fidelity != EMassifFidelity::Reference)
		{
			return Params;
		}

		FMassifSunParams Refined = Params;
		Refined.HorizonAzimuthCount = Params.HorizonAzimuthCount * 2;
		Refined.HorizonStepCount = Params.HorizonStepCount * 2;
		Refined.SunPathStepCount = Params.SunPathStepCount * 2;
		return Refined;
	}
}

/**
 * Sunlight over the world: how much of the sky a point can see, and how much of
 * a day's sun actually reaches it.
 *
 * Both are answered against a flat, unshaded surface at the same latitude, so a
 * value of 1 means "as much as level open ground here" and the numbers stay
 * meaningful without a radiative constant in them.
 */
class FInsolationProcess final : public IMassifEnvironmentProcess
{
public:
	explicit FInsolationProcess(const FMassifSunParams& InParams)
		: Params(InParams)
	{
	}

	FMassifProcessDeclaration Declare() const override;
	void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override;

private:
	FMassifChannelDescriptor MakeFractionChannel(const FName& Name, EMassifChannelColorMap ColorMap) const;

	void WriteUnlit(FMassifEnvironmentContext& Context) const;

	void SolveRow(
		const FTerrainWindow& Terrain, const TArray<FSunPosition>& SunPath, const FMassifSunParams& RunParams,
		const FMassifChannelGrid& Grid, int32 Row, EMassifFidelity Fidelity, TArrayView<double> OutSkyView,
		TArrayView<double> OutDirect, TArrayView<double> OutDiffuse) const;

	FMassifSunParams Params;
};

FMassifChannelDescriptor FInsolationProcess::MakeFractionChannel(
	const FName& Name, EMassifChannelColorMap ColorMap) const
{
	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = Name;
	Descriptor.Type = EMassifChannelType::Scalar;
	Descriptor.Encoding = EMassifChannelEncoding::Uint16;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.Unit = TEXT("0-1");
	Descriptor.ColorMap = ColorMap;
	Descriptor.DisplayRange = FVector2d(0.0, 1.0);
	return Descriptor;
}

FMassifProcessDeclaration FInsolationProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::Insolation();
	Declaration.InputChannels = {MassifEnvironmentChannels::Altitude()};
	Declaration.SupportedFidelities = {
		EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};
	Declaration.OutputChannels.Add(
		MakeFractionChannel(MassifEnvironmentChannels::SkyViewFactor(), EMassifChannelColorMap::Viridis));
	Declaration.OutputChannels.Add(
		MakeFractionChannel(MassifEnvironmentChannels::DirectInsolation(), EMassifChannelColorMap::Magma));
	Declaration.OutputChannels.Add(
		MakeFractionChannel(MassifEnvironmentChannels::DiffuseInsolation(), EMassifChannelColorMap::Magma));
	return Declaration;
}

void FInsolationProcess::WriteUnlit(FMassifEnvironmentContext& Context) const
{
	// Off is an open sky over level ground everywhere: the channels still exist, so
	// nothing downstream has to ask whether the sun was solved.
	const FName Names[] = {
		MassifEnvironmentChannels::SkyViewFactor(), MassifEnvironmentChannels::DirectInsolation(),
		MassifEnvironmentChannels::DiffuseInsolation()};
	for (const FName& Name : Names)
	{
		const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(Name);
		TArrayView<double> Values = Scope->GetComponent(0);
		for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
		{
			Values[CellIndex] = 1.0;
		}
		Scope->Commit();
	}
}

void FInsolationProcess::SolveRow(
	const FTerrainWindow& Terrain, const TArray<FSunPosition>& SunPath, const FMassifSunParams& RunParams,
	const FMassifChannelGrid& Grid, int32 Row, EMassifFidelity Fidelity, TArrayView<double> OutSkyView,
	TArrayView<double> OutDirect, TArrayView<double> OutDiffuse) const
{
	const bool bShadowed = Fidelity != EMassifFidelity::Cheap;

	TArray<double> HorizonRadians;
	for (int32 Column = 0; Column < Grid.Width; ++Column)
	{
		const FVector2d Position = Grid.CellCentreMeters(Column, Row);
		const FVector3d Normal = Terrain.NormalAt(Position, Grid.CellSizeMeters);

		HorizonRadians.Reset();
		if (bShadowed)
		{
			SweepHorizon(Terrain, Position, Terrain.AltitudeAt(Position), RunParams, HorizonRadians);
		}

		double ReceivedEnergy = 0.0;
		double LevelEnergy = 0.0;
		for (const FSunPosition& Sun : SunPath)
		{
			AccumulateStep(Sun, Normal, HorizonRadians, ReceivedEnergy, LevelEnergy);
		}

		const int32 CellIndex = Row * Grid.Width + Column;
		OutSkyView[CellIndex] = SkyViewFactorFrom(HorizonRadians);
		OutDirect[CellIndex] = ReceivedEnergy / FMath::Max(LevelEnergy, UE_DOUBLE_KINDA_SMALL_NUMBER);

		// Isotropic sky: a tilted surface sees less of it, and a hemmed-in one less again.
		OutDiffuse[CellIndex] = OutSkyView[CellIndex] * 0.5 * (1.0 + Normal.Z);
	}
}

void FInsolationProcess::Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const
{
	if (Fidelity == EMassifFidelity::Off)
	{
		WriteUnlit(Context);
		return;
	}

	const FMassifSunParams RunParams = RefineForFidelity(Params, Fidelity);
	const FTerrainWindow Terrain(
		Context.GetState(), Context.GetInput(MassifEnvironmentChannels::Altitude()));
	const TArray<FSunPosition> SunPath =
		SampleSunPath(Context.GetGeoreference().LatitudeDegrees, RunParams);

	const TUniquePtr<FMassifChannelWriteScope> SkyView =
		Context.OpenOutput(MassifEnvironmentChannels::SkyViewFactor());
	const TUniquePtr<FMassifChannelWriteScope> Direct =
		Context.OpenOutput(MassifEnvironmentChannels::DirectInsolation());
	const TUniquePtr<FMassifChannelWriteScope> Diffuse =
		Context.OpenOutput(MassifEnvironmentChannels::DiffuseInsolation());

	const FMassifChannelGrid& Grid = SkyView->GetGrid();
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		SolveRow(
			Terrain, SunPath, RunParams, Grid, Row, Fidelity, SkyView->GetComponent(0),
			Direct->GetComponent(0), Diffuse->GetComponent(0));
	}

	SkyView->Commit();
	Direct->Commit();
	Diffuse->Commit();
}

void MassifRegisterSunProcesses(FMassifEnvironmentScheduler& Scheduler, const FMassifSunParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FInsolationProcess>(Params));
}
