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

	const FName& AnnualInsolation()
	{
		static const FName Name(TEXT("Sun.AnnualInsolation"));
		return Name;
	}

	const FName& Horizon()
	{
		static const FName Name(TEXT("Sun.Horizon"));
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

	/** Middle of a half-month, which is the day that half-month's sun path is taken on. */
	double SliceMidDayOfYear(int32 TimeSlice)
	{
		return (TimeSlice + 0.5) * 365.0 / MassifSeasonalSliceCount;
	}

	double SolarDeclinationRadians(double DayOfYear)
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

	/**
	 * One half-month's sun path, and what level open ground takes over it.
	 *
	 * The step weight is what makes two days comparable: a December day is both
	 * lower and shorter than a June one, and an unweighted sum over a fixed step
	 * count would throw the shortness away and leave only the lowness.
	 */
	struct FSunDay
	{
		TArray<FSunPosition> Path;

		/** Hour angle one step covers, scaled so that a day's total comes out as a channel value. */
		double StepWeightRadians = 0.0;

		/** What level open ground at this latitude takes on this day, in the channel's own units. */
		double LevelEnergy = 0.0;
	};

	/** Slices of the year a span covers, both ends included. */
	struct FSliceRange
	{
		int32 FirstSlice = 0;
		int32 LastSlice = MassifSeasonalSliceCount - 1;

		int32 Count() const
		{
			return LastSlice - FirstSlice + 1;
		}
	};

	FSliceRange GrowingSeasonSlices(const FMassifSunParams& Params)
	{
		FSliceRange Range;
		Range.FirstSlice = FMath::Clamp(Params.GrowingSeasonFirstSlice, 0, MassifSeasonalSliceCount - 1);
		Range.LastSlice = FMath::Clamp(Params.GrowingSeasonLastSlice, Range.FirstSlice, MassifSeasonalSliceCount - 1);
		return Range;
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

	/** What one step of the day puts on a surface, before the day's step weight scales it. */
	double StepEnergy(const FSunPosition& Sun, FVector3d Normal, TArrayView<const double> HorizonRadians)
	{
		if (Sun.AltitudeRadians <= 0.0)
		{
			return 0.0;
		}
		if (!IsSunAboveHorizon(Sun, HorizonRadians))
		{
			return 0.0;
		}
		return FMath::Max(0.0, FVector3d::DotProduct(Normal, Sun.Direction));
	}

	/** What a surface takes over one whole day, integrated at the day's own step spacing. */
	double ReceivedDayEnergy(const FSunDay& Day, FVector3d Normal, TArrayView<const double> HorizonRadians)
	{
		double Energy = 0.0;
		for (const FSunPosition& Sun : Day.Path)
		{
			Energy += StepEnergy(Sun, Normal, HorizonRadians);
		}
		return Day.StepWeightRadians * Energy;
	}

	/** The same for level open ground under an open sky, which is what a slice is measured against. */
	double LevelDayEnergy(const FSunDay& Day)
	{
		return ReceivedDayEnergy(Day, FVector3d::ZAxisVector, TArrayView<const double>());
	}

	FSunDay SampleSunDay(double LatitudeRadians, double DayOfYear, const FMassifSunParams& Params)
	{
		const double DeclinationRadians = SolarDeclinationRadians(DayOfYear);
		const double SunsetRadians = SunsetHourAngleRadians(LatitudeRadians, DeclinationRadians);
		const int32 StepCount = FMath::Max(2, Params.SunPathStepCount);

		FSunDay Day;
		Day.StepWeightRadians = 2.0 * SunsetRadians / StepCount;
		Day.Path.Reserve(StepCount);
		for (int32 Step = 0; Step < StepCount; ++Step)
		{
			// Midpoints of equal slices of the day, so neither end is counted twice.
			const double Fraction = (Step + 0.5) / StepCount;
			const double HourAngleRadians = FMath::Lerp(-SunsetRadians, SunsetRadians, Fraction);
			Day.Path.Add(SunAt(LatitudeRadians, DeclinationRadians, HourAngleRadians));
		}

		Day.LevelEnergy = LevelDayEnergy(Day);
		return Day;
	}

	/**
	 * Divides the year through by what level open ground takes on an average
	 * growing-season day.
	 *
	 * One denominator for every slice, rather than each slice against its own day:
	 * a ratio taken against the same day it was measured on is one on level ground
	 * in December as much as in June, and the seasons cancel straight back out of
	 * the channel. Against a fixed reference, level ground reads a quarter in
	 * midwinter and the growing-season mean still reads exactly one, which is the
	 * level everything downstream measures a slope's advantage against.
	 */
	void NormaliseAgainstGrowingSeason(TArray<FSunDay>& Year, FSliceRange GrowingSeason)
	{
		double ReferenceEnergy = 0.0;
		for (int32 TimeSlice = GrowingSeason.FirstSlice; TimeSlice <= GrowingSeason.LastSlice; ++TimeSlice)
		{
			ReferenceEnergy += Year[TimeSlice].LevelEnergy;
		}

		const double MeanEnergy =
			FMath::Max(ReferenceEnergy / GrowingSeason.Count(), UE_DOUBLE_KINDA_SMALL_NUMBER);
		for (FSunDay& Day : Year)
		{
			Day.StepWeightRadians /= MeanEnergy;
			Day.LevelEnergy /= MeanEnergy;
		}
	}

	TArray<FSunDay> SampleSeasonalYear(double LatitudeDegrees, const FMassifSunParams& Params)
	{
		const double LatitudeRadians = FMath::DegreesToRadians(LatitudeDegrees);

		TArray<FSunDay> Year;
		Year.Reserve(MassifSeasonalSliceCount);
		for (int32 TimeSlice = 0; TimeSlice < MassifSeasonalSliceCount; ++TimeSlice)
		{
			Year.Add(SampleSunDay(LatitudeRadians, SliceMidDayOfYear(TimeSlice), Params));
		}

		NormaliseAgainstGrowingSeason(Year, GrowingSeasonSlices(Params));
		return Year;
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

	/**
	 * The swept skyline at one stored bearing, in degrees.
	 *
	 * The nearest swept sample rather than a blend of two: the sweep count is a
	 * multiple of the stored count at every supported fidelity, so the nearest
	 * sample *is* that bearing, and averaging two skyline angles would report a
	 * ridge halfway between two that are actually there.
	 *
	 * An empty sweep is the cheap fidelity, which does not sweep at all. A level
	 * skyline is the honest answer for it, and the same one Off writes.
	 */
	double HorizonDegreesAt(TArrayView<const double> SweptRadians, int32 BearingIndex)
	{
		if (SweptRadians.IsEmpty())
		{
			return 0.0;
		}

		const int32 SweptIndex =
			FMath::RoundToInt32(double(BearingIndex) * SweptRadians.Num() / MassifHorizonAzimuthCount)
			% SweptRadians.Num();
		return FMath::RadiansToDegrees(SweptRadians[SweptIndex]);
	}

	/**
	 * Everything the solve fills before any of it is committed.
	 *
	 * Held whole rather than written straight into channel slices because the
	 * horizon a cell stands under feeds all twenty-four of that cell's slices, and
	 * the sweep that found it is the expensive part of the bake.
	 */
	struct FInsolationField
	{
		explicit FInsolationField(const FMassifChannelGrid& InGrid)
			: Grid(InGrid)
		{
			SkyView.SetNumZeroed(Grid.CellCount());
			SurfaceSkyFraction.SetNumZeroed(Grid.CellCount());
			DirectBySlice.SetNumZeroed(Grid.CellCount() * MassifSeasonalSliceCount);
			HorizonByBearing.SetNumZeroed(Grid.CellCount() * MassifHorizonAzimuthCount);
		}

		TArrayView<double> SliceOfDirect(int32 TimeSlice)
		{
			return TArrayView<double>(DirectBySlice.GetData() + TimeSlice * Grid.CellCount(), Grid.CellCount());
		}

		TArrayView<const double> SliceOfDirect(int32 TimeSlice) const
		{
			return TArrayView<const double>(
				DirectBySlice.GetData() + TimeSlice * Grid.CellCount(), Grid.CellCount());
		}

		TArrayView<double> SliceOfHorizon(int32 BearingIndex)
		{
			return TArrayView<double>(
				HorizonByBearing.GetData() + BearingIndex * Grid.CellCount(), Grid.CellCount());
		}

		TArrayView<const double> SliceOfHorizon(int32 BearingIndex) const
		{
			return TArrayView<const double>(
				HorizonByBearing.GetData() + BearingIndex * Grid.CellCount(), Grid.CellCount());
		}

		FMassifChannelGrid Grid;

		TArray<double> SkyView;

		/** Sky view narrowed by the surface's own tilt: the skylight before the season scales it. */
		TArray<double> SurfaceSkyFraction;

		/** Slice-major, so one slice is a contiguous run to commit. */
		TArray<double> DirectBySlice;

		/** Bearing-major, for the same reason. Degrees above level. */
		TArray<double> HorizonByBearing;
	};

	/** Copies one solved surface into one slice of a channel, scaled by what that season is worth. */
	void WriteSlice(
		FMassifEnvironmentContext& Context, const FName& ChannelName, int32 TimeSlice,
		TArrayView<const double> Values, double Scale)
	{
		const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(ChannelName, TimeSlice);
		TArrayView<double> Target = Scope->GetComponent(0);
		for (int32 CellIndex = 0; CellIndex < Target.Num(); ++CellIndex)
		{
			Target[CellIndex] = Values[CellIndex] * Scale;
		}
		Scope->Commit();
	}

	/**
	 * Every bearing level, which is what Off means for a skyline: open sky is
	 * nothing standing above the horizontal, and that is zero rather than the one
	 * the fraction channels write.
	 */
	void WriteLevelHorizon(FMassifEnvironmentContext& Context)
	{
		const FName& ChannelName = MassifEnvironmentChannels::Horizon();
		const FMassifChannelHandle Channel = Context.GetOutput(ChannelName);

		TArray<double> Level;
		Level.SetNumZeroed(Context.GetState().GetGrid(Channel).CellCount());
		for (int32 BearingIndex = 0; BearingIndex < MassifHorizonAzimuthCount; ++BearingIndex)
		{
			WriteSlice(Context, ChannelName, BearingIndex, Level, 1.0);
		}
	}

	/** Every slice of one channel as open sky over level ground, which is what Off means. */
	void WriteOpenSky(FMassifEnvironmentContext& Context, const FName& ChannelName)
	{
		const FMassifEnvironmentState& State = Context.GetState();
		const FMassifChannelHandle Channel = Context.GetOutput(ChannelName);

		TArray<double> Open;
		Open.Init(1.0, State.GetGrid(Channel).CellCount());
		for (int32 TimeSlice = 0; TimeSlice < State.GetDescriptor(Channel).TimeSliceCount; ++TimeSlice)
		{
			WriteSlice(Context, ChannelName, TimeSlice, Open, 1.0);
		}
	}

	void AddSliceShare(TArrayView<double> Mean, TArrayView<const double> Slice, double Share)
	{
		for (int32 CellIndex = 0; CellIndex < Mean.Num(); ++CellIndex)
		{
			Mean[CellIndex] += Slice[CellIndex] * Share;
		}
	}

	/** Mean of the growing-season slices, which is what the annual channel holds. */
	TArray<double> GrowingSeasonMean(const FInsolationField& Field, FSliceRange GrowingSeason)
	{
		TArray<double> Mean;
		Mean.SetNumZeroed(Field.Grid.CellCount());

		const double Share = 1.0 / GrowingSeason.Count();
		for (int32 TimeSlice = GrowingSeason.FirstSlice; TimeSlice <= GrowingSeason.LastSlice; ++TimeSlice)
		{
			AddSliceShare(Mean, Field.SliceOfDirect(TimeSlice), Share);
		}
		return Mean;
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
 * each half-month's sun actually reaches it.
 *
 * Everything is answered against a flat, unshaded surface at the same latitude
 * on an average growing-season day, so a value of 1 means "as much as level open
 * ground here in the middle of its season" and the numbers stay meaningful
 * without a radiative constant in them. The year is carried because at 47 degrees
 * north the January sun stands thirty degrees lower than the July one and a north
 * face takes almost none of it — the difference that decides which slopes are
 * still white in May.
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
	FMassifChannelDescriptor MakeFractionChannel(
		const FName& Name, EMassifChannelColorMap ColorMap, int32 TimeSliceCount) const;

	FMassifChannelDescriptor MakeHorizonChannel() const;

	void WriteUnlit(FMassifEnvironmentContext& Context) const;

	void SolveCell(
		const FTerrainWindow& Terrain, const TArray<FSunDay>& Year, const FMassifSunParams& RunParams,
		int32 Column, int32 Row, EMassifFidelity Fidelity, TArray<double>& HorizonScratch,
		FInsolationField& Field) const;

	void SolveRow(
		const FTerrainWindow& Terrain, const TArray<FSunDay>& Year, const FMassifSunParams& RunParams, int32 Row,
		EMassifFidelity Fidelity, FInsolationField& Field) const;

	void CommitField(
		FMassifEnvironmentContext& Context, const TArray<FSunDay>& Year, const FInsolationField& Field) const;

	FMassifSunParams Params;
};

FMassifChannelDescriptor FInsolationProcess::MakeFractionChannel(
	const FName& Name, EMassifChannelColorMap ColorMap, int32 TimeSliceCount) const
{
	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = Name;
	Descriptor.Type = EMassifChannelType::Scalar;
	Descriptor.Encoding = EMassifChannelEncoding::Uint16;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.TimeSliceCount = TimeSliceCount;
	Descriptor.Unit = TEXT("0-1");
	Descriptor.ColorMap = ColorMap;
	Descriptor.DisplayRange = FVector2d(0.0, 1.0);
	return Descriptor;
}

FMassifChannelDescriptor FInsolationProcess::MakeHorizonChannel() const
{
	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = MassifEnvironmentChannels::Horizon();
	Descriptor.Type = EMassifChannelType::Scalar;
	Descriptor.Encoding = EMassifChannelEncoding::Uint16;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.TimeSliceCount = MassifHorizonAzimuthCount;
	Descriptor.Unit = TEXT("degrees");
	Descriptor.Description = TEXT("Skyline elevation per bearing. Slices are bearings from north, not dates.");
	Descriptor.ColorMap = EMassifChannelColorMap::Viridis;

	// A skyline steeper than sixty degrees needs a wall directly overhead, so the
	// legend spends its range on the angles a massif actually produces.
	Descriptor.DisplayRange = FVector2d(0.0, 60.0);
	return Descriptor;
}

FMassifProcessDeclaration FInsolationProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::Insolation();
	Declaration.DisplayName = TEXT("Sunlight and Shadow");
	Declaration.InputChannels = {MassifEnvironmentChannels::Altitude()};
	Declaration.SupportedFidelities = {
		EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};

	// The sky view is a property of the terrain and carries no date, which is also
	// what keeps its horizon sweep outside the per-slice loop in Execute.
	Declaration.OutputChannels.Add(
		MakeFractionChannel(MassifEnvironmentChannels::SkyViewFactor(), EMassifChannelColorMap::Viridis, 1));
	Declaration.OutputChannels.Add(MakeFractionChannel(
		MassifEnvironmentChannels::DirectInsolation(), EMassifChannelColorMap::Magma, MassifSeasonalSliceCount));
	Declaration.OutputChannels.Add(MakeFractionChannel(
		MassifEnvironmentChannels::DiffuseInsolation(), EMassifChannelColorMap::Magma, MassifSeasonalSliceCount));
	Declaration.OutputChannels.Add(
		MakeFractionChannel(MassifEnvironmentChannels::AnnualInsolation(), EMassifChannelColorMap::Magma, 1));
	Declaration.OutputChannels.Add(MakeHorizonChannel());
	return Declaration;
}

void FInsolationProcess::WriteUnlit(FMassifEnvironmentContext& Context) const
{
	// Off is an open sky over level ground everywhere, in every slice: the channels
	// still exist, so nothing downstream has to ask whether the sun was solved.
	const FName Names[] = {
		MassifEnvironmentChannels::SkyViewFactor(), MassifEnvironmentChannels::DirectInsolation(),
		MassifEnvironmentChannels::DiffuseInsolation(), MassifEnvironmentChannels::AnnualInsolation()};

	for (const FName& Name : Names)
	{
		WriteOpenSky(Context, Name);
	}

	WriteLevelHorizon(Context);
}

void FInsolationProcess::SolveCell(
	const FTerrainWindow& Terrain, const TArray<FSunDay>& Year, const FMassifSunParams& RunParams, int32 Column,
	int32 Row, EMassifFidelity Fidelity, TArray<double>& HorizonScratch, FInsolationField& Field) const
{
	const FVector2d Position = Field.Grid.CellCentreMeters(Column, Row);
	const FVector3d Normal = Terrain.NormalAt(Position, Field.Grid.CellSizeMeters);

	// Swept once, then integrated against every slice: the skyline depends on the
	// terrain alone, and re-sweeping it per half-month would cost the bake
	// twenty-four sweeps for one answer.
	HorizonScratch.Reset();
	if (Fidelity != EMassifFidelity::Cheap)
	{
		SweepHorizon(Terrain, Position, Terrain.AltitudeAt(Position), RunParams, HorizonScratch);
	}

	const int32 CellIndex = Row * Field.Grid.Width + Column;
	Field.SkyView[CellIndex] = SkyViewFactorFrom(HorizonScratch);

	// Isotropic sky: a tilted surface sees less of it, and a hemmed-in one less again.
	Field.SurfaceSkyFraction[CellIndex] = Field.SkyView[CellIndex] * 0.5 * (1.0 + Normal.Z);

	for (int32 BearingIndex = 0; BearingIndex < MassifHorizonAzimuthCount; ++BearingIndex)
	{
		Field.SliceOfHorizon(BearingIndex)[CellIndex] = HorizonDegreesAt(HorizonScratch, BearingIndex);
	}

	for (int32 TimeSlice = 0; TimeSlice < Year.Num(); ++TimeSlice)
	{
		Field.SliceOfDirect(TimeSlice)[CellIndex] = ReceivedDayEnergy(Year[TimeSlice], Normal, HorizonScratch);
	}
}

void FInsolationProcess::SolveRow(
	const FTerrainWindow& Terrain, const TArray<FSunDay>& Year, const FMassifSunParams& RunParams, int32 Row,
	EMassifFidelity Fidelity, FInsolationField& Field) const
{
	TArray<double> HorizonScratch;
	for (int32 Column = 0; Column < Field.Grid.Width; ++Column)
	{
		SolveCell(Terrain, Year, RunParams, Column, Row, Fidelity, HorizonScratch, Field);
	}
}

void FInsolationProcess::CommitField(
	FMassifEnvironmentContext& Context, const TArray<FSunDay>& Year, const FInsolationField& Field) const
{
	const TArray<double> AnnualMean = GrowingSeasonMean(Field, GrowingSeasonSlices(Params));
	WriteSlice(Context, MassifEnvironmentChannels::SkyViewFactor(), 0, Field.SkyView, 1.0);
	WriteSlice(Context, MassifEnvironmentChannels::AnnualInsolation(), 0, AnnualMean, 1.0);

	for (int32 BearingIndex = 0; BearingIndex < MassifHorizonAzimuthCount; ++BearingIndex)
	{
		WriteSlice(
			Context, MassifEnvironmentChannels::Horizon(), BearingIndex, Field.SliceOfHorizon(BearingIndex),
			1.0);
	}

	for (int32 TimeSlice = 0; TimeSlice < Year.Num(); ++TimeSlice)
	{
		WriteSlice(
			Context, MassifEnvironmentChannels::DirectInsolation(), TimeSlice, Field.SliceOfDirect(TimeSlice),
			1.0);

		// The sky is isotropic, so a surface's share of it does not turn over with the
		// date; what does is how much skylight there is to take, which is what level
		// open ground took on that day.
		WriteSlice(
			Context, MassifEnvironmentChannels::DiffuseInsolation(), TimeSlice, Field.SurfaceSkyFraction,
			Year[TimeSlice].LevelEnergy);
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
	const TArray<FSunDay> Year = SampleSeasonalYear(Context.GetGeoreference().LatitudeDegrees, RunParams);

	FInsolationField Field(
		Context.GetState().GetGrid(Context.GetOutput(MassifEnvironmentChannels::DirectInsolation())));
	for (int32 Row = 0; Row < Field.Grid.Height; ++Row)
	{
		SolveRow(Terrain, Year, RunParams, Row, Fidelity, Field);
	}

	CommitField(Context, Year, Field);
}

void MassifRegisterSunProcesses(FMassifEnvironmentScheduler& Scheduler, const FMassifSunParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FInsolationProcess>(Params));
}
