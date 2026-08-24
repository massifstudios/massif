#include "MassifRockfallProcesses.h"

#include "Async/ParallelFor.h"
#include "MassifEnvironmentGraph.h"
#include "MassifLithologyProcesses.h"
#include "MassifVegetationProcesses.h"

namespace MassifEnvironmentChannels
{
	const FName& RockfallDeposit()
	{
		static const FName Name(TEXT("Geology.RockfallDeposit"));
		return Name;
	}

	const FName& RockfallBlockRadius()
	{
		static const FName Name(TEXT("Geology.RockfallBlockRadius"));
		return Name;
	}
}

namespace MassifEnvironmentPointSets
{
	const FName& RockPlacements()
	{
		static const FName Name(TEXT("Geology.RockPlacements"));
		return Name;
	}
}

namespace MassifEnvironmentProcesses
{
	const FName& Rockfall()
	{
		static const FName Name(TEXT("Geology.Rockfall"));
		return Name;
	}
}

namespace
{
	constexpr double SquareMetresPerSquareKilometre = 1000000.0;

	/** How a fidelity level resolves one fall. */
	struct FFallSettings
	{
		double StepLengthMeters = 4.0;
		int32 MaxSteps = 400;
		bool bDampedByCanopy = true;
	};

	FFallSettings MakeFallSettings(const FMassifRockfallParams& Params, EMassifFidelity Fidelity)
	{
		FFallSettings Settings;
		Settings.StepLengthMeters = Params.StepLengthMeters;
		Settings.MaxSteps = Params.MaxFallSteps;

		if (Fidelity == EMassifFidelity::Cheap)
		{
			// Same model walked four times as coarsely: the cone lands in the right
			// place and is drawn with a quarter of the steps.
			Settings.StepLengthMeters = 4.0 * Params.StepLengthMeters;
			Settings.MaxSteps = FMath::Max(Params.MaxFallSteps / 4, 1);
			Settings.bDampedByCanopy = false;
			return Settings;
		}
		if (Fidelity == EMassifFidelity::Reference)
		{
			Settings.StepLengthMeters = 0.5 * Params.StepLengthMeters;
			Settings.MaxSteps = 2 * Params.MaxFallSteps;
		}
		return Settings;
	}

	/** One block where it stopped. Zero radius is a candidate that never released. */
	struct FRestingBlock
	{
		FVector3d PositionMeters = FVector3d::ZeroVector;
		double RadiusMeters = 0.0;
		FVector3d RotationDegrees = FVector3d::ZeroVector;
	};

	/**
	 * A candidate's own random sequence, drawn from its index alone.
	 *
	 * Not a stream carried along the fall: the candidate has to draw the same
	 * numbers whichever worker picks it up and whatever order the workers run in,
	 * and an index-seeded sequence is the only arrangement that holds under both.
	 */
	class FCandidateRandom
	{
	public:
		FCandidateRandom(uint32 InSeed, int64 CandidateIndex)
			: Seed(HashCombine(InSeed, GetTypeHash(CandidateIndex)))
		{
		}

		/** Uniform in [0,1). Advances the sequence. */
		double Next()
		{
			Seed = HashCombine(Seed, ++DrawIndex);
			return Seed / (static_cast<double>(TNumericLimits<uint32>::Max()) + 1.0);
		}

		double NextSigned()
		{
			return 2.0 * Next() - 1.0;
		}

	private:
		uint32 Seed = 0;
		uint32 DrawIndex = 0;
	};

	/**
	 * Bounded power law over the block-size band: many small blocks, few large.
	 *
	 * Inverse of the Pareto cumulative, cut off at both ends, so the largest block
	 * a face can shed is a property of the band rather than of how many were drawn.
	 */
	double DrawBlockRadius(FCandidateRandom& Random, const FMassifRockfallParams& Params)
	{
		const double Exponent = FMath::Max(Params.BlockSizeExponent, 0.1);
		const double MinTerm = FMath::Pow(Params.MinBlockRadiusMeters, -Exponent);
		const double MaxTerm = FMath::Pow(Params.MaxBlockRadiusMeters, -Exponent);
		return FMath::Pow(MinTerm - Random.Next() * (MinTerm - MaxTerm), -1.0 / Exponent);
	}

	/** Everything one run reads, resolved once instead of per candidate. */
	class FRockfallInputs
	{
	public:
		FRockfallInputs(const FMassifEnvironmentContext& Context, const FMassifRockfallParams& InParams);

		double AltitudeAt(FVector2d PositionMeters) const;
		FVector2d SurfaceGradientAt(FVector2d PositionMeters) const;
		double SlopeDegreesAt(FVector2d PositionMeters) const;

		/** Share of the full release rate this ground sheds, 0 where it sheds nothing. */
		double ReleaseFractionAt(FVector2d PositionMeters, double SlopeDegrees) const;

		double CanopyAt(FVector2d PositionMeters) const;

		FVector2d GetWorldSizeMeters() const
		{
			return WorldSizeMeters;
		}

		bool IsInsideWorld(FVector2d PositionMeters) const;

	private:
		const FMassifEnvironmentState& State;
		const IMassifElevationSource& Elevation;
		FMassifRockfallParams Params;
		FVector2d WorldSizeMeters = FVector2d::ZeroVector;

		FMassifChannelHandle SoilDepth;
		FMassifChannelHandle RockHardness;
		FMassifChannelHandle ScreeProduction;
		FMassifChannelHandle Canopy;

		double MaxScreeMetresPerMillennium = 1.0;
	};

	FRockfallInputs::FRockfallInputs(
		const FMassifEnvironmentContext& Context, const FMassifRockfallParams& InParams)
		: State(Context.GetState())
		, Elevation(Context.GetElevation())
		, Params(InParams)
		, WorldSizeMeters(Context.GetElevation().GetWorldSizeMeters())
		, SoilDepth(Context.FindOptionalInput(MassifEnvironmentChannels::SoilDepth()))
		, RockHardness(Context.FindOptionalInput(MassifEnvironmentChannels::RockHardness()))
		, ScreeProduction(Context.FindOptionalInput(MassifEnvironmentChannels::ScreeProduction()))
		, Canopy(Context.FindOptionalInput(MassifEnvironmentChannels::VegetationCanopy()))
	{
		if (ScreeProduction.IsValid() && State.IsChannelWritten(ScreeProduction))
		{
			MaxScreeMetresPerMillennium = FMath::Max(State.GetChannelMaximum(ScreeProduction), UE_DOUBLE_SMALL_NUMBER);
		}
	}

	double FRockfallInputs::AltitudeAt(FVector2d PositionMeters) const
	{
		return Elevation.SampleAltitudeMeters(PositionMeters, Params.StepLengthMeters);
	}

	FVector2d FRockfallInputs::SurfaceGradientAt(FVector2d PositionMeters) const
	{
		const double Spacing = Params.StepLengthMeters;
		const double East = AltitudeAt(PositionMeters + FVector2d(Spacing, 0.0));
		const double West = AltitudeAt(PositionMeters - FVector2d(Spacing, 0.0));
		const double North = AltitudeAt(PositionMeters + FVector2d(0.0, Spacing));
		const double South = AltitudeAt(PositionMeters - FVector2d(0.0, Spacing));
		return FVector2d((East - West) / (2.0 * Spacing), (North - South) / (2.0 * Spacing));
	}

	double FRockfallInputs::SlopeDegreesAt(FVector2d PositionMeters) const
	{
		return FMath::RadiansToDegrees(FMath::Atan(SurfaceGradientAt(PositionMeters).Length()));
	}

	bool FRockfallInputs::IsInsideWorld(FVector2d PositionMeters) const
	{
		return FMath::Abs(PositionMeters.X) < 0.5 * WorldSizeMeters.X
			&& FMath::Abs(PositionMeters.Y) < 0.5 * WorldSizeMeters.Y;
	}

	double FRockfallInputs::CanopyAt(FVector2d PositionMeters) const
	{
		return FMath::Clamp(MassifSampleOptional(State, Canopy, PositionMeters), 0.0, 1.0);
	}

	double FRockfallInputs::ReleaseFractionAt(FVector2d PositionMeters, double SlopeDegrees) const
	{
		const double SlopeShare = FMath::Clamp(
			(SlopeDegrees - Params.MinReleaseSlopeDegrees) / FMath::Max(Params.ReleaseSlopeSpanDegrees, 1.0),
			0.0, 1.0);
		if (SlopeShare <= 0.0)
		{
			return 0.0;
		}

		// Soil over a face means it is not a face. Hard rock stands in cliffs and
		// sheds blocks; soft rock crumbles to gravel, which is not an object.
		const double BareShare = SoilDepth.IsValid()
			? FMath::Clamp(1.0 - State.SampleScalar(SoilDepth, PositionMeters) / Params.ReleaseSoilDepthMeters, 0.0, 1.0)
			: 1.0;
		const double Hardness = RockHardness.IsValid() ? State.SampleScalar(RockHardness, PositionMeters) : 0.5;
		const double HardnessShare = FMath::Lerp(Params.SoftRockReleaseFraction, 1.0, Hardness);
		const double ScreeShare = ScreeProduction.IsValid()
			? State.SampleScalar(ScreeProduction, PositionMeters) / MaxScreeMetresPerMillennium
			: 1.0;

		return SlopeShare * BareShare * HardnessShare * FMath::Clamp(ScreeShare, 0.0, 1.0);
	}

	/** One block falling: where it is, how fast, and which way it was going. */
	struct FFallingBlock
	{
		FVector2d PositionMeters = FVector2d::ZeroVector;
		FVector2d Direction = FVector2d::ZeroVector;
		double SpeedSquared = 0.0;
	};

	/**
	 * Runs one candidate from release to rest.
	 *
	 * The block is carried by its own momentum rather than by the gradient under
	 * it, which is what separates a rockfall from a water droplet: a block that
	 * followed the steepest descent exactly would end up in the stream bed, and
	 * boulders are not found in stream beds. Inertia is what carries one across a
	 * bench and out onto the cone below.
	 */
	class FRockfallRunner
	{
	public:
		FRockfallRunner(
			const FRockfallInputs& InInputs, const FMassifRockfallParams& InParams, const FFallSettings& InSettings,
			uint32 InCandidateSeed, const FMassifChannelGrid& InCandidateGrid)
			: Inputs(InInputs)
			, Params(InParams)
			, Settings(InSettings)
			, CandidateSeed(InCandidateSeed)
			, CandidateGrid(InCandidateGrid)
			, ReposeFriction(FMath::Tan(FMath::DegreesToRadians(InParams.ReposeSlopeDegrees)))
			, RestSpeedSquared(InParams.RestSpeedMetersPerSecond * InParams.RestSpeedMetersPerSecond)
			, MaxSpeedSquared(InParams.MaxSpeedMetersPerSecond * InParams.MaxSpeedMetersPerSecond)
		{
		}

		FRestingBlock Run(int64 CandidateIndex) const;

	private:
		/** Candidates sit on a lattice with a seeded offset, so they cover the world without lining up. */
		FVector2d MakeReleasePosition(FCandidateRandom& Random, int64 CandidateIndex) const;

		/** How much less friction a large block feels, as a share of the small-block figure. */
		double RunoutFactorFor(double RadiusMeters) const;

		void Advance(FFallingBlock& Block, FCandidateRandom& Random, double RunoutFactor) const;
		FVector2d SteerFrom(const FFallingBlock& Block, FCandidateRandom& Random) const;

		FRestingBlock MakeResult(const FFallingBlock& Block, FCandidateRandom& Random, double RadiusMeters) const;

		const FRockfallInputs& Inputs;
		FMassifRockfallParams Params;
		FFallSettings Settings;
		uint32 CandidateSeed = 0;
		FMassifChannelGrid CandidateGrid;
		double ReposeFriction = 0.65;
		double RestSpeedSquared = 0.16;
		double MaxSpeedSquared = 2025.0;
	};

	FVector2d FRockfallRunner::MakeReleasePosition(FCandidateRandom& Random, int64 CandidateIndex) const
	{
		const int32 LatticeCell = static_cast<int32>(CandidateIndex % CandidateGrid.CellCount());
		const FVector2d Centre =
			CandidateGrid.CellCentreMeters(LatticeCell % CandidateGrid.Width, LatticeCell / CandidateGrid.Width);
		return Centre + 0.5 * CandidateGrid.CellSizeMeters * FVector2d(Random.NextSigned(), Random.NextSigned());
	}

	double FRockfallRunner::RunoutFactorFor(double RadiusMeters) const
	{
		const double Span = FMath::Max(Params.MaxBlockRadiusMeters - Params.MinBlockRadiusMeters, UE_DOUBLE_SMALL_NUMBER);
		const double SizeRank = FMath::Clamp((RadiusMeters - Params.MinBlockRadiusMeters) / Span, 0.0, 1.0);
		return 1.0 - Params.SizeRunoutBias * SizeRank;
	}

	FVector2d FRockfallRunner::SteerFrom(const FFallingBlock& Block, FCandidateRandom& Random) const
	{
		const FVector2d Gradient = Inputs.SurfaceGradientAt(Block.PositionMeters);
		const FVector2d Downhill = -Gradient;
		const FVector2d Steered = Block.Direction * Params.Inertia + Downhill * (1.0 - Params.Inertia);
		if (Steered.IsNearlyZero())
		{
			return FVector2d::ZeroVector;
		}

		// Scatter grows with speed: a block barely moving does not bound sideways,
		// and a fast one is what spreads a fall line into a cone.
		const FVector2d Heading = Steered.GetSafeNormal();
		const double SpeedShare = FMath::Sqrt(FMath::Clamp(Block.SpeedSquared / MaxSpeedSquared, 0.0, 1.0));
		const FVector2d Sideways(-Heading.Y, Heading.X);
		return (Heading + Sideways * (Params.LateralScatter * SpeedShare * Random.NextSigned())).GetSafeNormal();
	}

	void FRockfallRunner::Advance(FFallingBlock& Block, FCandidateRandom& Random, double RunoutFactor) const
	{
		Block.Direction = SteerFrom(Block, Random);
		if (Block.Direction.IsNearlyZero())
		{
			Block.SpeedSquared = 0.0;
			return;
		}

		const FVector2d NextPosition = Block.PositionMeters + Block.Direction * Settings.StepLengthMeters;
		const double DropMeters = Inputs.AltitudeAt(Block.PositionMeters) - Inputs.AltitudeAt(NextPosition);
		const double FrictionMeters = ReposeFriction * Settings.StepLengthMeters * RunoutFactor;

		// Energy in metres of head: the block gains what it descends and loses what
		// the repose angle would have cost it over the same ground.
		Block.SpeedSquared = FMath::Min(
			FMath::Max(Block.SpeedSquared + 2.0 * Params.Gravity * (DropMeters - FrictionMeters), 0.0),
			MaxSpeedSquared);

		if (Settings.bDampedByCanopy)
		{
			Block.SpeedSquared *= 1.0 - Params.CanopyDamping * Inputs.CanopyAt(NextPosition);
		}
		Block.PositionMeters = NextPosition;
	}

	FRestingBlock FRockfallRunner::MakeResult(
		const FFallingBlock& Block, FCandidateRandom& Random, double RadiusMeters) const
	{
		const double YawDegrees = FMath::RadiansToDegrees(FMath::Atan2(Block.Direction.X, Block.Direction.Y));

		FRestingBlock Result;
		Result.PositionMeters =
			FVector3d(Block.PositionMeters.X, Block.PositionMeters.Y, Inputs.AltitudeAt(Block.PositionMeters));
		Result.RadiusMeters = RadiusMeters;
		Result.RotationDegrees = FVector3d(YawDegrees, 360.0 * Random.Next(), 360.0 * Random.Next());
		return Result;
	}

	FRestingBlock FRockfallRunner::Run(int64 CandidateIndex) const
	{
		FCandidateRandom Random(CandidateSeed, CandidateIndex);
		const FVector2d ReleasePosition = MakeReleasePosition(Random, CandidateIndex);
		const double SlopeDegrees = Inputs.SlopeDegreesAt(ReleasePosition);
		if (Random.Next() >= Inputs.ReleaseFractionAt(ReleasePosition, SlopeDegrees))
		{
			return FRestingBlock();
		}

		const double RadiusMeters = DrawBlockRadius(Random, Params);
		const double RunoutFactor = RunoutFactorFor(RadiusMeters);

		FFallingBlock Block;
		Block.PositionMeters = ReleasePosition;
		Block.Direction = (-Inputs.SurfaceGradientAt(ReleasePosition)).GetSafeNormal();

		for (int32 Step = 0; Step < Settings.MaxSteps; ++Step)
		{
			Advance(Block, Random, RunoutFactor);

			// Leaving the world is tested before coming to rest: a block that does
			// both in one step has left, and must not be placed on the border it
			// stopped against.
			if (!Inputs.IsInsideWorld(Block.PositionMeters))
			{
				return FRestingBlock();
			}
			if (Block.SpeedSquared <= RestSpeedSquared)
			{
				break;
			}
		}
		return MakeResult(Block, Random, RadiusMeters);
	}

	/** Cell a resting block falls in, or INDEX_NONE for a candidate that never released. */
	int32 RestingCellIndex(const FMassifChannelGrid& Grid, const FRestingBlock& Block)
	{
		if (Block.RadiusMeters <= 0.0)
		{
			return INDEX_NONE;
		}

		const int32 Column = FMath::FloorToInt32(
			(Block.PositionMeters.X + 0.5 * Grid.WorldSizeXMeters) / Grid.CellSizeMeters);
		const int32 Row = FMath::FloorToInt32(
			(0.5 * Grid.WorldSizeYMeters - Block.PositionMeters.Y) / Grid.CellSizeMeters);
		return Grid.CellIndex(Column, Row);
	}

	/**
	 * Adds one block's own volume to the cell it stopped in, as a depth.
	 *
	 * Volume over cell area rather than a count times a constant, so the deposit
	 * means metres of debris whatever the block-size band or the cell size is set
	 * to, and a cone of large blocks reads deeper than a cone of small ones.
	 */
	void AccumulateBlock(
		const FMassifChannelGrid& Grid, const FRestingBlock& Block, double CellArea,
		TArrayView<double> DepositMeters, TArrayView<double> RadiusSum, TArrayView<int32> BlockCounts)
	{
		const int32 CellIndex = RestingCellIndex(Grid, Block);
		if (CellIndex == INDEX_NONE)
		{
			return;
		}

		const double Volume = 4.0 / 3.0 * UE_DOUBLE_PI * FMath::Pow(Block.RadiusMeters, 3.0);
		DepositMeters[CellIndex] += Volume / CellArea;
		RadiusSum[CellIndex] += Block.RadiusMeters;
		++BlockCounts[CellIndex];
	}
}

/**
 * What the cliffs shed, run down the mountain and left where it stopped.
 *
 * The pass produces two things out of one set of falling blocks: the blocks
 * themselves as placed objects, and the deposit they left as a field. Both are
 * the same particles, so the scree cone the player walks on cannot disagree
 * with the one the rest of the graph reads.
 *
 * Every input is optional. A graph with no rock, no soil and no forest in it
 * still sheds blocks off its steep ground and still piles them at the foot of
 * it — which is the thing a slope threshold could never say, because a slope
 * threshold puts the debris on the cliff rather than under it.
 */
class FRockfallProcess final : public IMassifEnvironmentProcess
{
public:
	explicit FRockfallProcess(const FMassifRockfallParams& InParams)
		: Params(InParams)
	{
	}

	FMassifProcessDeclaration Declare() const override;
	void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override;

private:
	FMassifChannelDescriptor MakeDepositChannel() const;
	FMassifChannelDescriptor MakeBlockRadiusChannel() const;
	FMassifPointSetDescriptor MakePlacementsDescriptor() const;

	void WriteBareWorld(FMassifEnvironmentContext& Context) const;

	int64 CandidateCountFor(FVector2d WorldSizeMeters) const;

	/** Runs every candidate, wide over the candidate index. */
	void SolveBlocks(
		const FRockfallInputs& Inputs, const FMassifEnvironmentContext& Context, EMassifFidelity Fidelity,
		TArray<FRestingBlock>& OutBlocks) const;

	void WritePlacements(
		FMassifEnvironmentContext& Context, TConstArrayView<FRestingBlock> Blocks) const;
	void WriteDeposit(FMassifEnvironmentContext& Context, TConstArrayView<FRestingBlock> Blocks) const;

	FMassifRockfallParams Params;
};

FMassifChannelDescriptor FRockfallProcess::MakeDepositChannel() const
{
	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = MassifEnvironmentChannels::RockfallDeposit();
	Descriptor.Type = EMassifChannelType::Scalar;
	Descriptor.Encoding = EMassifChannelEncoding::Logarithmic16;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.Unit = TEXT("m");
	Descriptor.Description = TEXT("Blocks lying where the falling stopped");
	Descriptor.ColorMap = EMassifChannelColorMap::Magma;
	return Descriptor;
}

FMassifChannelDescriptor FRockfallProcess::MakeBlockRadiusChannel() const
{
	FMassifChannelDescriptor Descriptor;
	Descriptor.Name = MassifEnvironmentChannels::RockfallBlockRadius();
	Descriptor.Type = EMassifChannelType::Scalar;
	Descriptor.Encoding = EMassifChannelEncoding::Uint16;
	Descriptor.CellSizeMeters = Params.CellSizeMeters;
	Descriptor.Unit = TEXT("m");
	Descriptor.Description = TEXT("Mean radius of the blocks that stopped here");
	Descriptor.ColorMap = EMassifChannelColorMap::Viridis;
	Descriptor.DisplayRange = FVector2d(0.0, Params.MaxBlockRadiusMeters);
	return Descriptor;
}

FMassifPointSetDescriptor FRockfallProcess::MakePlacementsDescriptor() const
{
	FMassifPointSetDescriptor Descriptor;
	Descriptor.Name = MassifEnvironmentPointSets::RockPlacements();
	Descriptor.TileSizeMeters = Params.TileSizeMeters;
	Descriptor.RadiusRangeMeters = FVector2d(Params.MinBlockRadiusMeters, Params.MaxBlockRadiusMeters);
	Descriptor.ClassNames = { TEXT("Block") };
	Descriptor.Description = TEXT("Boulders where the rockfall left them");
	return Descriptor;
}

FMassifProcessDeclaration FRockfallProcess::Declare() const
{
	FMassifProcessDeclaration Declaration;
	Declaration.Name = MassifEnvironmentProcesses::Rockfall();
	Declaration.DisplayName = TEXT("Rockfall");

	// All optional: steep ground sheds blocks whether or not the graph knows what
	// the rock is, what soil is over it, or what is growing below it.
	Declaration.OptionalInputChannels = {
		MassifEnvironmentChannels::SoilDepth(),
		MassifEnvironmentChannels::RockHardness(),
		MassifEnvironmentChannels::ScreeProduction(),
		MassifEnvironmentChannels::VegetationCanopy()};
	Declaration.SupportedFidelities = {
		EMassifFidelity::Cheap, EMassifFidelity::Standard, EMassifFidelity::Reference};

	Declaration.OutputChannels.Add(MakeDepositChannel());
	Declaration.OutputChannels.Add(MakeBlockRadiusChannel());
	Declaration.OutputPointSets.Add(MakePlacementsDescriptor());
	return Declaration;
}

void FRockfallProcess::WriteBareWorld(FMassifEnvironmentContext& Context) const
{
	// Off is a world whose cliffs never shed: no blocks, no debris. The channels
	// and the set still exist, so nothing downstream asks whether rock falls here.
	MassifFillChannelSlice(Context, MassifEnvironmentChannels::RockfallDeposit(), 0, 0.0);
	MassifFillChannelSlice(Context, MassifEnvironmentChannels::RockfallBlockRadius(), 0, 0.0);

	const TUniquePtr<FMassifPointSetWriteScope> Placements =
		Context.OpenPointSetOutput(MassifEnvironmentPointSets::RockPlacements());
	Placements->Reserve(0);
	Placements->Commit();
}

int64 FRockfallProcess::CandidateCountFor(FVector2d WorldSizeMeters) const
{
	const double AreaSquareKilometres =
		WorldSizeMeters.X * WorldSizeMeters.Y / SquareMetresPerSquareKilometre;
	return FMath::Max(FMath::RoundToInt64(Params.CandidatesPerSquareKilometre * AreaSquareKilometres), 1);
}

void FRockfallProcess::SolveBlocks(
	const FRockfallInputs& Inputs, const FMassifEnvironmentContext& Context, EMassifFidelity Fidelity,
	TArray<FRestingBlock>& OutBlocks) const
{
	const FVector2d WorldSizeMeters = Inputs.GetWorldSizeMeters();
	const int64 CandidateCount = CandidateCountFor(WorldSizeMeters);
	OutBlocks.SetNum(CandidateCount);

	// The lattice candidates are spread over. Its own grid rather than the deposit
	// grid, so the number of blocks does not move when the deposit is re-resolved.
	const double LatticeCellSizeMeters =
		FMath::Sqrt(WorldSizeMeters.X * WorldSizeMeters.Y / static_cast<double>(CandidateCount));
	const FRockfallRunner Runner(
		Inputs, Params, MakeFallSettings(Params, Fidelity), Context.MakeSeed(TEXT("Candidates")),
		FMassifChannelGrid::FromWorldSize(WorldSizeMeters, LatticeCellSizeMeters));

	const int32 BatchSize = FMath::Max(Params.CandidatesPerBatch, 1);
	const int64 BatchCount = FMath::DivideAndRoundUp(CandidateCount, static_cast<int64>(BatchSize));
	ParallelFor(static_cast<int32>(BatchCount), [&](int32 BatchIndex)
	{
		const int64 FirstCandidate = static_cast<int64>(BatchIndex) * BatchSize;
		const int64 LastCandidate = FMath::Min(FirstCandidate + BatchSize, CandidateCount);
		for (int64 Candidate = FirstCandidate; Candidate < LastCandidate; ++Candidate)
		{
			OutBlocks[Candidate] = Runner.Run(Candidate);
		}
	});
}

void FRockfallProcess::WritePlacements(
	FMassifEnvironmentContext& Context, TConstArrayView<FRestingBlock> Blocks) const
{
	const TUniquePtr<FMassifPointSetWriteScope> Placements =
		Context.OpenPointSetOutput(MassifEnvironmentPointSets::RockPlacements());
	TArrayView<FMassifEnvironmentPoint> Slots = Placements->Reserve(Blocks.Num());

	for (int32 BlockIndex = 0; BlockIndex < Blocks.Num(); ++BlockIndex)
	{
		Slots[BlockIndex].PositionMeters = Blocks[BlockIndex].PositionMeters;
		Slots[BlockIndex].RadiusMeters = Blocks[BlockIndex].RadiusMeters;
		Slots[BlockIndex].RotationDegrees = Blocks[BlockIndex].RotationDegrees;
		Slots[BlockIndex].BuriedFraction = Params.BuriedFraction;
	}
	Placements->Commit();
}

void FRockfallProcess::WriteDeposit(
	FMassifEnvironmentContext& Context, TConstArrayView<FRestingBlock> Blocks) const
{
	const TUniquePtr<FMassifChannelWriteScope> Deposit =
		Context.OpenOutput(MassifEnvironmentChannels::RockfallDeposit());
	const TUniquePtr<FMassifChannelWriteScope> BlockRadius =
		Context.OpenOutput(MassifEnvironmentChannels::RockfallBlockRadius());

	const FMassifChannelGrid& Grid = Deposit->GetGrid();
	TArrayView<double> DepositMeters = Deposit->GetComponent(0);
	TArrayView<double> MeanRadius = BlockRadius->GetComponent(0);
	TArray<int32> BlockCounts;
	BlockCounts.SetNumZeroed(Grid.CellCount());

	// Serial, and cheap against the fall itself: the resting positions are already
	// fixed, so the reduction needs no atomics to come out the same every run.
	const double CellArea = Grid.CellSizeMeters * Grid.CellSizeMeters;
	for (const FRestingBlock& Block : Blocks)
	{
		AccumulateBlock(Grid, Block, CellArea, DepositMeters, MeanRadius, BlockCounts);
	}

	for (int32 CellIndex = 0; CellIndex < Grid.CellCount(); ++CellIndex)
	{
		MeanRadius[CellIndex] /= FMath::Max(BlockCounts[CellIndex], 1);
	}

	Deposit->Commit();
	BlockRadius->Commit();
}

void FRockfallProcess::Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const
{
	if (Fidelity == EMassifFidelity::Off)
	{
		WriteBareWorld(Context);
		return;
	}

	const FRockfallInputs Inputs(Context, Params);
	TArray<FRestingBlock> Blocks;
	SolveBlocks(Inputs, Context, Fidelity, Blocks);

	WritePlacements(Context, Blocks);
	WriteDeposit(Context, Blocks);
}

void MassifRegisterRockfallProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifRockfallParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FRockfallProcess>(Params));
}
