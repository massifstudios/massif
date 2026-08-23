#include "MassifWindProcesses.h"

#include "MassifVegetationProcesses.h"

#include "MassifEnvironmentGraph.h"

namespace MassifEnvironmentChannels
{
	const FName& WindExposure()
	{
		static const FName Name(TEXT("Wind.Exposure"));
		return Name;
	}

	const FName& WindField()
	{
		static const FName Name(TEXT("Wind.Field"));
		return Name;
	}

	const FName& KatabaticStrength()
	{
		static const FName Name(TEXT("Wind.KatabaticStrength"));
		return Name;
	}

	const FName& FoehnWarming()
	{
		static const FName Name(TEXT("Wind.FoehnWarming"));
		return Name;
	}
}

namespace MassifEnvironmentProcesses
{
	const FName& Wind()
	{
		static const FName Name(TEXT("Wind.Field"));
		return Name;
	}
}

namespace
{
	/** The one wind the whole world is under before the terrain gets at it. */
	struct FSynopticWind
	{
		/** Into the fetch: the direction the bearing names, which is where the air comes from. */
		FVector2d UpwindDirection = FVector2d(0.0, 1.0);

		/** The direction the air travels, which is the reverse of the bearing. */
		FVector2d FlowDirection = FVector2d(0.0, -1.0);

		double SpeedMetersPerSecond = 0.0;
	};

	/** Wind at one point: how exposed the point is, and what the flow over it comes out as. */
	struct FWindSample
	{
		double Exposure = 1.0;
		FVector2d FieldMetersPerSecond = FVector2d::ZeroVector;
		double KatabaticStrength = 0.0;
		double FoehnWarmingCelsius = 0.0;
	};

	/**
	 * The channels the wind reads where the graph has them.
	 *
	 * Both are optional: the wind is solved in graphs with no vegetation and in
	 * graphs with no water, and neither arrangement is a broken version of the
	 * other. An absent channel reads zero, which is the same as no canopy and no
	 * catchment.
	 */
	struct FWindInputs
	{
		const FMassifEnvironmentState& State;
		FMassifChannelHandle Canopy;
		FMassifChannelHandle FlowAccumulation;
		FMassifChannelHandle StandingWaterDepth;

		double CanopyAt(FVector2d PositionMeters) const
		{
			return MassifSampleOptional(State, Canopy, PositionMeters);
		}

		double AccumulationSquareMetersAt(FVector2d PositionMeters) const
		{
			return MassifSampleOptional(State, FlowAccumulation, PositionMeters);
		}

		/** Filled surface less measured: how deep a hollow the air has no way out of. */
		double BasinDepthMetersAt(FVector2d PositionMeters) const
		{
			return MassifSampleOptional(State, StandingWaterDepth, PositionMeters);
		}
	};

	/** Scratch for the four channels one pass over the grid fills. */
	struct FWindOutputs
	{
		TArrayView<double> Exposure;
		TArrayView<double> Eastward;
		TArrayView<double> Northward;
		TArrayView<double> KatabaticStrength;
		TArrayView<double> FoehnWarmingCelsius;
	};

	/** Horizontal direction of a compass bearing, in the frame the terrain is in: X east, Y north. */
	FVector2d BearingDirection(double BearingDegrees)
	{
		const double BearingRadians = FMath::DegreesToRadians(BearingDegrees);
		return FVector2d(FMath::Sin(BearingRadians), FMath::Cos(BearingRadians));
	}

	double SynopticBearingDegrees(const FMassifEnvironmentContext& Context, const FMassifWindParams& Params)
	{
		if (Params.SynopticBearingDegrees.IsSet())
		{
			return Params.SynopticBearingDegrees.GetValue();
		}

		FRandomStream Stream = Context.MakeStream(TEXT("Synoptic"));
		return Stream.FRandRange(0.0f, 360.0f);
	}

	FSynopticWind MakeSynopticWind(const FMassifEnvironmentContext& Context, const FMassifWindParams& Params)
	{
		FSynopticWind Wind;
		Wind.UpwindDirection = BearingDirection(SynopticBearingDegrees(Context, Params));
		Wind.FlowDirection = -Wind.UpwindDirection;
		Wind.SpeedMetersPerSecond = Params.SynopticSpeedMetersPerSecond;
		return Wind;
	}

	/** The terrain the sweep marches over, sampled from the altitude channel at this grid's spacing. */
	class FWindTerrain
	{
	public:
		FWindTerrain(const FMassifEnvironmentState& InState, FMassifChannelHandle InAltitude)
			: State(InState)
			, Altitude(InAltitude)
		{
		}

		double AltitudeAt(FVector2d PositionMeters) const
		{
			return State.SampleScalar(Altitude, PositionMeters);
		}

		/** Uphill gradient in rise per metre, differenced across one cell of the grid being solved. */
		FVector2d GradientAt(FVector2d PositionMeters, double SpacingMeters) const
		{
			const double East = AltitudeAt(PositionMeters + FVector2d(SpacingMeters, 0.0));
			const double West = AltitudeAt(PositionMeters - FVector2d(SpacingMeters, 0.0));
			const double North = AltitudeAt(PositionMeters + FVector2d(0.0, SpacingMeters));
			const double South = AltitudeAt(PositionMeters - FVector2d(0.0, SpacingMeters));
			return FVector2d((East - West) / (2.0 * SpacingMeters), (North - South) / (2.0 * SpacingMeters));
		}

	private:
		const FMassifEnvironmentState& State;
		FMassifChannelHandle Altitude;
	};

	/** What one march up the fetch measures. Both readings come off the same samples. */
	struct FUpwindFetch
	{
		/**
		 * Sx, the topographic exposure index: the steepest angle anything within
		 * the fetch subtends at the point.
		 *
		 * Positive means ground upwind stands above the point and takes the flow
		 * off it; negative means the point stands above everything it faces into,
		 * and the flow is compressed over it. This is the whole difference between
		 * a crest and the pocket behind it, and no local measure of slope can tell
		 * them apart.
		 */
		double ExposureAngleRadians = 0.0;

		/** Highest ground the flow crossed to get here, which is what it has descended from. */
		double HighestAltitudeMeters = 0.0;
	};

	FUpwindFetch SweepUpwindFetch(
		const FWindTerrain& Terrain, FVector2d PositionMeters, const FSynopticWind& Wind,
		const FMassifWindParams& Params)
	{
		const double GroundAltitudeMeters = Terrain.AltitudeAt(PositionMeters);
		const int32 StepCount = FMath::Max(1, Params.ExposureStepCount);

		double HighestTangent = TNumericLimits<double>::Lowest();
		double HighestAltitudeMeters = TNumericLimits<double>::Lowest();
		for (int32 StepIndex = 1; StepIndex <= StepCount; ++StepIndex)
		{
			const double DistanceMeters = Params.ExposureReachMeters * StepIndex / StepCount;
			const double AltitudeMeters =
				Terrain.AltitudeAt(PositionMeters + Wind.UpwindDirection * DistanceMeters);
			HighestAltitudeMeters = FMath::Max(HighestAltitudeMeters, AltitudeMeters);
			HighestTangent = FMath::Max(HighestTangent, (AltitudeMeters - GroundAltitudeMeters) / DistanceMeters);
		}

		FUpwindFetch Fetch;
		Fetch.ExposureAngleRadians = FMath::Atan(HighestTangent);
		Fetch.HighestAltitudeMeters = HighestAltitudeMeters;
		return Fetch;
	}

	/** Level open ground is 1; the clamp keeps a cliff edge from multiplying the wind without limit. */
	double ExposureFromAngle(double ExposureAngleRadians, const FMassifWindParams& Params)
	{
		return FMath::Clamp(1.0 - Params.ExposureSensitivity * ExposureAngleRadians, 0.0, 2.0);
	}

	/**
	 * Which of an axis's two signs the flow runs along.
	 *
	 * Flow exactly across the axis runs along neither, and that is the case on
	 * every flank the wind meets head-on. The tie is settled by the axis itself so
	 * the whole flank picks one side: left to the rounding of a dot product that is
	 * zero either way, neighbouring cells would steer opposite ways.
	 */
	bool AxisRunsWithFlow(FVector2d Axis, FVector2d FlowDirection)
	{
		const double Alignment = FVector2d::DotProduct(Axis, FlowDirection);
		if (FMath::Abs(Alignment) > UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return Alignment > 0.0;
		}
		if (FMath::Abs(Axis.Y) > UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return Axis.Y > 0.0;
		}
		return Axis.X >= 0.0;
	}

	/**
	 * The valley's axis: the direction the ground changes least in, signed to the
	 * side the flow already points along.
	 */
	FVector2d ValleyAxis(FVector2d Gradient, FVector2d FlowDirection)
	{
		const FVector2d Axis = FVector2d(-Gradient.Y, Gradient.X).GetSafeNormal();
		if (Axis.IsNearlyZero())
		{
			return FlowDirection;
		}
		if (!AxisRunsWithFlow(Axis, FlowDirection))
		{
			return -Axis;
		}
		return Axis;
	}

	/** How much of the direction the terrain takes over, from how confined the point is. */
	double ChannelingWeight(FVector2d Gradient, const FMassifWindParams& Params)
	{
		const double ReferenceSlope = FMath::Max(Params.ChannelingReferenceSlope, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double Slope = Gradient.Length();
		return Params.ChannelingStrength * Slope / (Slope + ReferenceSlope);
	}

	FVector2d BlendDirection(FVector2d Direction, FVector2d Target, double Weight)
	{
		const FVector2d Blended = FMath::Lerp(Direction, Target, Weight);
		if (Blended.IsNearlyZero())
		{
			return Direction;
		}
		return Blended.GetSafeNormal();
	}

	/**
	 * How far behind the skyline the point lies: nothing on ground facing an open
	 * fetch, one where the fetch has taken the wind off it entirely. Ground the
	 * flow meets head-on reads above 1 for exposure and so gets exactly zero here.
	 */
	double ShelterFraction(double Exposure)
	{
		return FMath::Clamp(1.0 - Exposure, 0.0, 1.0);
	}

	/**
	 * How completely the flow has left the ground here: steep ground falling away
	 * downwind, and a point already in shelter. Either alone is a slope, not a
	 * separation.
	 */
	double LeeSeparationStrength(
		const FWindTerrain& Terrain, FVector2d PositionMeters, const FSynopticWind& Wind, double Exposure,
		double SpacingMeters, const FMassifWindParams& Params)
	{
		const double DownwindAltitudeMeters =
			Terrain.AltitudeAt(PositionMeters + Wind.FlowDirection * SpacingMeters);
		const double FallSlope = (Terrain.AltitudeAt(PositionMeters) - DownwindAltitudeMeters) / SpacingMeters;
		const double SeparationSlope = FMath::Max(Params.LeeSeparationSlope, UE_DOUBLE_KINDA_SMALL_NUMBER);

		const double Fall = FMath::Clamp(FallSlope / SeparationSlope, 0.0, 1.0);
		return Fall * ShelterFraction(Exposure);
	}

	/**
	 * Cold air drains like water and pools where it can settle.
	 *
	 * The slope area above a point says how much arrives, which is the same shape
	 * the water solve already routed, so this reads that rather than routing a
	 * second time. The ground under the point says whether it stays: a steep gully
	 * has the air moving through it on its way down, and the floor below has it
	 * sitting. Both have to be true, so a crest with nothing above it reads zero
	 * however flat it is.
	 */
	double DrainageStrength(
		double AccumulationSquareMeters, double BasinDepthMeters, double SlopeTangent,
		const FMassifWindParams& Params)
	{
		const double ReferenceArea =
			FMath::Max(Params.DrainageReferenceAreaSquareMeters, UE_DOUBLE_KINDA_SMALL_NUMBER);
		const double MaxSlopeTangent = FMath::Max(
			FMath::Tan(FMath::DegreesToRadians(Params.DrainageMaxSlopeDegrees)), UE_DOUBLE_KINDA_SMALL_NUMBER);

		const double Collected = FMath::Clamp(AccumulationSquareMeters / ReferenceArea, 0.0, 1.0);
		const double Settling = 1.0 - FMath::Clamp(SlopeTangent / MaxSlopeTangent, 0.0, 1.0);

		// Two different places hold cold air, and one of them the routed water cannot
		// find. An open valley floor collects what drains off the slopes above it,
		// which is the accumulation term. A closed hollow holds air because the air
		// has nowhere to leave - and on a filled surface that hollow is an
		// accumulation *minimum*, since the fill routes everything out over its rim.
		// Basin depth is what says so, and a frost hollow is exactly the case.
		const double Trapped = FMath::Clamp(
			BasinDepthMeters / FMath::Max(Params.TrappingDepthMeters, UE_DOUBLE_KINDA_SMALL_NUMBER), 0.0, 1.0);

		// Settling applies to both: a hollow's steep flank is below the spill level
		// too, but the air there is on its way to the floor rather than lying on it.
		return FMath::Max(Collected, Trapped) * Settling;
	}

	/**
	 * Warming a descending lee flow brings, measured from the highest ground the
	 * upwind sweep crossed.
	 *
	 * Air that was forced over that ground left its water on the far side, so it
	 * comes down dry and warms at the dry adiabatic rate over the whole descent
	 * rather than at the moist rate it climbed at. The shelter scales it because a
	 * point the flow has not descended onto is not in a foehn at all, whatever
	 * stands upwind of it.
	 */
	double FoehnWarmingCelsius(
		const FUpwindFetch& Fetch, double GroundAltitudeMeters, double Exposure, const FMassifWindParams& Params)
	{
		const double DescentMeters = FMath::Max(0.0, Fetch.HighestAltitudeMeters - GroundAltitudeMeters);
		const double WarmingCelsius = FMath::Min(
			Params.DryAdiabaticLapseCelsiusPerMeter * DescentMeters, Params.MaxFoehnWarmingCelsius);
		return WarmingCelsius * ShelterFraction(Exposure);
	}

	/**
	 * Collapses the along-wind flow and turns a fraction of it back upwind, which
	 * is what makes the pocket behind a crest slow and recirculating rather than
	 * merely weak. What is left across the wind drifts along the crest.
	 */
	FVector2d SeparateInLee(
		FVector2d FieldMetersPerSecond, const FSynopticWind& Wind, double Strength, const FMassifWindParams& Params)
	{
		const double Along = FVector2d::DotProduct(FieldMetersPerSecond, Wind.FlowDirection);
		const FVector2d Across = FieldMetersPerSecond - Wind.FlowDirection * Along;
		const double SeparatedAlong = FMath::Lerp(Along, -Params.LeeReversalFraction * Along, Strength);
		return Wind.FlowDirection * SeparatedAlong + Across;
	}

	/** Channeling and lee separation: the two things that move the direction, not just the strength. */
	FVector2d SteerByTerrain(
		const FWindTerrain& Terrain, FVector2d PositionMeters, const FSynopticWind& Wind, const FWindSample& Sample,
		FVector2d Gradient, double SpacingMeters, const FMassifWindParams& Params)
	{
		const FVector2d Channeled = BlendDirection(
			Sample.FieldMetersPerSecond.GetSafeNormal(), ValleyAxis(Gradient, Wind.FlowDirection),
			ChannelingWeight(Gradient, Params));

		const double Separation =
			LeeSeparationStrength(Terrain, PositionMeters, Wind, Sample.Exposure, SpacingMeters, Params);
		return SeparateInLee(Channeled * Sample.FieldMetersPerSecond.Length(), Wind, Separation, Params);
	}

	/**
	 * Everything the terrain adds once the synoptic wind is known: where it steers
	 * the flow, where cold air pools on a still night, and where a descending flow
	 * arrives warm. All three need terrain, so all three are skipped together at
	 * the fidelity that does not steer.
	 */
	FWindSample SolveWindAt(
		const FWindTerrain& Terrain, FVector2d PositionMeters, const FSynopticWind& Wind, double SpacingMeters,
		bool bTerrainSteers, double AccumulationSquareMeters, double BasinDepthMeters,
		const FMassifWindParams& Params)
	{
		const FUpwindFetch Fetch = SweepUpwindFetch(Terrain, PositionMeters, Wind, Params);

		FWindSample Sample;
		Sample.Exposure = ExposureFromAngle(Fetch.ExposureAngleRadians, Params);
		Sample.FieldMetersPerSecond = Wind.FlowDirection * (Wind.SpeedMetersPerSecond * Sample.Exposure);
		if (!bTerrainSteers)
		{
			return Sample;
		}

		const FVector2d Gradient = Terrain.GradientAt(PositionMeters, SpacingMeters);
		Sample.FieldMetersPerSecond =
			SteerByTerrain(Terrain, PositionMeters, Wind, Sample, Gradient, SpacingMeters, Params);
		Sample.KatabaticStrength =
			DrainageStrength(AccumulationSquareMeters, BasinDepthMeters, Gradient.Length(), Params);
		Sample.FoehnWarmingCelsius =
			FoehnWarmingCelsius(Fetch, Terrain.AltitudeAt(PositionMeters), Sample.Exposure, Params);
		return Sample;
	}

	/** Reference sweeps the fetch twice as finely; the model itself is the same. */
	FMassifWindParams RefineForFidelity(const FMassifWindParams& Params, EMassifFidelity Fidelity)
	{
		if (Fidelity != EMassifFidelity::Reference)
		{
			return Params;
		}

		FMassifWindParams Refined = Params;
		Refined.ExposureStepCount = Params.ExposureStepCount * 2;
		return Refined;
	}
}

/**
 * Wind over the world: how exposed each point is to the prevailing wind, and
 * what the flow there actually is.
 *
 * The world gets one synoptic wind from its seed, and the terrain rewrites it:
 * compressed over ground that faces into an open fetch, taken away behind
 * anything that stands upwind, steered along the axis of whatever confines it,
 * and separated into a slow recirculating pocket where the ground drops away
 * downwind. Exposure is a multiplier on level open ground, so a reading stays
 * meaningful without believing any absolute wind speed.
 *
 * The two mountain wind systems the synoptic wind does not drive at all are
 * solved here as well, because both are read off work this pass already does:
 * the nightly cold-air drainage off the local slope and the catchment above it,
 * and the foehn warming off the same upwind sweep and the same shelter term the
 * lee separation needs. Splitting them out would mean marching the fetch twice.
 */
class FWindFieldProcess final : public IMassifEnvironmentProcess
{
public:
	explicit FWindFieldProcess(const FMassifWindParams& InParams)
		: Params(InParams)
	{
	}

	FMassifProcessDeclaration Declare() const override;
	void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override;

private:
	FMassifChannelDescriptor MakeExposureChannel() const;
	FMassifChannelDescriptor MakeFieldChannel() const;
	FMassifChannelDescriptor MakeKatabaticChannel() const;
	FMassifChannelDescriptor MakeFoehnChannel() const;

	void WriteSynoptic(FMassifEnvironmentContext& Context, const FSynopticWind& Wind) const;

	void SolveRow(
		const FWindTerrain& Terrain, const FSynopticWind& Wind, const FMassifWindParams& RunParams,
		const FMassifChannelGrid& Grid, int32 Row, EMassifFidelity Fidelity, const FWindInputs& Inputs,
		FWindOutputs& Outputs) const;

	FMassifWindParams Params;
};

FMassifChannelDescriptor FWindFieldProcess::MakeExposureChannel() const
{
	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = MassifEnvironmentChannels::WindExposure();
	Descriptor.Type = EMassifChannelType::Scalar;
	Descriptor.Encoding = EMassifChannelEncoding::Uint16;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.Unit = TEXT("0-2");
	Descriptor.ColorMap = EMassifChannelColorMap::Viridis;
	Descriptor.DisplayRange = FVector2d(0.0, 2.0);
	return Descriptor;
}

FMassifChannelDescriptor FWindFieldProcess::MakeFieldChannel() const
{
	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = MassifEnvironmentChannels::WindField();
	Descriptor.Type = EMassifChannelType::Vector2;
	Descriptor.Encoding = EMassifChannelEncoding::Float32;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.Unit = TEXT("m/s");
	Descriptor.ColorMap = EMassifChannelColorMap::Viridis;
	return Descriptor;
}

FMassifChannelDescriptor FWindFieldProcess::MakeKatabaticChannel() const
{
	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = MassifEnvironmentChannels::KatabaticStrength();
	Descriptor.Type = EMassifChannelType::Scalar;
	Descriptor.Encoding = EMassifChannelEncoding::Uint16;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.Unit = TEXT("0-1");
	Descriptor.ColorMap = EMassifChannelColorMap::BlueWhite;
	Descriptor.DisplayRange = FVector2d(0.0, 1.0);
	return Descriptor;
}

FMassifChannelDescriptor FWindFieldProcess::MakeFoehnChannel() const
{
	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = MassifEnvironmentChannels::FoehnWarming();
	Descriptor.Type = EMassifChannelType::Scalar;
	Descriptor.Encoding = EMassifChannelEncoding::Uint16;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.Unit = TEXT("C");
	Descriptor.ColorMap = EMassifChannelColorMap::Magma;
	return Descriptor;
}

FMassifProcessDeclaration FWindFieldProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::Wind();
	Declaration.InputChannels = {MassifEnvironmentChannels::Altitude()};

	// The catchment above a point is what cold air drains off, and the hollow it
	// cannot leave is what holds it there; the water solve has already worked out
	// both. Optional, so the wind still solves in a graph with no hydrology - there
	// is simply nothing draining and nothing trapped anywhere.
	Declaration.OptionalInputChannels = {
		MassifEnvironmentChannels::VegetationCanopy(), MassifEnvironmentChannels::FlowAccumulation(),
		MassifEnvironmentChannels::StandingWaterDepth()};

	// Vegetation reads the snow this wind moves, so the scheduler finds a cycle
	// through it and has to be told it is meant.
	Declaration.bParticipatesInFeedback = true;
	Declaration.SupportedFidelities = {
		EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};
	Declaration.OutputChannels.Add(MakeExposureChannel());
	Declaration.OutputChannels.Add(MakeFieldChannel());
	Declaration.OutputChannels.Add(MakeKatabaticChannel());
	Declaration.OutputChannels.Add(MakeFoehnChannel());
	return Declaration;
}

void FWindFieldProcess::WriteSynoptic(FMassifEnvironmentContext& Context, const FSynopticWind& Wind) const
{
	// Off is the synoptic wind over level ground everywhere: the channels still
	// exist, so nothing downstream has to ask whether the wind was solved. Level
	// ground has nothing above it to drain and nothing upwind to descend from, so
	// both terrain-driven channels read zero.
	const TUniquePtr<FMassifChannelWriteScope> Exposure =
		Context.OpenOutput(MassifEnvironmentChannels::WindExposure());
	const TUniquePtr<FMassifChannelWriteScope> Field = Context.OpenOutput(MassifEnvironmentChannels::WindField());
	const TUniquePtr<FMassifChannelWriteScope> Katabatic =
		Context.OpenOutput(MassifEnvironmentChannels::KatabaticStrength());
	const TUniquePtr<FMassifChannelWriteScope> Foehn = Context.OpenOutput(MassifEnvironmentChannels::FoehnWarming());

	const FVector2d Synoptic = Wind.FlowDirection * Wind.SpeedMetersPerSecond;
	TArrayView<double> ExposureValues = Exposure->GetComponent(0);
	TArrayView<double> EastwardValues = Field->GetComponent(0);
	TArrayView<double> NorthwardValues = Field->GetComponent(1);
	TArrayView<double> KatabaticValues = Katabatic->GetComponent(0);
	TArrayView<double> FoehnValues = Foehn->GetComponent(0);
	for (int32 CellIndex = 0; CellIndex < ExposureValues.Num(); ++CellIndex)
	{
		ExposureValues[CellIndex] = 1.0;
		EastwardValues[CellIndex] = Synoptic.X;
		NorthwardValues[CellIndex] = Synoptic.Y;
		KatabaticValues[CellIndex] = 0.0;
		FoehnValues[CellIndex] = 0.0;
	}

	Exposure->Commit();
	Field->Commit();
	Katabatic->Commit();
	Foehn->Commit();
}

void FWindFieldProcess::SolveRow(
	const FWindTerrain& Terrain, const FSynopticWind& Wind, const FMassifWindParams& RunParams,
	const FMassifChannelGrid& Grid, int32 Row, EMassifFidelity Fidelity, const FWindInputs& Inputs,
	FWindOutputs& Outputs) const
{
	const bool bTerrainSteers = Fidelity != EMassifFidelity::Cheap;

	for (int32 Column = 0; Column < Grid.Width; ++Column)
	{
		const FVector2d Position = Grid.CellCentreMeters(Column, Row);
		const FWindSample Sample = SolveWindAt(
			Terrain, Position, Wind, Grid.CellSizeMeters, bTerrainSteers,
			Inputs.AccumulationSquareMetersAt(Position), Inputs.BasinDepthMetersAt(Position), RunParams);

		const double Drag = 1.0 - RunParams.CanopyDragFraction * Inputs.CanopyAt(Position);

		const int32 CellIndex = Row * Grid.Width + Column;
		Outputs.Exposure[CellIndex] = Sample.Exposure;
		Outputs.Eastward[CellIndex] = Sample.FieldMetersPerSecond.X * Drag;
		Outputs.Northward[CellIndex] = Sample.FieldMetersPerSecond.Y * Drag;
		Outputs.KatabaticStrength[CellIndex] = Sample.KatabaticStrength;
		Outputs.FoehnWarmingCelsius[CellIndex] = Sample.FoehnWarmingCelsius;
	}
}

void FWindFieldProcess::Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const
{
	const FSynopticWind Wind = MakeSynopticWind(Context, Params);
	if (Fidelity == EMassifFidelity::Off)
	{
		WriteSynoptic(Context, Wind);
		return;
	}

	const FMassifWindParams RunParams = RefineForFidelity(Params, Fidelity);
	const FWindTerrain Terrain(Context.GetState(), Context.GetInput(MassifEnvironmentChannels::Altitude()));

	const TUniquePtr<FMassifChannelWriteScope> Exposure =
		Context.OpenOutput(MassifEnvironmentChannels::WindExposure());
	const TUniquePtr<FMassifChannelWriteScope> Field = Context.OpenOutput(MassifEnvironmentChannels::WindField());
	const TUniquePtr<FMassifChannelWriteScope> Katabatic =
		Context.OpenOutput(MassifEnvironmentChannels::KatabaticStrength());
	const TUniquePtr<FMassifChannelWriteScope> Foehn = Context.OpenOutput(MassifEnvironmentChannels::FoehnWarming());

	const FWindInputs Inputs{
		Context.GetState(), Context.FindOptionalInput(MassifEnvironmentChannels::VegetationCanopy()),
		Context.FindOptionalInput(MassifEnvironmentChannels::FlowAccumulation()),
		Context.FindOptionalInput(MassifEnvironmentChannels::StandingWaterDepth())};
	FWindOutputs Outputs{
		Exposure->GetComponent(0), Field->GetComponent(0), Field->GetComponent(1), Katabatic->GetComponent(0),
		Foehn->GetComponent(0)};

	const FMassifChannelGrid& Grid = Exposure->GetGrid();
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		SolveRow(Terrain, Wind, RunParams, Grid, Row, Fidelity, Inputs, Outputs);
	}

	Exposure->Commit();
	Field->Commit();
	Katabatic->Commit();
	Foehn->Commit();
}

void MassifRegisterWindProcesses(FMassifEnvironmentScheduler& Scheduler, const FMassifWindParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FWindFieldProcess>(Params));
}
