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

	/**
	 * Sx, the topographic exposure index: the steepest angle anything within the
	 * upwind fetch subtends at the point.
	 *
	 * Positive means ground upwind stands above the point and takes the flow off
	 * it; negative means the point stands above everything it faces into, and the
	 * flow is compressed over it. This is the whole difference between a crest and
	 * the pocket behind it, and no local measure of slope can tell them apart.
	 */
	double UpwindExposureAngle(
		const FWindTerrain& Terrain, FVector2d PositionMeters, const FSynopticWind& Wind,
		const FMassifWindParams& Params)
	{
		const double GroundAltitudeMeters = Terrain.AltitudeAt(PositionMeters);
		const int32 StepCount = FMath::Max(1, Params.ExposureStepCount);

		double HighestTangent = TNumericLimits<double>::Lowest();
		for (int32 StepIndex = 1; StepIndex <= StepCount; ++StepIndex)
		{
			const double DistanceMeters = Params.ExposureReachMeters * StepIndex / StepCount;
			const double RiseMeters =
				Terrain.AltitudeAt(PositionMeters + Wind.UpwindDirection * DistanceMeters) - GroundAltitudeMeters;
			HighestTangent = FMath::Max(HighestTangent, RiseMeters / DistanceMeters);
		}
		return FMath::Atan(HighestTangent);
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
		const double Shelter = FMath::Clamp(1.0 - Exposure, 0.0, 1.0);
		return Fall * Shelter;
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
		double SpacingMeters, const FMassifWindParams& Params)
	{
		const FVector2d Gradient = Terrain.GradientAt(PositionMeters, SpacingMeters);
		const FVector2d Channeled = BlendDirection(
			Sample.FieldMetersPerSecond.GetSafeNormal(), ValleyAxis(Gradient, Wind.FlowDirection),
			ChannelingWeight(Gradient, Params));

		const double Separation =
			LeeSeparationStrength(Terrain, PositionMeters, Wind, Sample.Exposure, SpacingMeters, Params);
		return SeparateInLee(Channeled * Sample.FieldMetersPerSecond.Length(), Wind, Separation, Params);
	}

	FWindSample SolveWindAt(
		const FWindTerrain& Terrain, FVector2d PositionMeters, const FSynopticWind& Wind, double SpacingMeters,
		bool bTerrainSteers, const FMassifWindParams& Params)
	{
		FWindSample Sample;
		Sample.Exposure = ExposureFromAngle(UpwindExposureAngle(Terrain, PositionMeters, Wind, Params), Params);
		Sample.FieldMetersPerSecond = Wind.FlowDirection * (Wind.SpeedMetersPerSecond * Sample.Exposure);
		if (!bTerrainSteers)
		{
			return Sample;
		}

		Sample.FieldMetersPerSecond = SteerByTerrain(Terrain, PositionMeters, Wind, Sample, SpacingMeters, Params);
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

	void WriteSynoptic(FMassifEnvironmentContext& Context, const FSynopticWind& Wind) const;

	void SolveRow(
		const FWindTerrain& Terrain, const FSynopticWind& Wind, const FMassifWindParams& RunParams,
		const FMassifChannelGrid& Grid, int32 Row, EMassifFidelity Fidelity, TArrayView<double> OutExposure,
		TArrayView<double> OutEastward, TArrayView<double> OutNorthward,
		TFunctionRef<double(FVector2d)> CanopyAt) const;

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

FMassifProcessDeclaration FWindFieldProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::Wind();
	Declaration.InputChannels = {MassifEnvironmentChannels::Altitude()};
	Declaration.OptionalInputChannels = {MassifEnvironmentChannels::VegetationCanopy()};

	// Vegetation reads the snow this wind moves, so the scheduler finds a cycle
	// through it and has to be told it is meant.
	Declaration.bParticipatesInFeedback = true;
	Declaration.SupportedFidelities = {
		EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};
	Declaration.OutputChannels.Add(MakeExposureChannel());
	Declaration.OutputChannels.Add(MakeFieldChannel());
	return Declaration;
}

void FWindFieldProcess::WriteSynoptic(FMassifEnvironmentContext& Context, const FSynopticWind& Wind) const
{
	// Off is the synoptic wind over level ground everywhere: the channels still
	// exist, so nothing downstream has to ask whether the wind was solved.
	const TUniquePtr<FMassifChannelWriteScope> Exposure =
		Context.OpenOutput(MassifEnvironmentChannels::WindExposure());
	const TUniquePtr<FMassifChannelWriteScope> Field = Context.OpenOutput(MassifEnvironmentChannels::WindField());

	const FVector2d Synoptic = Wind.FlowDirection * Wind.SpeedMetersPerSecond;
	TArrayView<double> ExposureValues = Exposure->GetComponent(0);
	TArrayView<double> EastwardValues = Field->GetComponent(0);
	TArrayView<double> NorthwardValues = Field->GetComponent(1);
	for (int32 CellIndex = 0; CellIndex < ExposureValues.Num(); ++CellIndex)
	{
		ExposureValues[CellIndex] = 1.0;
		EastwardValues[CellIndex] = Synoptic.X;
		NorthwardValues[CellIndex] = Synoptic.Y;
	}

	Exposure->Commit();
	Field->Commit();
}

void FWindFieldProcess::SolveRow(
	const FWindTerrain& Terrain, const FSynopticWind& Wind, const FMassifWindParams& RunParams,
	const FMassifChannelGrid& Grid, int32 Row, EMassifFidelity Fidelity, TArrayView<double> OutExposure,
	TArrayView<double> OutEastward, TArrayView<double> OutNorthward,
	TFunctionRef<double(FVector2d)> CanopyAt) const
{
	const bool bTerrainSteers = Fidelity != EMassifFidelity::Cheap;

	for (int32 Column = 0; Column < Grid.Width; ++Column)
	{
		const FVector2d Position = Grid.CellCentreMeters(Column, Row);
		const FWindSample Sample =
			SolveWindAt(Terrain, Position, Wind, Grid.CellSizeMeters, bTerrainSteers, RunParams);

		const double Drag = 1.0 - RunParams.CanopyDragFraction * CanopyAt(Position);

		const int32 CellIndex = Row * Grid.Width + Column;
		OutExposure[CellIndex] = Sample.Exposure;
		OutEastward[CellIndex] = Sample.FieldMetersPerSecond.X * Drag;
		OutNorthward[CellIndex] = Sample.FieldMetersPerSecond.Y * Drag;
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

	const FMassifEnvironmentState& State = Context.GetState();
	const FMassifChannelHandle Canopy =
		Context.FindOptionalInput(MassifEnvironmentChannels::VegetationCanopy());
	const auto CanopyAt = [&State, Canopy](FVector2d Position)
	{
		if (!Canopy.IsValid())
		{
			return 0.0;
		}
		return State.SampleScalar(Canopy, Position);
	};

	const FMassifChannelGrid& Grid = Exposure->GetGrid();
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		SolveRow(
			Terrain, Wind, RunParams, Grid, Row, Fidelity, Exposure->GetComponent(0), Field->GetComponent(0),
			Field->GetComponent(1), CanopyAt);
	}

	Exposure->Commit();
	Field->Commit();
}

void MassifRegisterWindProcesses(FMassifEnvironmentScheduler& Scheduler, const FMassifWindParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FWindFieldProcess>(Params));
}
