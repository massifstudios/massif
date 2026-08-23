#include "MassifElevationSource.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifClimateProcesses.h"
#include "MassifPrecipitationProcesses.h"
#include "MassifVegetationProcesses.h"
#include "MassifWindProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 PrecipTestSeed = 4711;
	constexpr double PrecipTestWorldMeters = 4096.0;

	/** Half-monthly slices of a representative year, as the channels declare them. */
	constexpr int32 PrecipTestSliceCount = 24;

	/**
	 * Temperature the flat-world rig is held at: warm enough that the air asks for
	 * water, cool enough that it asks for far less than falls, so neither limit on
	 * the evaporation hides the other.
	 */
	constexpr double PrecipFlatTestCelsius = 8.0;

	/** The wind bearing is pinned in every test: a seed-drawn one makes windward unreadable. */
	FMassifEnvironmentParams MakePrecipParams(
		double SynopticBearingDegrees,
		const FMassifPrecipitationParams& PrecipitationParams = FMassifPrecipitationParams())
	{
		FMassifEnvironmentParams Params;
		Params.Water.CellSizeMeters = 32.0;

		// Soil is irrelevant here, and the droplets are the expensive part.
		Params.Water.DropletsPerCell = 0.05;
		Params.Water.MaxDropletSteps = 8;
		Params.Wind.SynopticBearingDegrees = SynopticBearingDegrees;
		Params.Precipitation = PrecipitationParams;
		return Params;
	}

	FMassifAnalyticElevationSource::FParams MakePrecipTerrain(
		EMassifAnalyticTerrain Shape, double BearingDegrees, double ReliefMeters)
	{
		FMassifAnalyticElevationSource::FParams Params;
		Params.Shape = Shape;
		Params.WorldSizeMeters = PrecipTestWorldMeters;
		Params.BaseAltitudeMeters = 1200.0;
		Params.ReliefMeters = ReliefMeters;
		Params.BearingDegrees = BearingDegrees;
		return Params;
	}

	/**
	 * The whole standard set, which precipitation is part of.
	 *
	 * It reads the wind and the temperature, and the scheduler refuses a graph
	 * whose inputs no registered process produces.
	 */
	TUniquePtr<FMassifEnvironmentScheduler> SolvePrecipitation(
		const FMassifAnalyticElevationSource::FParams& TerrainParams, EMassifFidelity PrecipFidelity,
		double SynopticBearingDegrees,
		const FMassifPrecipitationParams& PrecipitationParams = FMassifPrecipitationParams())
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), FMassifGeoreference(), PrecipTestSeed);

		MassifRegisterEnvironmentProcesses(
			*Scheduler, MakePrecipParams(SynopticBearingDegrees, PrecipitationParams));

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->SetFidelity(MassifEnvironmentProcesses::Precipitation(), PrecipFidelity);
		Scheduler->Execute();
		return Scheduler;
	}

	double SamplePrecipChannel(
		const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName, FVector2d PositionMeters)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		return State.SampleScalar(State.FindChannel(ChannelName), PositionMeters);
	}

	/** All a uint16 channel can be asked to hold, so nothing here guesses a tolerance. */
	double PrecipQuantisationStep(const FMassifEnvironmentScheduler& Scheduler, const FName& ChannelName)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		const FMassifChannelHandle Channel = State.FindChannel(ChannelName);
		const double Span = State.GetChannelMaximum(Channel) - State.GetChannelMinimum(Channel);
		return FMath::Max(Span / 65535.0, UE_DOUBLE_KINDA_SMALL_NUMBER);
	}

	/** One cell of the precipitation grid, which every channel this file reads is solved on. */
	struct FPrecipCell
	{
		int32 Column = INDEX_NONE;
		int32 Row = INDEX_NONE;

		bool IsValid() const
		{
			return Column != INDEX_NONE;
		}
	};

	FPrecipCell PrecipCellOfIndex(const FMassifChannelGrid& Grid, int32 CellIndex)
	{
		FPrecipCell Cell;
		Cell.Column = CellIndex % Grid.Width;
		Cell.Row = CellIndex / Grid.Width;
		return Cell;
	}

	/** What the whole year puts on one cell, half-month by half-month. */
	double PrecipSeasonalSumMillimetres(const FMassifEnvironmentState& State, FPrecipCell Cell)
	{
		const FMassifChannelHandle Seasonal =
			State.FindChannel(MassifEnvironmentChannels::SeasonalPrecipitation());

		double TotalMillimetres = 0.0;
		for (int32 TimeSlice = 0; TimeSlice < PrecipTestSliceCount; ++TimeSlice)
		{
			TotalMillimetres += State.ReadCell(Seasonal, Cell.Column, Cell.Row, 0, TimeSlice);
		}
		return TotalMillimetres;
	}

	/** The worst cell in the world, so the year is asserted everywhere rather than at a chosen point. */
	double LargestPrecipSeasonalSumErrorMillimetres(const FMassifEnvironmentScheduler& Scheduler)
	{
		const FMassifEnvironmentState& State = Scheduler.GetState();
		const FMassifChannelHandle Annual = State.FindChannel(MassifEnvironmentChannels::AnnualPrecipitation());
		const FMassifChannelGrid& Grid = State.GetGrid(Annual);

		double LargestMillimetres = 0.0;
		for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
		{
			const FPrecipCell Cell = PrecipCellOfIndex(Grid, CellIndex);
			const double AnnualMillimetres = State.ReadCell(Annual, Cell.Column, Cell.Row);
			LargestMillimetres = FMath::Max(
				LargestMillimetres, FMath::Abs(PrecipSeasonalSumMillimetres(State, Cell) - AnnualMillimetres));
		}
		return LargestMillimetres;
	}

	/** The model's own turn from rain to snow, written out here rather than called. */
	double ExpectedPrecipSnowFraction(double TemperatureCelsius)
	{
		const FMassifPrecipitationParams Defaults;
		const double AboveFreezing = TemperatureCelsius - Defaults.FreezingCelsius;
		return FMath::Clamp(0.5 - AboveFreezing / (2.0 * Defaults.SnowTransitionCelsius), 0.0, 1.0);
	}

	/** The two ends of one point's year, found by reading the temperature rather than assuming it. */
	struct FPrecipSeasonalExtremes
	{
		int32 ColdestSlice = 0;
		int32 WarmestSlice = 0;
		double ColdestCelsius = TNumericLimits<double>::Max();
		double WarmestCelsius = TNumericLimits<double>::Lowest();
	};

	void ExtendPrecipSeasonalExtremes(FPrecipSeasonalExtremes& Extremes, int32 TimeSlice, double Celsius)
	{
		if (Celsius < Extremes.ColdestCelsius)
		{
			Extremes.ColdestCelsius = Celsius;
			Extremes.ColdestSlice = TimeSlice;
		}
		if (Celsius > Extremes.WarmestCelsius)
		{
			Extremes.WarmestCelsius = Celsius;
			Extremes.WarmestSlice = TimeSlice;
		}
	}

	FPrecipSeasonalExtremes ReadPrecipSeasonalExtremes(
		const FMassifEnvironmentState& State, FVector2d PositionMeters)
	{
		const FMassifChannelHandle Temperature =
			State.FindChannel(MassifEnvironmentChannels::SeasonalTemperature());

		FPrecipSeasonalExtremes Extremes;
		for (int32 TimeSlice = 0; TimeSlice < PrecipTestSliceCount; ++TimeSlice)
		{
			ExtendPrecipSeasonalExtremes(
				Extremes, TimeSlice, State.SampleScalar(Temperature, PositionMeters, TimeSlice));
		}
		return Extremes;
	}

	/** Whether this cell's year turns right over: all snow at its coldest, all rain at its warmest. */
	bool PrecipTurnsOverAcrossTheYear(
		const FMassifEnvironmentState& State, const FMassifChannelGrid& Grid, FPrecipCell Cell)
	{
		const FMassifChannelHandle Snow = State.FindChannel(MassifEnvironmentChannels::SnowFraction());
		const FPrecipSeasonalExtremes Extremes =
			ReadPrecipSeasonalExtremes(State, Grid.CellCentreMeters(Cell.Column, Cell.Row));
		return State.ReadCell(Snow, Cell.Column, Cell.Row, 0, Extremes.ColdestSlice) > 0.95
			&& State.ReadCell(Snow, Cell.Column, Cell.Row, 0, Extremes.WarmestSlice) < 0.05;
	}

	FPrecipCell FindPrecipCellThatTurnsOver(const FMassifEnvironmentState& State)
	{
		const FMassifChannelHandle Snow = State.FindChannel(MassifEnvironmentChannels::SnowFraction());
		const FMassifChannelGrid& Grid = State.GetGrid(Snow);
		for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
		{
			const FPrecipCell Cell = PrecipCellOfIndex(Grid, CellIndex);
			if (PrecipTurnsOverAcrossTheYear(State, Grid, Cell))
			{
				return Cell;
			}
		}
		return FPrecipCell();
	}

	/** How far one cell's year of snow fractions stands from the temperature it was drawn off. */
	double LargestPrecipSnowFractionDeviation(const FMassifEnvironmentState& State, FPrecipCell Cell)
	{
		const FMassifChannelHandle Snow = State.FindChannel(MassifEnvironmentChannels::SnowFraction());
		const FMassifChannelHandle Temperature =
			State.FindChannel(MassifEnvironmentChannels::SeasonalTemperature());
		const FVector2d Position = State.GetGrid(Snow).CellCentreMeters(Cell.Column, Cell.Row);

		double LargestDeviation = 0.0;
		for (int32 TimeSlice = 0; TimeSlice < PrecipTestSliceCount; ++TimeSlice)
		{
			const double Celsius = State.SampleScalar(Temperature, Position, TimeSlice);
			const double Fraction = State.ReadCell(Snow, Cell.Column, Cell.Row, 0, TimeSlice);
			LargestDeviation =
				FMath::Max(LargestDeviation, FMath::Abs(Fraction - ExpectedPrecipSnowFraction(Celsius)));
		}
		return LargestDeviation;
	}

	/** The two ways the water balance can be broken, measured over the whole world. */
	struct FPrecipEvapotranspirationScan
	{
		/** Water leaving a cell that never arrived on it. */
		double LargestOvershootMillimetres = 0.0;

		/** Water leaving ground the air is too cold to take any off, and how much ground that is. */
		double LargestFrozenMillimetres = 0.0;
		int32 FrozenCellCount = 0;
	};

	void ExtendPrecipEvapotranspirationScan(
		FPrecipEvapotranspirationScan& Scan, double EvaporatedMillimetres, double AnnualMillimetres,
		double TemperatureCelsius)
	{
		Scan.LargestOvershootMillimetres =
			FMath::Max(Scan.LargestOvershootMillimetres, EvaporatedMillimetres - AnnualMillimetres);
		if (TemperatureCelsius > FMassifPrecipitationParams().EvapotranspirationBaseCelsius)
		{
			return;
		}

		Scan.FrozenCellCount += 1;
		Scan.LargestFrozenMillimetres = FMath::Max(Scan.LargestFrozenMillimetres, EvaporatedMillimetres);
	}

	FPrecipEvapotranspirationScan ScanPrecipEvapotranspiration(const FMassifEnvironmentState& State)
	{
		const FMassifChannelHandle Evaporated =
			State.FindChannel(MassifEnvironmentChannels::Evapotranspiration());
		const FMassifChannelHandle Annual = State.FindChannel(MassifEnvironmentChannels::AnnualPrecipitation());
		const FMassifChannelHandle Temperature = State.FindChannel(MassifEnvironmentChannels::MeanTemperature());
		const FMassifChannelGrid& Grid = State.GetGrid(Evaporated);

		FPrecipEvapotranspirationScan Scan;
		for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
		{
			const FPrecipCell Cell = PrecipCellOfIndex(Grid, CellIndex);
			ExtendPrecipEvapotranspirationScan(
				Scan, State.ReadCell(Evaporated, Cell.Column, Cell.Row),
				State.ReadCell(Annual, Cell.Column, Cell.Row),
				State.SampleScalar(Temperature, Grid.CellCentreMeters(Cell.Column, Cell.Row)));
		}
		return Scan;
	}

	FMassifChannelDescriptor MakePrecipStubChannel(const FName& Name, const TCHAR* Unit, int32 TimeSliceCount)
	{
		FMassifChannelDescriptor Descriptor;
		Descriptor.Name = Name;
		Descriptor.Type = EMassifChannelType::Scalar;
		Descriptor.Encoding = EMassifChannelEncoding::Uint16;
		Descriptor.CellSizeMeters = FMassifPrecipitationParams().CellSizeMeters;
		Descriptor.TimeSliceCount = TimeSliceCount;
		Descriptor.Unit = Unit;
		return Descriptor;
	}

	void FillPrecipStubSlice(
		FMassifEnvironmentContext& Context, const FName& ChannelName, int32 TimeSlice, double Value)
	{
		const TUniquePtr<FMassifChannelWriteScope> Scope = Context.OpenOutput(ChannelName, TimeSlice);
		TArrayView<double> Values = Scope->GetComponent(0);
		for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
		{
			Values[CellIndex] = Value;
		}
		Scope->Commit();
	}

	/** A world at one temperature all year, so nothing but the ground varies under the evaporation. */
	class FPrecipFixedTemperatureProcess final : public IMassifEnvironmentProcess
	{
	public:
		explicit FPrecipFixedTemperatureProcess(double InCelsius)
			: Celsius(InCelsius)
		{
		}

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = FName(TEXT("Test.FixedTemperature"));
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(
				MakePrecipStubChannel(MassifEnvironmentChannels::MeanTemperature(), TEXT("C"), 1));
			Declaration.OutputChannels.Add(MakePrecipStubChannel(
				MassifEnvironmentChannels::SeasonalTemperature(), TEXT("C"), PrecipTestSliceCount));
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			FillPrecipStubSlice(Context, MassifEnvironmentChannels::MeanTemperature(), 0, Celsius);
			for (int32 TimeSlice = 0; TimeSlice < PrecipTestSliceCount; ++TimeSlice)
			{
				FillPrecipStubSlice(Context, MassifEnvironmentChannels::SeasonalTemperature(), TimeSlice, Celsius);
			}
		}

	private:
		double Celsius = 0.0;
	};

	double PrecipSplitCoverAtColumn(const FMassifChannelGrid& Grid, int32 Column)
	{
		if (Column < Grid.Width / 2)
		{
			return 0.0;
		}
		return 1.0;
	}

	/** Bare rock over the west half of the world and closed canopy over the east. */
	class FPrecipSplitVegetationCoverProcess final : public IMassifEnvironmentProcess
	{
	public:
		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = FName(TEXT("Test.SplitVegetationCover"));
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};

			FMassifChannelDescriptor Descriptor =
				MakePrecipStubChannel(MassifEnvironmentChannels::VegetationCover(), TEXT("0-1"), 1);
			Descriptor.DisplayRange = FVector2d(0.0, 1.0);
			Declaration.OutputChannels.Add(Descriptor);
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::VegetationCover());

			const FMassifChannelGrid& Grid = Scope->GetGrid();
			TArrayView<double> Cover = Scope->GetComponent(0);
			for (int32 CellIndex = 0; CellIndex < Cover.Num(); ++CellIndex)
			{
				Cover[CellIndex] = PrecipSplitCoverAtColumn(Grid, CellIndex % Grid.Width);
			}
			Scope->Commit();
		}
	};

	/**
	 * Precipitation over dead flat ground held at one temperature, with or without
	 * something growing on it.
	 *
	 * A subset of the graph rather than the standard set, because what is being
	 * asked is what the solve does when nothing in the graph produces a vegetation
	 * cover at all — which the standard set always does. Flat ground at a fixed
	 * temperature also leaves the rainfall and the evaporative demand identical
	 * everywhere, so the cover is the only thing separating two points.
	 */
	TUniquePtr<FMassifEnvironmentScheduler> SolveFlatPrecipitation(
		double TemperatureCelsius, bool bWithVegetation)
	{
		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(
				MakePrecipTerrain(EMassifAnalyticTerrain::Flat, 0.0, 0.0)),
			FMassifGeoreference(), PrecipTestSeed);

		FMassifEnvironmentBakeParams AltitudeParams;
		AltitudeParams.CellSizeMeters = 32.0;

		FMassifWindParams WindParams;
		WindParams.SynopticBearingDegrees = 90.0;

		MassifRegisterAltitudeProcess(*Scheduler, AltitudeParams);
		Scheduler->RegisterProcess(MakeShared<FPrecipFixedTemperatureProcess>(TemperatureCelsius));
		MassifRegisterWindProcesses(*Scheduler, WindParams);
		MassifRegisterPrecipitationProcesses(*Scheduler, FMassifPrecipitationParams());
		if (bWithVegetation)
		{
			Scheduler->RegisterProcess(MakeShared<FPrecipSplitVegetationCoverProcess>());
		}

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->Execute();
		return Scheduler;
	}

	/** The model's evaporation, written out here rather than called. */
	double ExpectedPrecipEvapotranspirationMillimetres(
		double TemperatureCelsius, double AnnualMillimetres, double VegetationCover)
	{
		const FMassifPrecipitationParams Defaults;
		const double AboveBase =
			FMath::Max(0.0, TemperatureCelsius - Defaults.EvapotranspirationBaseCelsius);
		const double Potential = Defaults.ReferenceEvapotranspirationMillimetres * AboveBase
			/ Defaults.EvapotranspirationReferenceCelsius;
		const double Scale = Defaults.BareGroundEvaporationFraction
			+ (1.0 - Defaults.BareGroundEvaporationFraction) * VegetationCover;
		return FMath::Min(Potential, AnnualMillimetres) * Scale;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationAltitudeTest,
	"Massif.WorldGen.Precipitation.Altitude",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationAltitudeTest::RunTest(const FString& Parameters)
{
	// Cheap is base plus the altitude gradient and nothing else, so the closed form
	// is exact and worth asserting against rather than a threshold.
	const TUniquePtr<FMassifEnvironmentScheduler> Cone =
		SolvePrecipitation(MakePrecipTerrain(EMassifAnalyticTerrain::Cone, 0.0, 1200.0),
			EMassifFidelity::Cheap, 90.0);

	const FMassifEnvironmentState& State = Cone->GetState();
	const FMassifChannelHandle Altitude = State.FindChannel(MassifEnvironmentChannels::Altitude());
	const FMassifChannelHandle Annual = State.FindChannel(MassifEnvironmentChannels::AnnualPrecipitation());

	// Both readings are taken on the precipitation grid's own cells: it is coarser
	// than the altitude it reads, so sampling the two at an arbitrary point
	// compares a cell value against an interpolation of a finer one.
	const FMassifChannelGrid& Grid = State.GetGrid(Annual);
	const FVector2d Summit = Grid.CellCentreMeters(Grid.Width / 2, Grid.Height / 2);
	const FVector2d Flank = Grid.CellCentreMeters(Grid.Width - 4, Grid.Height / 2);

	const FMassifPrecipitationParams Defaults;
	const double AltitudeDifference =
		State.SampleScalar(Altitude, Summit) - State.SampleScalar(Altitude, Flank);
	const double Expected = Defaults.AltitudeGainMillimetresPerKilometre * AltitudeDifference / 1000.0;

	const double Measured = State.ReadCell(Annual, Grid.Width / 2, Grid.Height / 2)
		- State.ReadCell(Annual, Grid.Width - 4, Grid.Height / 2);

	TestTrue(
		FString::Printf(TEXT("Rainfall follows the altitude gradient (%.1f against %.1f mm)"), Measured, Expected),
		FMath::Abs(Measured - Expected)
			<= 2.0 * PrecipQuantisationStep(*Cone, MassifEnvironmentChannels::AnnualPrecipitation()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationRainShadowTest,
	"Massif.WorldGen.Precipitation.RainShadow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationRainShadowTest::RunTest(const FString& Parameters)
{
	// A ridge across the wind: air climbs the windward flank and drops its water
	// there, and the lee is dry because of what happened on the other side rather
	// than because of anything about the lee itself. Both flanks are at the same
	// altitude, so the altitude term cancels exactly and only the orography is left.
	const TUniquePtr<FMassifEnvironmentScheduler> Ridge =
		SolvePrecipitation(MakePrecipTerrain(EMassifAnalyticTerrain::Ridge, 90.0, 1200.0),
			EMassifFidelity::Standard, 0.0);

	// The synoptic bearing is where the wind comes from, so a northerly blows
	// towards -Y: the north flank is windward and the south flank is in the lee.
	const FVector2d Windward(0.0, 1000.0);
	const FVector2d Lee(0.0, -1000.0);

	const FMassifEnvironmentState& State = Ridge->GetState();
	const FMassifChannelHandle Altitude = State.FindChannel(MassifEnvironmentChannels::Altitude());
	TestTrue(
		TEXT("The two flanks sit at the same altitude"),
		FMath::Abs(State.SampleScalar(Altitude, Windward) - State.SampleScalar(Altitude, Lee)) < 1.0);

	const double WindwardRain =
		SamplePrecipChannel(*Ridge, MassifEnvironmentChannels::AnnualPrecipitation(), Windward);
	const double LeeRain = SamplePrecipChannel(*Ridge, MassifEnvironmentChannels::AnnualPrecipitation(), Lee);

	TestTrue(
		FString::Printf(TEXT("The windward flank takes more than the lee (%.0f against %.0f mm)"),
			WindwardRain, LeeRain),
		WindwardRain > LeeRain);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationSnowFractionTest,
	"Massif.WorldGen.Precipitation.SnowFraction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationSnowFractionTest::RunTest(const FString& Parameters)
{
	// The freezing level is what the split is about: everything well above it is
	// snow, everything well below it is rain, and the fraction never leaves 0-1.
	const TUniquePtr<FMassifEnvironmentScheduler> Cone =
		SolvePrecipitation(MakePrecipTerrain(EMassifAnalyticTerrain::Cone, 0.0, 3000.0),
			EMassifFidelity::Standard, 90.0);

	const FMassifEnvironmentState& State = Cone->GetState();
	const FMassifChannelHandle SnowFraction = State.FindChannel(MassifEnvironmentChannels::SnowFraction());
	const FMassifChannelHandle Temperature = State.FindChannel(MassifEnvironmentChannels::MeanTemperature());

	TestTrue(
		FString::Printf(TEXT("The snow fraction stays inside 0-1 (%.3f to %.3f)"),
			State.GetChannelMinimum(SnowFraction), State.GetChannelMaximum(SnowFraction)),
		State.GetChannelMinimum(SnowFraction) >= 0.0 && State.GetChannelMaximum(SnowFraction) <= 1.0);

	const FVector2d Summit = FVector2d::ZeroVector;
	const FVector2d Foot(1900.0, 0.0);
	TestTrue(
		TEXT("The summit is the colder of the two"),
		State.SampleScalar(Temperature, Summit) < State.SampleScalar(Temperature, Foot));
	TestTrue(
		FString::Printf(TEXT("And it takes the greater share as snow (%.3f against %.3f)"),
			State.SampleScalar(SnowFraction, Summit), State.SampleScalar(SnowFraction, Foot)),
		State.SampleScalar(SnowFraction, Summit) > State.SampleScalar(SnowFraction, Foot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationSeasonalSumTest,
	"Massif.WorldGen.Precipitation.SeasonalSum",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationSeasonalSumTest::RunTest(const FString& Parameters)
{
	// The shape of the year decides when the water arrives and must never decide
	// how much of it there is: whatever the amplitude, including none at all, the
	// twenty-four half-months have to add back up to the annual climatology that
	// everything downstream reads.
	const double Amplitudes[] = {0.0, 0.35, 0.9};
	for (double Amplitude : Amplitudes)
	{
		FMassifPrecipitationParams PrecipitationParams;
		PrecipitationParams.SeasonalAmplitude = Amplitude;

		const TUniquePtr<FMassifEnvironmentScheduler> Cone = SolvePrecipitation(
			MakePrecipTerrain(EMassifAnalyticTerrain::Cone, 0.0, 1200.0), EMassifFidelity::Standard, 90.0,
			PrecipitationParams);

		const double SliceStep =
			PrecipQuantisationStep(*Cone, MassifEnvironmentChannels::SeasonalPrecipitation());
		const double ToleranceMillimetres =
			PrecipQuantisationStep(*Cone, MassifEnvironmentChannels::AnnualPrecipitation())
			+ PrecipTestSliceCount * SliceStep;
		const double LargestMillimetres = LargestPrecipSeasonalSumErrorMillimetres(*Cone);

		TestTrue(
			FString::Printf(
				TEXT("The year sums to the annual total at amplitude %.2f (worst cell off by %.4f mm)"),
				Amplitude, LargestMillimetres),
			LargestMillimetres <= ToleranceMillimetres);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationSeasonalCycleTest,
	"Massif.WorldGen.Precipitation.SeasonalCycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationSeasonalCycleTest::RunTest(const FString& Parameters)
{
	// An Alpine massif takes its water off summer convection, not off winter
	// fronts. Half a year apart the cosine is at its two ends, so the ratio between
	// the wettest and the driest half-month is (1+A)/(1-A) and nothing else.
	const TUniquePtr<FMassifEnvironmentScheduler> Cone = SolvePrecipitation(
		MakePrecipTerrain(EMassifAnalyticTerrain::Cone, 0.0, 1200.0), EMassifFidelity::Standard, 90.0);

	const FMassifEnvironmentState& State = Cone->GetState();
	const FMassifChannelHandle Seasonal =
		State.FindChannel(MassifEnvironmentChannels::SeasonalPrecipitation());
	const FMassifChannelGrid& Grid = State.GetGrid(Seasonal);

	const FMassifPrecipitationParams Defaults;
	const int32 DriestSlice = (Defaults.WettestSlice + PrecipTestSliceCount / 2) % PrecipTestSliceCount;
	const FPrecipCell Cell = PrecipCellOfIndex(Grid, Grid.CellCount() / 2 + Grid.Width / 4);

	const double WetMillimetres = State.ReadCell(Seasonal, Cell.Column, Cell.Row, 0, Defaults.WettestSlice);
	const double DryMillimetres = State.ReadCell(Seasonal, Cell.Column, Cell.Row, 0, DriestSlice);
	TestTrue(
		FString::Printf(TEXT("High summer is the wetter half-month (%.2f against %.2f mm)"),
			WetMillimetres, DryMillimetres),
		WetMillimetres > DryMillimetres);

	const double ExpectedRatio =
		(1.0 + Defaults.SeasonalAmplitude) / (1.0 - Defaults.SeasonalAmplitude);
	const double MeasuredRatio = WetMillimetres / DryMillimetres;

	// Both readings carry half a step of the channel's own quantisation, which is
	// all the ratio is allowed to be wrong by.
	const double StepMillimetres =
		PrecipQuantisationStep(*Cone, MassifEnvironmentChannels::SeasonalPrecipitation());
	const double RatioTolerance =
		MeasuredRatio * (StepMillimetres / WetMillimetres + StepMillimetres / DryMillimetres);
	TestTrue(
		FString::Printf(TEXT("And by the amplitude's own ratio (%.5f against %.5f)"),
			MeasuredRatio, ExpectedRatio),
		FMath::Abs(MeasuredRatio - ExpectedRatio) <= RatioTolerance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationSeasonalSnowFractionTest,
	"Massif.WorldGen.Precipitation.SeasonalSnowFraction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationSeasonalSnowFractionTest::RunTest(const FString& Parameters)
{
	// The reason the fraction is sliced at all: one point takes snow in January and
	// rain in July, which no annual mean can say. Three kilometres of relief puts a
	// belt of the mountain across the freezing level for part of the year.
	const TUniquePtr<FMassifEnvironmentScheduler> Cone = SolvePrecipitation(
		MakePrecipTerrain(EMassifAnalyticTerrain::Cone, 0.0, 3000.0), EMassifFidelity::Standard, 90.0);

	const FMassifEnvironmentState& State = Cone->GetState();
	const FMassifChannelHandle Snow = State.FindChannel(MassifEnvironmentChannels::SnowFraction());
	const FMassifChannelGrid& Grid = State.GetGrid(Snow);

	const FPrecipCell Cell = FindPrecipCellThatTurnsOver(State);
	TestTrue(TEXT("Somewhere on the mountain the year turns from snow to rain and back"), Cell.IsValid());
	if (!Cell.IsValid())
	{
		return false;
	}

	const FPrecipSeasonalExtremes Extremes =
		ReadPrecipSeasonalExtremes(State, Grid.CellCentreMeters(Cell.Column, Cell.Row));
	TestTrue(
		FString::Printf(TEXT("That point's coldest half-month is below the turn (%.2f C)"),
			Extremes.ColdestCelsius),
		Extremes.ColdestCelsius < FMassifPrecipitationParams().FreezingCelsius);
	TestTrue(
		FString::Printf(TEXT("And its warmest is above it (%.2f C)"), Extremes.WarmestCelsius),
		Extremes.WarmestCelsius > FMassifPrecipitationParams().FreezingCelsius);
	TestTrue(
		FString::Printf(TEXT("Its winter fall is snow (%.3f) and its summer fall is rain (%.3f)"),
			State.ReadCell(Snow, Cell.Column, Cell.Row, 0, Extremes.ColdestSlice),
			State.ReadCell(Snow, Cell.Column, Cell.Row, 0, Extremes.WarmestSlice)),
		State.ReadCell(Snow, Cell.Column, Cell.Row, 0, Extremes.ColdestSlice)
			> State.ReadCell(Snow, Cell.Column, Cell.Row, 0, Extremes.WarmestSlice));

	// Every slice against the half-monthly temperature it was drawn off, so the
	// fraction is shown to follow the year rather than merely to differ across it.
	const double LargestDeviation = LargestPrecipSnowFractionDeviation(State, Cell);
	TestTrue(
		FString::Printf(TEXT("Every half-month follows its own temperature (worst slice off by %.6f)"),
			LargestDeviation),
		LargestDeviation <= PrecipQuantisationStep(*Cone, MassifEnvironmentChannels::SnowFraction()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationEvapotranspirationTest,
	"Massif.WorldGen.Precipitation.Evapotranspiration",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationEvapotranspirationTest::RunTest(const FString& Parameters)
{
	// Potential is not actual: the air cannot take back water that never fell, and
	// it takes nothing at all off ground it is too cold to lift any from.
	const TUniquePtr<FMassifEnvironmentScheduler> Cone = SolvePrecipitation(
		MakePrecipTerrain(EMassifAnalyticTerrain::Cone, 0.0, 3000.0), EMassifFidelity::Standard, 90.0);

	const FPrecipEvapotranspirationScan Scan = ScanPrecipEvapotranspiration(Cone->GetState());
	const double ToleranceMillimetres =
		PrecipQuantisationStep(*Cone, MassifEnvironmentChannels::Evapotranspiration())
		+ PrecipQuantisationStep(*Cone, MassifEnvironmentChannels::AnnualPrecipitation());

	TestTrue(
		FString::Printf(TEXT("Nowhere evaporates more than fell on it (worst cell over by %.4f mm)"),
			Scan.LargestOvershootMillimetres),
		Scan.LargestOvershootMillimetres <= ToleranceMillimetres);

	TestTrue(
		FString::Printf(TEXT("The mountain has ground at or below the base temperature (%d cells)"),
			Scan.FrozenCellCount),
		Scan.FrozenCellCount > 0);
	TestTrue(
		FString::Printf(TEXT("And none of it gives anything back (worst cell at %.4f mm)"),
			Scan.LargestFrozenMillimetres),
		Scan.LargestFrozenMillimetres
			<= PrecipQuantisationStep(*Cone, MassifEnvironmentChannels::Evapotranspiration()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationBareGroundEvaporationTest,
	"Massif.WorldGen.Precipitation.BareGroundEvaporation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationBareGroundEvaporationTest::RunTest(const FString& Parameters)
{
	// Flat ground at one temperature: the same rain falls on both halves of the
	// world and the same demand stands over them, so what is growing is the only
	// thing left to separate them.
	const TUniquePtr<FMassifEnvironmentScheduler> Flat = SolveFlatPrecipitation(PrecipFlatTestCelsius, true);
	const FMassifEnvironmentState& State = Flat->GetState();

	const FMassifChannelHandle Evaporated =
		State.FindChannel(MassifEnvironmentChannels::Evapotranspiration());
	const FMassifChannelGrid& Grid = State.GetGrid(Evaporated);
	const FVector2d WestPosition = Grid.CellCentreMeters(Grid.Width / 4, Grid.Height / 2);
	const FVector2d EastPosition = Grid.CellCentreMeters(3 * Grid.Width / 4, Grid.Height / 2);

	// Which side is the bare one is read out of the cover channel, not assumed.
	const double WestCover =
		SamplePrecipChannel(*Flat, MassifEnvironmentChannels::VegetationCover(), WestPosition);
	const double EastCover =
		SamplePrecipChannel(*Flat, MassifEnvironmentChannels::VegetationCover(), EastPosition);
	TestTrue(
		FString::Printf(TEXT("One side is bare and the other is covered (%.2f against %.2f)"),
			WestCover, EastCover),
		EastCover > WestCover);

	const double WestAnnual =
		SamplePrecipChannel(*Flat, MassifEnvironmentChannels::AnnualPrecipitation(), WestPosition);
	const double EastAnnual =
		SamplePrecipChannel(*Flat, MassifEnvironmentChannels::AnnualPrecipitation(), EastPosition);
	TestEqual(TEXT("The same rain falls on both"), WestAnnual, EastAnnual);

	const double BareMillimetres = State.SampleScalar(Evaporated, WestPosition);
	const double CoveredMillimetres = State.SampleScalar(Evaporated, EastPosition);
	TestTrue(
		FString::Printf(TEXT("Bare ground gives back less than vegetated ground (%.2f against %.2f mm)"),
			BareMillimetres, CoveredMillimetres),
		BareMillimetres < CoveredMillimetres);

	const double ToleranceMillimetres =
		PrecipQuantisationStep(*Flat, MassifEnvironmentChannels::Evapotranspiration());
	const double ExpectedBare =
		ExpectedPrecipEvapotranspirationMillimetres(PrecipFlatTestCelsius, WestAnnual, WestCover);
	const double ExpectedCovered =
		ExpectedPrecipEvapotranspirationMillimetres(PrecipFlatTestCelsius, EastAnnual, EastCover);
	TestTrue(
		FString::Printf(TEXT("Bare ground stands where its cover puts it (%.3f against %.3f mm)"),
			BareMillimetres, ExpectedBare),
		FMath::Abs(BareMillimetres - ExpectedBare) <= ToleranceMillimetres);
	TestTrue(
		FString::Printf(TEXT("And so does the covered ground (%.3f against %.3f mm)"),
			CoveredMillimetres, ExpectedCovered),
		FMath::Abs(CoveredMillimetres - ExpectedCovered) <= ToleranceMillimetres);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationWithoutVegetationTest,
	"Massif.WorldGen.Precipitation.WithoutVegetation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationWithoutVegetationTest::RunTest(const FString& Parameters)
{
	// The cover is an optional input because vegetation reads the water balance
	// this closes. A graph with nothing growing in it must still solve, and must
	// evaporate off bare ground rather than off nothing.
	const TUniquePtr<FMassifEnvironmentScheduler> Flat = SolveFlatPrecipitation(PrecipFlatTestCelsius, false);
	const FMassifEnvironmentState& State = Flat->GetState();

	TestFalse(
		TEXT("Nothing in this graph produces a vegetation cover"),
		State.FindChannel(MassifEnvironmentChannels::VegetationCover()).IsValid());

	const FMassifChannelHandle Evaporated =
		State.FindChannel(MassifEnvironmentChannels::Evapotranspiration());
	TestTrue(TEXT("The evapotranspiration channel exists"), Evaporated.IsValid());
	TestTrue(TEXT("And is written"), State.IsChannelWritten(Evaporated));

	const double AnnualMillimetres =
		SamplePrecipChannel(*Flat, MassifEnvironmentChannels::AnnualPrecipitation(), FVector2d::ZeroVector);
	const double Measured = State.SampleScalar(Evaporated, FVector2d::ZeroVector);
	const double Expected =
		ExpectedPrecipEvapotranspirationMillimetres(PrecipFlatTestCelsius, AnnualMillimetres, 0.0);
	TestTrue(
		FString::Printf(TEXT("At the bare-ground share of the potential (%.3f against %.3f mm)"),
			Measured, Expected),
		FMath::Abs(Measured - Expected)
			<= PrecipQuantisationStep(*Flat, MassifEnvironmentChannels::Evapotranspiration()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPrecipitationOffTest,
	"Massif.WorldGen.Precipitation.Off",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPrecipitationOffTest::RunTest(const FString& Parameters)
{
	// Off is flat weather, not an absent channel: the same rain everywhere whatever
	// the ground does, so nothing downstream asks whether it rained.
	const TUniquePtr<FMassifEnvironmentScheduler> Flat =
		SolvePrecipitation(MakePrecipTerrain(EMassifAnalyticTerrain::Cone, 0.0, 1200.0),
			EMassifFidelity::Off, 90.0);
	const FMassifEnvironmentState& State = Flat->GetState();

	const FMassifChannelHandle Annual = State.FindChannel(MassifEnvironmentChannels::AnnualPrecipitation());
	const FMassifChannelHandle Seasonal =
		State.FindChannel(MassifEnvironmentChannels::SeasonalPrecipitation());
	const FMassifChannelHandle Snow = State.FindChannel(MassifEnvironmentChannels::SnowFraction());
	const FMassifChannelHandle Evaporated =
		State.FindChannel(MassifEnvironmentChannels::Evapotranspiration());
	TestTrue(TEXT("Annual precipitation is written at Off"), State.IsChannelWritten(Annual));
	TestTrue(TEXT("The seasonal precipitation is written at Off"), State.IsChannelWritten(Seasonal));
	TestTrue(TEXT("The snow fraction is written at Off"), State.IsChannelWritten(Snow));
	TestTrue(TEXT("The evapotranspiration is written at Off"), State.IsChannelWritten(Evaporated));

	const FMassifPrecipitationParams Defaults;
	TestEqual(
		TEXT("It rains the base amount at the summit"),
		State.SampleScalar(Annual, FVector2d::ZeroVector), Defaults.BaseAnnualMillimetres);
	TestEqual(
		TEXT("And the same on the flank, whatever the ground does"),
		State.SampleScalar(Annual, FVector2d(1400.0, 0.0)), Defaults.BaseAnnualMillimetres);
	// The phase is not part of what Off switches off. A world below freezing still
	// takes snow, because the snowpack reads this channel and "none of it is
	// frozen" would be a false claim rather than a trivial one - it would silently
	// leave the pack with nothing to build out of.
	const FMassifChannelHandle Temperature =
		State.FindChannel(MassifEnvironmentChannels::SeasonalTemperature());
	const bool bFreezingSomewhere = State.GetChannelMinimum(Temperature) <= Defaults.FreezingCelsius;
	TestEqual(
		FString::Printf(TEXT("Cold ground still takes snow at Off (coldest %.1f C)"),
			State.GetChannelMinimum(Temperature)),
		State.GetChannelMaximum(Snow) > 0.0, bFreezingSomewhere);
	TestEqual(TEXT("And none of it goes back into the air"), State.GetChannelMaximum(Evaporated), 0.0);

	// A year without a season is still a whole year: every half-month takes the
	// same share and they still add up to the annual total.
	const FMassifChannelGrid& Grid = State.GetGrid(Seasonal);
	const FPrecipCell Cell = PrecipCellOfIndex(Grid, Grid.CellCount() / 2);
	TestEqual(
		TEXT("Every half-month takes an equal share of the year"),
		State.ReadCell(Seasonal, Cell.Column, Cell.Row, 0, 0),
		Defaults.BaseAnnualMillimetres / PrecipTestSliceCount);
	TestEqual(
		TEXT("Which sums to the year"), PrecipSeasonalSumMillimetres(State, Cell), Defaults.BaseAnnualMillimetres);
	return true;
}
