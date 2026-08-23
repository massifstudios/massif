#include "MassifEnvironmentGraph.h"

#include "MassifEnvironmentBakeGrid.h"
#include "MassifClimateProcesses.h"
#include "MassifPrecipitationProcesses.h"
#include "MassifLithologyProcesses.h"
#include "MassifSnowProcesses.h"
#include "MassifSunProcesses.h"
#include "MassifVegetationProcesses.h"
#include "MassifWindProcesses.h"
#include "MassifRiverNetwork.h"

namespace MassifEnvironmentChannels
{
	const FName& Altitude()
	{
		static const FName Name(TEXT("Terrain.Altitude"));
		return Name;
	}

	const FName& FilledAltitude()
	{
		static const FName Name(TEXT("Hydrology.FilledAltitude"));
		return Name;
	}

	const FName& StandingWaterDepth()
	{
		static const FName Name(TEXT("Hydrology.StandingWaterDepth"));
		return Name;
	}

	const FName& FlowAccumulation()
	{
		static const FName Name(TEXT("Hydrology.FlowAccumulation"));
		return Name;
	}

	const FName& Wetness()
	{
		static const FName Name(TEXT("Hydrology.Wetness"));
		return Name;
	}

	const FName& SoilDepth()
	{
		static const FName Name(TEXT("Soil.Depth"));
		return Name;
	}

	const FName& SoilDeposition()
	{
		static const FName Name(TEXT("Soil.Deposition"));
		return Name;
	}

	const FName& Infiltration()
	{
		static const FName Name(TEXT("Hydrology.Infiltration"));
		return Name;
	}

	const FName& SpringDischarge()
	{
		static const FName Name(TEXT("Hydrology.SpringDischarge"));
		return Name;
	}

	const FName& SeasonalFlow()
	{
		static const FName Name(TEXT("Hydrology.SeasonalFlow"));
		return Name;
	}
}

namespace MassifEnvironmentProcesses
{
	const FName& Altitude()
	{
		static const FName Name(TEXT("Terrain.Altitude"));
		return Name;
	}

	const FName& DepressionFill()
	{
		static const FName Name(TEXT("Hydrology.DepressionFill"));
		return Name;
	}

	const FName& StandingWater()
	{
		static const FName Name(TEXT("Hydrology.StandingWater"));
		return Name;
	}

	const FName& FlowRouting()
	{
		static const FName Name(TEXT("Hydrology.FlowRouting"));
		return Name;
	}

	const FName& SoilDepth()
	{
		static const FName Name(TEXT("Soil.Depth"));
		return Name;
	}

	const FName& SoilDeposition()
	{
		static const FName Name(TEXT("Soil.Deposition"));
		return Name;
	}

	const FName& Wetness()
	{
		static const FName Name(TEXT("Hydrology.Wetness"));
		return Name;
	}

	const FName& Infiltration()
	{
		static const FName Name(TEXT("Hydrology.Infiltration"));
		return Name;
	}

	const FName& Springs()
	{
		static const FName Name(TEXT("Hydrology.Springs"));
		return Name;
	}

	const FName& SeasonalFlow()
	{
		static const FName Name(TEXT("Hydrology.SeasonalFlow"));
		return Name;
	}
}

namespace
{
	FMassifChannelDescriptor MakeChannel(
		const FName& Name, EMassifChannelEncoding Encoding, double CellSizeMeters, const TCHAR* Unit,
		EMassifChannelColorMap ColorMap)
	{
		FMassifChannelDescriptor Descriptor;
		Descriptor.Name = Name;
		Descriptor.Type = EMassifChannelType::Scalar;
		Descriptor.Encoding = Encoding;
		Descriptor.CellSizeMeters = CellSizeMeters;
		Descriptor.Unit = Unit;
		Descriptor.ColorMap = ColorMap;
		return Descriptor;
	}

	FMassifBakeGrid MakeBakeGrid(const FMassifChannelGrid& ChannelGrid)
	{
		FMassifBakeGrid Grid;
		Grid.Width = ChannelGrid.Width;
		Grid.Height = ChannelGrid.Height;
		Grid.CellSizeMeters = ChannelGrid.CellSizeMeters;
		Grid.WorldSizeXMeters = ChannelGrid.WorldSizeXMeters;
		Grid.WorldSizeYMeters = ChannelGrid.WorldSizeYMeters;
		return Grid;
	}

	void ReadCells(const FMassifEnvironmentState& State, FMassifChannelHandle Channel, TArray<double>& OutValues)
	{
		const FMassifChannelGrid& Grid = State.GetGrid(Channel);
		OutValues.SetNumUninitialized(Grid.CellCount());
		for (int32 Row = 0; Row < Grid.Height; ++Row)
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				OutValues[Row * Grid.Width + Column] = State.ReadCell(Channel, Column, Row);
			}
		}
	}

	void ReadCellsAsFloats(
		const FMassifEnvironmentState& State, FMassifChannelHandle Channel, TArray<float>& OutValues)
	{
		TArray<double> Values;
		ReadCells(State, Channel, Values);
		OutValues.SetNumUninitialized(Values.Num());
		for (int32 CellIndex = 0; CellIndex < Values.Num(); ++CellIndex)
		{
			OutValues[CellIndex] = static_cast<float>(Values[CellIndex]);
		}
	}

	void WriteCells(FMassifChannelWriteScope& Scope, TArrayView<const double> Values)
	{
		TArrayView<double> Scratch = Scope.GetComponent(0);
		for (int32 CellIndex = 0; CellIndex < Scratch.Num(); ++CellIndex)
		{
			Scratch[CellIndex] = Values[CellIndex];
		}
		Scope.Commit();
	}

	void WriteCells(FMassifChannelWriteScope& Scope, TArrayView<const float> Values)
	{
		TArrayView<double> Scratch = Scope.GetComponent(0);
		for (int32 CellIndex = 0; CellIndex < Scratch.Num(); ++CellIndex)
		{
			Scratch[CellIndex] = Values[CellIndex];
		}
		Scope.Commit();
	}

	/** The precipitation solve's half-monthly totals, read wherever the graph has them. */
	const FName& SeasonalPrecipitationChannel()
	{
		static const FName Name(TEXT("Precip.Seasonal"));
		return Name;
	}

	FVector2d CellCentre(const FMassifChannelGrid& Grid, int32 CellIndex)
	{
		return Grid.CellCentreMeters(CellIndex % Grid.Width, CellIndex / Grid.Width);
	}

	/** Cell of a grid a world position falls in, clamped to its edges. */
	int32 CellIndexAt(const FMassifChannelGrid& Grid, FVector2d PositionMeters)
	{
		const double FromWestMeters = PositionMeters.X + 0.5 * Grid.WorldSizeXMeters;
		const double FromNorthMeters = 0.5 * Grid.WorldSizeYMeters - PositionMeters.Y;
		const int32 Column =
			FMath::Clamp(FMath::FloorToInt32(FromWestMeters / Grid.CellSizeMeters), 0, Grid.Width - 1);
		const int32 Row =
			FMath::Clamp(FMath::FloorToInt32(FromNorthMeters / Grid.CellSizeMeters), 0, Grid.Height - 1);
		return Row * Grid.Width + Column;
	}

	/**
	 * A channel read onto a coarser grid by the largest value covering each cell.
	 *
	 * The reduction a river survives. Flow accumulation is one cell wide wherever it
	 * matters, so a mean would divide the stream away over the dry ground beside it
	 * and a point sample would miss it wherever the thread does not run through the
	 * coarse cell's centre.
	 */
	void ReadLargestPerCell(
		const FMassifEnvironmentState& State, FMassifChannelHandle Channel, const FMassifChannelGrid& OutGrid,
		TArray<double>& OutValues)
	{
		const FMassifChannelGrid& InGrid = State.GetGrid(Channel);
		OutValues.Init(0.0, OutGrid.CellCount());
		for (int32 CellIndex = 0; CellIndex < InGrid.CellCount(); ++CellIndex)
		{
			const int32 Column = CellIndex % InGrid.Width;
			const int32 Row = CellIndex / InGrid.Width;
			const int32 OutIndex = CellIndexAt(OutGrid, InGrid.CellCentreMeters(Column, Row));
			OutValues[OutIndex] = FMath::Max(OutValues[OutIndex], State.ReadCell(Channel, Column, Row));
		}
	}

	void FillWithConstant(FMassifChannelWriteScope& Scope, double Value)
	{
		TArrayView<double> Scratch = Scope.GetComponent(0);
		for (int32 CellIndex = 0; CellIndex < Scratch.Num(); ++CellIndex)
		{
			Scratch[CellIndex] = Value;
		}
		Scope.Commit();
	}

	/** Shared by every process here: they all carry the same parameter block for now. */
	class FBakeProcess : public IMassifEnvironmentProcess
	{
	public:
		explicit FBakeProcess(const FMassifEnvironmentBakeParams& InParams)
			: Params(InParams)
		{
		}

	protected:
		FMassifEnvironmentBakeParams Params;
	};

	/**
	 * The elevation source sampled onto the graph's grid.
	 *
	 * Has no `Off` behaviour of its own: the ground is not a system that can be
	 * switched off, it is the domain every other system runs over.
	 */
	class FAltitudeProcess final : public FBakeProcess
	{
	public:
		using FBakeProcess::FBakeProcess;

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = MassifEnvironmentProcesses::Altitude();
			Declaration.DisplayName = TEXT("Terrain Sampling");
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(MakeChannel(
				MassifEnvironmentChannels::Altitude(), EMassifChannelEncoding::Float64, Params.CellSizeMeters,
				TEXT("m"), EMassifChannelColorMap::Terrain));
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity) const override
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::Altitude());
			const FMassifChannelGrid& Grid = Scope->GetGrid();
			TArrayView<double> Altitudes = Scope->GetComponent(0);

			for (int32 Row = 0; Row < Grid.Height; ++Row)
			{
				SampleRow(Context, Grid, Row, Altitudes);
			}
			Scope->Commit();
		}

	private:
		static void SampleRow(
			const FMassifEnvironmentContext& Context, const FMassifChannelGrid& Grid, int32 Row,
			TArrayView<double> OutAltitudes)
		{
			for (int32 Column = 0; Column < Grid.Width; ++Column)
			{
				const FVector2d Position = Grid.CellCentreMeters(Column, Row);
				OutAltitudes[Row * Grid.Width + Column] =
					Context.GetElevation().SampleAltitudeMeters(Position, Grid.CellSizeMeters);
			}
		}
	};

	/** Raises every hollow to the level it overflows at. Off leaves the measured surface alone. */
	class FDepressionFillProcess final : public FBakeProcess
	{
	public:
		using FBakeProcess::FBakeProcess;

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = MassifEnvironmentProcesses::DepressionFill();
			Declaration.DisplayName = TEXT("Depression Filling");
			Declaration.InputChannels = {MassifEnvironmentChannels::Altitude()};
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(MakeChannel(
				MassifEnvironmentChannels::FilledAltitude(), EMassifChannelEncoding::Float64, Params.CellSizeMeters,
				TEXT("m"), EMassifChannelColorMap::Terrain));
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			TArray<double> Measured;
			ReadCells(Context.GetState(), Context.GetInput(MassifEnvironmentChannels::Altitude()), Measured);

			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::FilledAltitude());
			if (Fidelity == EMassifFidelity::Off)
			{
				WriteCells(*Scope, Measured);
				return;
			}

			TArray<double> Filled;
			MassifBakeCpu::FillDepressions(MakeBakeGrid(Scope->GetGrid()), Params, Measured, Filled);
			WriteCells(*Scope, Filled);
		}
	};

	/** The filled surface less the measured one: water a hollow cannot drain. */
	class FStandingWaterProcess final : public FBakeProcess
	{
	public:
		using FBakeProcess::FBakeProcess;

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = MassifEnvironmentProcesses::StandingWater();
			Declaration.DisplayName = TEXT("Standing Water");
			Declaration.InputChannels = {
				MassifEnvironmentChannels::Altitude(), MassifEnvironmentChannels::FilledAltitude()};
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(MakeChannel(
				MassifEnvironmentChannels::StandingWaterDepth(), EMassifChannelEncoding::Uint16,
				Params.CellSizeMeters, TEXT("m"), EMassifChannelColorMap::BlueWhite));
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::StandingWaterDepth());
			if (Fidelity == EMassifFidelity::Off)
			{
				FillWithConstant(*Scope, 0.0);
				return;
			}

			TArray<double> Measured;
			TArray<double> Filled;
			ReadCells(Context.GetState(), Context.GetInput(MassifEnvironmentChannels::Altitude()), Measured);
			ReadCells(Context.GetState(), Context.GetInput(MassifEnvironmentChannels::FilledAltitude()), Filled);

			TArray<float> DepthMeters;
			MassifBakeCpu::MeasureStandingWater(Measured, Filled, DepthMeters);
			WriteCells(*Scope, DepthMeters);
		}
	};

	/**
	 * Square metres draining through each cell.
	 *
	 * Kept exact rather than logarithmic: the river network is traced from this
	 * channel, and quantising it would move where a catchment crosses the
	 * threshold that makes it a river.
	 */
	class FFlowRoutingProcess final : public FBakeProcess
	{
	public:
		using FBakeProcess::FBakeProcess;

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = MassifEnvironmentProcesses::FlowRouting();
			Declaration.DisplayName = TEXT("Water Flow Routing");
			Declaration.InputChannels = {MassifEnvironmentChannels::FilledAltitude()};
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(MakeChannel(
				MassifEnvironmentChannels::FlowAccumulation(), EMassifChannelEncoding::Float32,
				Params.CellSizeMeters, TEXT("m2"), EMassifChannelColorMap::Viridis));
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::FlowAccumulation());
			const FMassifBakeGrid Grid = MakeBakeGrid(Scope->GetGrid());
			if (Fidelity == EMassifFidelity::Off)
			{
				// Every cell drains itself and nothing else: no catchment anywhere.
				FillWithConstant(*Scope, Grid.CellAreaSquareMeters());
				return;
			}

			TArray<double> Filled;
			ReadCells(Context.GetState(), Context.GetInput(MassifEnvironmentChannels::FilledAltitude()), Filled);

			TArray<float> AccumulationSquareMeters;
			MassifBakeCpu::RouteFlow(Grid, Params, Filled, AccumulationSquareMeters);
			WriteCells(*Scope, AccumulationSquareMeters);
		}
	};

	/**
	 * Where droplets running down the surface dropped what they were carrying.
	 *
	 * Deliberately separate from the soil depth that reads it. This is the
	 * expensive part of the whole bake and it depends only on the filled surface,
	 * which nothing in the vegetation loop touches - so keeping it out of that loop
	 * is the difference between running it once and running it once per relaxation
	 * round.
	 */
	class FSoilDepositionProcess final : public FBakeProcess
	{
	public:
		using FBakeProcess::FBakeProcess;

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = MassifEnvironmentProcesses::SoilDeposition();
			Declaration.DisplayName = TEXT("Soil Erosion");
			Declaration.InputChannels = {MassifEnvironmentChannels::FilledAltitude()};
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(MakeChannel(
				MassifEnvironmentChannels::SoilDeposition(), EMassifChannelEncoding::Uint16,
				Params.CellSizeMeters, TEXT("m"), EMassifChannelColorMap::Magma));
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::SoilDeposition());
			if (Fidelity == EMassifFidelity::Off)
			{
				// Regolith on ground nothing has moved, everywhere.
				FillWithConstant(*Scope, Params.BaseSoilDepthMeters);
				return;
			}

			TArray<double> Filled;
			ReadCells(Context.GetState(), Context.GetInput(MassifEnvironmentChannels::FilledAltitude()), Filled);

			TArray<float> SoilDepthMeters;
			MassifBakeCpu::RunDroplets(
				MakeBakeGrid(Scope->GetGrid()), Params, Context.MakeSeed(TEXT("Droplets")), Filled,
				SoilDepthMeters);
			WriteCells(*Scope, SoilDepthMeters);
		}
	};

	/** What the droplets left, plus what roots hold on to. */
	class FSoilDepthProcess final : public FBakeProcess
	{
	public:
		using FBakeProcess::FBakeProcess;

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = MassifEnvironmentProcesses::SoilDepth();
			Declaration.DisplayName = TEXT("Soil Depth");
			Declaration.InputChannels = {MassifEnvironmentChannels::SoilDeposition()};
			Declaration.OptionalInputChannels = {MassifEnvironmentChannels::VegetationCover()};
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};

			// Vegetation reads the soil this writes, so the scheduler finds a cycle
			// here and has to be told it is meant.
			Declaration.bParticipatesInFeedback = true;
			Declaration.OutputChannels.Add(MakeChannel(
				MassifEnvironmentChannels::SoilDepth(), EMassifChannelEncoding::Uint16, Params.CellSizeMeters,
				TEXT("m"), EMassifChannelColorMap::Magma));
			return Declaration;
		}

		/** Nothing where no vegetation is in the graph at all, which leaves the soil untouched. */
		static double CoverAt(
			const FMassifEnvironmentContext& Context, FMassifChannelHandle Cover, const FMassifChannelGrid& Grid,
			int32 CellIndex)
		{
			if (!Cover.IsValid())
			{
				return 0.0;
			}
			const FVector2d Position = Grid.CellCentreMeters(CellIndex % Grid.Width, CellIndex / Grid.Width);
			return Context.GetState().SampleScalar(Cover, Position);
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			TArray<double> Deposited;
			ReadCells(
				Context.GetState(), Context.GetInput(MassifEnvironmentChannels::SoilDeposition()), Deposited);

			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::SoilDepth());
			if (Fidelity == EMassifFidelity::Off)
			{
				WriteCells(*Scope, Deposited);
				return;
			}

			const FMassifChannelHandle Cover =
				Context.FindOptionalInput(MassifEnvironmentChannels::VegetationCover());
			const FMassifChannelGrid& Grid = Scope->GetGrid();

			TArrayView<double> Scratch = Scope->GetComponent(0);
			for (int32 CellIndex = 0; CellIndex < Scratch.Num(); ++CellIndex)
			{
				const double Retained =
					1.0 + Params.VegetationSoilRetention * CoverAt(Context, Cover, Grid, CellIndex);
				Scratch[CellIndex] = FMath::Min(Deposited[CellIndex] * Retained, Params.MaxSoilDepthMeters);
			}
			Scope->Commit();
		}
	};

	/** Soil moisture from the topographic wetness index, saturated under standing water. */
	class FWetnessProcess final : public FBakeProcess
	{
	public:
		using FBakeProcess::FBakeProcess;

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = MassifEnvironmentProcesses::Wetness();
			Declaration.DisplayName = TEXT("Ground Wetness");
			Declaration.InputChannels = {
				MassifEnvironmentChannels::Altitude(), MassifEnvironmentChannels::FlowAccumulation(),
				MassifEnvironmentChannels::StandingWaterDepth()};
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};

			FMassifChannelDescriptor Descriptor = MakeChannel(
				MassifEnvironmentChannels::Wetness(), EMassifChannelEncoding::Uint16, Params.CellSizeMeters,
				TEXT("0-1"), EMassifChannelColorMap::BlueWhite);
			Descriptor.DisplayRange = FVector2d(0.0, 1.0);
			Declaration.OutputChannels.Add(Descriptor);
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::Wetness());
			if (Fidelity == EMassifFidelity::Off)
			{
				FillWithConstant(*Scope, 0.0);
				return;
			}

			TArray<double> Measured;
			TArray<float> Accumulation;
			TArray<float> StandingWater;
			ReadCells(Context.GetState(), Context.GetInput(MassifEnvironmentChannels::Altitude()), Measured);
			ReadCellsAsFloats(
				Context.GetState(), Context.GetInput(MassifEnvironmentChannels::FlowAccumulation()), Accumulation);
			ReadCellsAsFloats(
				Context.GetState(), Context.GetInput(MassifEnvironmentChannels::StandingWaterDepth()),
				StandingWater);

			TArray<float> Wetness;
			MassifBakeCpu::ComputeWetness(
				MakeBakeGrid(Scope->GetGrid()), Params, Measured, Accumulation, StandingWater, Wetness);
			WriteCells(*Scope, Wetness);
		}
	};

	/**
	 * The share of arriving water that soaks in rather than running off.
	 *
	 * Both inputs are optional, and for different reasons. Soil depth is written
	 * inside the vegetation feedback loop, so requiring it would drag every reader
	 * of this channel into that loop. Rock is a system a graph may simply not have.
	 * Neither present is bare ground over tight rock, which is what the baseline
	 * stands for.
	 */
	class FInfiltrationProcess final : public FBakeProcess
	{
	public:
		using FBakeProcess::FBakeProcess;

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = MassifEnvironmentProcesses::Infiltration();
			Declaration.DisplayName = TEXT("Infiltration");
			Declaration.OptionalInputChannels = {
				MassifEnvironmentChannels::SoilDepth(), MassifEnvironmentChannels::SoilYield()};
			Declaration.SupportedFidelities = {EMassifFidelity::Cheap, EMassifFidelity::Standard};

			FMassifChannelDescriptor Descriptor = MakeChannel(
				MassifEnvironmentChannels::Infiltration(), EMassifChannelEncoding::Uint16, Params.CellSizeMeters,
				TEXT("0-1"), EMassifChannelColorMap::BlueWhite);
			Descriptor.DisplayRange = FVector2d(0.0, 1.0);
			Declaration.OutputChannels.Add(Descriptor);
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::Infiltration());
			if (Fidelity == EMassifFidelity::Off)
			{
				FillWithConstant(*Scope, Params.BaselineInfiltration);
				return;
			}

			const FMassifChannelHandle Soil = Context.FindOptionalInput(MassifEnvironmentChannels::SoilDepth());
			const FMassifChannelHandle Bedrock = FindBedrock(Context, Fidelity);
			const FMassifChannelGrid& Grid = Scope->GetGrid();

			TArrayView<double> Scratch = Scope->GetComponent(0);
			for (int32 CellIndex = 0; CellIndex < Scratch.Num(); ++CellIndex)
			{
				const FVector2d Position = CellCentre(Grid, CellIndex);
				Scratch[CellIndex] = ShareSoakingIn(
					MassifSampleOptional(Context.GetState(), Soil, Position),
					MassifSampleOptional(Context.GetState(), Bedrock, Position));
			}
			Scope->Commit();
		}

	private:
		/** Cheap is the soil alone: what lies under it is a second system to read. */
		static FMassifChannelHandle FindBedrock(
			const FMassifEnvironmentContext& Context, EMassifFidelity Fidelity)
		{
			if (Fidelity == EMassifFidelity::Cheap)
			{
				return FMassifChannelHandle();
			}
			return Context.FindOptionalInput(MassifEnvironmentChannels::SoilYield());
		}

		double ShareSoakingIn(double SoilDepthMeters, double SoilYield) const
		{
			const double FullDepthMeters =
				FMath::Max(Params.FullInfiltrationSoilDepthMeters, UE_DOUBLE_SMALL_NUMBER);
			const double Saturation = FMath::Clamp(SoilDepthMeters / FullDepthMeters, 0.0, 1.0);
			const double Share = Params.BaselineInfiltration + Params.SoilInfiltrationGain * Saturation
				+ Params.BedrockInfiltrationGain * SoilYield;
			return FMath::Clamp(Share, 0.0, Params.MaxInfiltration);
		}
	};

	/** The rock and the soaking above a point, averaged over the samples that stand above it. */
	struct FUphillMeans
	{
		double SoilYield = 0.0;
		double Infiltration = 0.0;
		int32 SampleCount = 0;
	};

	/** What one pass of the spring solve reads, so the per-cell work is a single call. */
	struct FSpringInputs
	{
		const FMassifEnvironmentState& State;
		FMassifChannelHandle SoilYield;
		TArrayView<const double> AltitudeMeters;
		TArrayView<const float> AccumulationSquareMeters;
		TArrayView<const float> Infiltration;
		int32 RadiusInCells = 1;
	};

	void AddUphillSample(
		const FSpringInputs& Inputs, const FMassifChannelGrid& Grid, int32 Column, int32 Row,
		double CentreAltitudeMeters, FUphillMeans& InOutMeans)
	{
		const int32 CellIndex = Grid.CellIndex(Column, Row);
		if (CellIndex == INDEX_NONE || Inputs.AltitudeMeters[CellIndex] <= CentreAltitudeMeters)
		{
			return;
		}
		InOutMeans.SoilYield += MassifSampleOptional(Inputs.State, Inputs.SoilYield, Grid.CellCentreMeters(Column, Row));
		InOutMeans.Infiltration += Inputs.Infiltration[CellIndex];
		++InOutMeans.SampleCount;
	}

	FUphillMeans MeasureUphill(
		const FSpringInputs& Inputs, const FMassifChannelGrid& Grid, int32 Column, int32 Row)
	{
		FUphillMeans Means;
		const double CentreAltitudeMeters = Inputs.AltitudeMeters[Grid.CellIndex(Column, Row)];
		for (int32 Direction = 0; Direction < MassifBakeNeighbours::Count; ++Direction)
		{
			AddUphillSample(
				Inputs, Grid, Column + MassifBakeNeighbours::OffsetX[Direction] * Inputs.RadiusInCells,
				Row + MassifBakeNeighbours::OffsetY[Direction] * Inputs.RadiusInCells, CentreAltitudeMeters,
				Means);
		}
		if (Means.SampleCount == 0)
		{
			return Means;
		}
		Means.SoilYield /= Means.SampleCount;
		Means.Infiltration /= Means.SampleCount;
		return Means;
	}

	/** Square metres of catchment re-emerging at one cell. */
	double SpringDischargeAt(
		const FMassifEnvironmentBakeParams& Params, const FSpringInputs& Inputs, const FMassifChannelGrid& Grid,
		int32 CellIndex)
	{
		const int32 Column = CellIndex % Grid.Width;
		const int32 Row = CellIndex / Grid.Width;
		const FUphillMeans Uphill = MeasureUphill(Inputs, Grid, Column, Row);
		if (Uphill.SampleCount == 0)
		{
			return 0.0;
		}

		const double LocalYield =
			MassifSampleOptional(Inputs.State, Inputs.SoilYield, Grid.CellCentreMeters(Column, Row));
		const double Span = FMath::Max(Params.SpringYieldDropSpan, UE_DOUBLE_SMALL_NUMBER);
		const double Contrast = FMath::Clamp(
			(Uphill.SoilYield - LocalYield - Params.SpringYieldDropThreshold) / Span, 0.0, 1.0);
		return Inputs.AccumulationSquareMeters[CellIndex] * Uphill.Infiltration * Contrast;
	}

	/**
	 * Cheap reaches half as far as Standard, so it finds the strong contacts and
	 * misses the gradual ones. Both are distances in metres: a reach counted in
	 * cells would mean a different thing at every resolution, and at any resolution
	 * the water is routed at it would fall well inside the rock it is comparing.
	 */
	int32 SpringRadiusInCells(
		const FMassifEnvironmentBakeParams& Params, const FMassifChannelGrid& Grid, EMassifFidelity Fidelity)
	{
		double RadiusMeters = Params.SpringComparisonRadiusMeters;
		if (Fidelity == EMassifFidelity::Cheap)
		{
			RadiusMeters *= 0.5;
		}

		// Never further than the world can answer over. Reaching past every edge at
		// once leaves each point with nothing to compare against, and a world smaller
		// than the reach would come back with no springs anywhere rather than with a
		// complaint.
		const int32 LongestUsefulReach = FMath::Max(1, FMath::Min(Grid.Width, Grid.Height) / 4);
		return FMath::Clamp(
			FMath::RoundToInt32(RadiusMeters / Grid.CellSizeMeters), 1, LongestUsefulReach);
	}

	/**
	 * Where water that soaked in upslope comes back to the surface.
	 *
	 * The rock decides. Water sinks into permeable ground and re-emerges where the
	 * permeability falls away under it - at a contact, or where a slope steepens
	 * into a bench. This is why a stream can start mid-slope with no catchment
	 * above it at all, which routed surface flow on its own can never produce.
	 */
	class FSpringProcess final : public FBakeProcess
	{
	public:
		using FBakeProcess::FBakeProcess;

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = MassifEnvironmentProcesses::Springs();
			Declaration.DisplayName = TEXT("Springs");
			Declaration.InputChannels = {
				MassifEnvironmentChannels::Altitude(), MassifEnvironmentChannels::FlowAccumulation(),
				MassifEnvironmentChannels::Infiltration()};
			Declaration.OptionalInputChannels = {MassifEnvironmentChannels::SoilYield()};
			Declaration.SupportedFidelities = {EMassifFidelity::Cheap, EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(MakeChannel(
				MassifEnvironmentChannels::SpringDischarge(), EMassifChannelEncoding::Uint16,
				Params.CellSizeMeters, TEXT("m2"), EMassifChannelColorMap::BlueWhite));
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::SpringDischarge());
			if (Fidelity == EMassifFidelity::Off)
			{
				FillWithConstant(*Scope, 0.0);
				return;
			}

			TArray<double> AltitudeMeters;
			TArray<float> AccumulationSquareMeters;
			TArray<float> Infiltration;
			ReadCells(Context.GetState(), Context.GetInput(MassifEnvironmentChannels::Altitude()), AltitudeMeters);
			ReadCellsAsFloats(
				Context.GetState(), Context.GetInput(MassifEnvironmentChannels::FlowAccumulation()),
				AccumulationSquareMeters);
			ReadCellsAsFloats(
				Context.GetState(), Context.GetInput(MassifEnvironmentChannels::Infiltration()), Infiltration);

			const FMassifChannelGrid& Grid = Scope->GetGrid();
			const FSpringInputs Inputs{
				Context.GetState(), Context.FindOptionalInput(MassifEnvironmentChannels::SoilYield()),
				AltitudeMeters, AccumulationSquareMeters, Infiltration,
				SpringRadiusInCells(Params, Grid, Fidelity)};

			TArrayView<double> Scratch = Scope->GetComponent(0);
			for (int32 CellIndex = 0; CellIndex < Scratch.Num(); ++CellIndex)
			{
				Scratch[CellIndex] = SpringDischargeAt(Params, Inputs, Grid, CellIndex);
			}
			Scope->Commit();
		}
	};

	/** Half-monthly, matching the slicing the climate and the snowpack are solved on. */
	constexpr int32 SeasonalFlowSliceCount = 24;

	/**
	 * The seasonal channels the flow is shaped by, where the graph has them.
	 *
	 * All three are optional. The water solve has to stand on its own in a graph
	 * with no weather in it, and a year nothing is known about is a year whose
	 * water arrives evenly.
	 */
	struct FSeasonalWaterInputs
	{
		const FMassifEnvironmentState& State;
		FMassifChannelHandle SeasonalPrecipitation;
		FMassifChannelHandle SnowFraction;
		FMassifChannelHandle SnowDepth;

		/** An unsolved year falls at one rate throughout; only the ratio between slices is read. */
		double PrecipitationAt(FVector2d PositionMeters, int32 TimeSlice) const
		{
			if (!SeasonalPrecipitation.IsValid())
			{
				return 1.0;
			}
			return MassifSampleOptional(State, SeasonalPrecipitation, PositionMeters, TimeSlice);
		}

		double SnowShareAt(FVector2d PositionMeters, int32 TimeSlice) const
		{
			return MassifSampleOptional(State, SnowFraction, PositionMeters, TimeSlice);
		}

		double SnowDepthMetersAt(FVector2d PositionMeters, int32 TimeSlice) const
		{
			return MassifSampleOptional(State, SnowDepth, PositionMeters, TimeSlice);
		}
	};

	double RainAt(const FSeasonalWaterInputs& Inputs, FVector2d PositionMeters, int32 TimeSlice)
	{
		return Inputs.PrecipitationAt(PositionMeters, TimeSlice)
			* (1.0 - Inputs.SnowShareAt(PositionMeters, TimeSlice));
	}

	/** Water leaving the pack: the drop in what is lying since the slice before, where it is a drop. */
	double MeltAt(const FSeasonalWaterInputs& Inputs, FVector2d PositionMeters, int32 TimeSlice)
	{
		const int32 PreviousSlice = (TimeSlice + SeasonalFlowSliceCount - 1) % SeasonalFlowSliceCount;
		const double DropMeters = Inputs.SnowDepthMetersAt(PositionMeters, PreviousSlice)
			- Inputs.SnowDepthMetersAt(PositionMeters, TimeSlice);
		return FMath::Max(DropMeters, 0.0);
	}

	/** A year's worth of arriving and releasing at one point, in whatever units the channels speak. */
	struct FYearWaterBalance
	{
		double RainTotal = 0.0;
		double SnowfallTotal = 0.0;
		double MeltTotal = 0.0;
	};

	FYearWaterBalance MeasureYear(const FSeasonalWaterInputs& Inputs, FVector2d PositionMeters)
	{
		FYearWaterBalance Balance;
		for (int32 TimeSlice = 0; TimeSlice < SeasonalFlowSliceCount; ++TimeSlice)
		{
			const double Fall = Inputs.PrecipitationAt(PositionMeters, TimeSlice);
			const double SnowShare = Inputs.SnowShareAt(PositionMeters, TimeSlice);
			Balance.RainTotal += Fall * (1.0 - SnowShare);
			Balance.SnowfallTotal += Fall * SnowShare;
			Balance.MeltTotal += MeltAt(Inputs, PositionMeters, TimeSlice);
		}
		return Balance;
	}

	/** A slice's share of a year's total, spread evenly over a year that has no total to share. */
	double ShareOfYear(double SliceValue, double YearTotal)
	{
		if (YearTotal <= UE_DOUBLE_SMALL_NUMBER)
		{
			return 1.0 / SeasonalFlowSliceCount;
		}
		return SliceValue / YearTotal;
	}

	/** Cheap keeps no pack: all of the year's water counts as arriving when it falls. */
	double SnowShareOfYear(const FYearWaterBalance& Balance, EMassifFidelity Fidelity)
	{
		if (Fidelity == EMassifFidelity::Cheap)
		{
			return 0.0;
		}
		const double FallTotal = Balance.RainTotal + Balance.SnowfallTotal;
		if (FallTotal <= UE_DOUBLE_SMALL_NUMBER)
		{
			return 0.0;
		}
		return Balance.SnowfallTotal / FallTotal;
	}

	/**
	 * How much of a year's water moves in one slice, against an even year.
	 *
	 * 1 is the annual mean, so the slices of an unsolved year all read the flow the
	 * catchment alone gives. Rain counts when it lands and snow counts when it
	 * leaves the pack, which is what puts the pulse in May rather than in January.
	 */
	double SliceFlowScale(
		const FSeasonalWaterInputs& Inputs, const FYearWaterBalance& Balance, EMassifFidelity Fidelity,
		FVector2d PositionMeters, int32 TimeSlice)
	{
		const double SnowShare = SnowShareOfYear(Balance, Fidelity);
		const double RainPart = (1.0 - SnowShare)
			* ShareOfYear(RainAt(Inputs, PositionMeters, TimeSlice), Balance.RainTotal);
		const double MeltPart =
			SnowShare * ShareOfYear(MeltAt(Inputs, PositionMeters, TimeSlice), Balance.MeltTotal);
		return (RainPart + MeltPart) * SeasonalFlowSliceCount;
	}

	/**
	 * What is actually moving through a point in a given half-month.
	 *
	 * The routed catchment is a fixed area; this is the water crossing it. Two
	 * things shape it: the share that soaked away instead of running off, and how
	 * much of the year's water the half-month delivers. Wet meadows sit below
	 * late-lying snow because of the second.
	 */
	class FSeasonalFlowProcess final : public FBakeProcess
	{
	public:
		using FBakeProcess::FBakeProcess;

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = MassifEnvironmentProcesses::SeasonalFlow();
			Declaration.DisplayName = TEXT("Seasonal Discharge");
			Declaration.InputChannels = {
				MassifEnvironmentChannels::FlowAccumulation(), MassifEnvironmentChannels::Infiltration()};
			Declaration.OptionalInputChannels = {
				SeasonalPrecipitationChannel(), MassifEnvironmentChannels::SnowFraction(),
				MassifEnvironmentChannels::SnowDepth()};
			Declaration.SupportedFidelities = {EMassifFidelity::Cheap, EMassifFidelity::Standard};

			FMassifChannelDescriptor Descriptor = MakeChannel(
				MassifEnvironmentChannels::SeasonalFlow(), EMassifChannelEncoding::Uint16,
				Params.SeasonalFlowCellSizeMeters, TEXT("m2"), EMassifChannelColorMap::Viridis);
			Descriptor.TimeSliceCount = SeasonalFlowSliceCount;
			Declaration.OutputChannels.Add(Descriptor);
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			// The water is routed at the hydrology's resolution and stored at a coarser
			// one, because a year of half-monthly slices at 8 m is a quarter of a
			// gigabyte. Everything read here is therefore reduced onto the output's own
			// grid before a cell index means anything.
			const FMassifChannelGrid& Grid =
				Context.GetState().GetGrid(Context.GetOutput(MassifEnvironmentChannels::SeasonalFlow()));

			TArray<double> FlowSquareMeters;
			ReadLargestPerCell(
				Context.GetState(), Context.GetInput(MassifEnvironmentChannels::FlowAccumulation()), Grid,
				FlowSquareMeters);

			if (Fidelity == EMassifFidelity::Off)
			{
				WriteEverySliceAlike(Context, FlowSquareMeters);
				return;
			}

			TakeInfiltrationOff(Context, Grid, FlowSquareMeters);
			WriteSeasons(Context, Fidelity, FlowSquareMeters);
		}

	private:
		static void WriteEverySliceAlike(
			FMassifEnvironmentContext& Context, TArrayView<const double> FlowSquareMeters)
		{
			for (int32 TimeSlice = 0; TimeSlice < SeasonalFlowSliceCount; ++TimeSlice)
			{
				const TUniquePtr<FMassifChannelWriteScope> Scope =
					Context.OpenOutput(MassifEnvironmentChannels::SeasonalFlow(), TimeSlice);
				WriteCells(*Scope, FlowSquareMeters);
			}
		}

		/**
		 * Ground that soaks water up hands less of it to the stream below.
		 *
		 * Applied to the accumulated figure rather than to each cell's own
		 * contribution before it is routed: MassifBakeCpu::RouteFlow takes no
		 * per-cell input weight, and the routing is not this module's to change. The
		 * loss a point takes is therefore its own infiltration rather than its
		 * catchment's, which understates a stream fed across a permeability change
		 * and is exact wherever a catchment is of one piece.
		 */
		static void TakeInfiltrationOff(
			const FMassifEnvironmentContext& Context, const FMassifChannelGrid& Grid,
			TArray<double>& InOutFlowSquareMeters)
		{
			const FMassifChannelHandle Infiltration =
				Context.GetInput(MassifEnvironmentChannels::Infiltration());
			for (int32 CellIndex = 0; CellIndex < InOutFlowSquareMeters.Num(); ++CellIndex)
			{
				const double Share =
					Context.GetState().SampleScalar(Infiltration, CellCentre(Grid, CellIndex));
				InOutFlowSquareMeters[CellIndex] *= 1.0 - Share;
			}
		}

		static FSeasonalWaterInputs MakeSeasonalInputs(const FMassifEnvironmentContext& Context)
		{
			return FSeasonalWaterInputs{
				Context.GetState(), Context.FindOptionalInput(SeasonalPrecipitationChannel()),
				Context.FindOptionalInput(MassifEnvironmentChannels::SnowFraction()),
				Context.FindOptionalInput(MassifEnvironmentChannels::SnowDepth())};
		}

		void WriteSeasons(
			FMassifEnvironmentContext& Context, EMassifFidelity Fidelity,
			TArrayView<const double> RunoffSquareMeters) const
		{
			const FSeasonalWaterInputs Inputs = MakeSeasonalInputs(Context);
			const FMassifChannelGrid& Grid =
				Context.GetState().GetGrid(Context.GetOutput(MassifEnvironmentChannels::SeasonalFlow()));

			// Measured once and kept: every slice divides by the same year, and
			// re-walking it per slice would read the seasonal channels a square number
			// of times.
			TArray<FYearWaterBalance> Balances;
			Balances.SetNum(Grid.CellCount());
			for (int32 CellIndex = 0; CellIndex < Balances.Num(); ++CellIndex)
			{
				Balances[CellIndex] = MeasureYear(Inputs, CellCentre(Grid, CellIndex));
			}

			for (int32 TimeSlice = 0; TimeSlice < SeasonalFlowSliceCount; ++TimeSlice)
			{
				WriteSlice(Context, Inputs, Fidelity, Balances, RunoffSquareMeters, TimeSlice);
			}
		}

		static void WriteSlice(
			FMassifEnvironmentContext& Context, const FSeasonalWaterInputs& Inputs, EMassifFidelity Fidelity,
			TArrayView<const FYearWaterBalance> Balances, TArrayView<const double> RunoffSquareMeters,
			int32 TimeSlice)
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::SeasonalFlow(), TimeSlice);
			const FMassifChannelGrid& Grid = Scope->GetGrid();

			TArrayView<double> Scratch = Scope->GetComponent(0);
			for (int32 CellIndex = 0; CellIndex < Scratch.Num(); ++CellIndex)
			{
				Scratch[CellIndex] = RunoffSquareMeters[CellIndex]
					* SliceFlowScale(
						Inputs, Balances[CellIndex], Fidelity, CellCentre(Grid, CellIndex), TimeSlice);
			}
			Scope->Commit();
		}
	};
}

void MassifRegisterAltitudeProcess(
	FMassifEnvironmentScheduler& Scheduler, const FMassifEnvironmentBakeParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FAltitudeProcess>(Params));
}

void MassifRegisterWaterBalanceProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifEnvironmentBakeParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FInfiltrationProcess>(Params));
	Scheduler.RegisterProcess(MakeShared<FSpringProcess>(Params));
	Scheduler.RegisterProcess(MakeShared<FSeasonalFlowProcess>(Params));
}

void MassifRegisterEnvironmentProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifEnvironmentParams& Params)
{
	MassifRegisterAltitudeProcess(Scheduler, Params.Water);
	Scheduler.RegisterProcess(MakeShared<FDepressionFillProcess>(Params.Water));
	Scheduler.RegisterProcess(MakeShared<FStandingWaterProcess>(Params.Water));
	Scheduler.RegisterProcess(MakeShared<FFlowRoutingProcess>(Params.Water));
	Scheduler.RegisterProcess(MakeShared<FSoilDepositionProcess>(Params.Water));
	Scheduler.RegisterProcess(MakeShared<FSoilDepthProcess>(Params.Water));
	Scheduler.RegisterProcess(MakeShared<FWetnessProcess>(Params.Water));
	MassifRegisterWaterBalanceProcesses(Scheduler, Params.Water);
	MassifRegisterLithologyProcesses(Scheduler, Params.Lithology);
	MassifRegisterSunProcesses(Scheduler, Params.Sun);
	MassifRegisterClimateProcesses(Scheduler, Params.Climate);
	MassifRegisterWindProcesses(Scheduler, Params.Wind);
	MassifRegisterPrecipitationProcesses(Scheduler, Params.Precipitation);
	MassifRegisterSnowProcesses(Scheduler, Params.Snow);
	MassifRegisterVegetationProcesses(Scheduler, Params.Vegetation);
}

void MassifRegisterEnvironmentProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifEnvironmentBakeParams& WaterParams)
{
	FMassifEnvironmentParams Params;
	Params.Water = WaterParams;
	MassifRegisterEnvironmentProcesses(Scheduler, Params);
}

TUniquePtr<FMassifEnvironmentScheduler> MassifPrepareEnvironment(
	TSharedRef<const IMassifElevationSource> Elevation,
	const FMassifGeoreference& Georeference,
	uint32 MasterSeed,
	const FMassifEnvironmentBakeParams& Params,
	FString& OutError)
{
	TUniquePtr<FMassifEnvironmentScheduler> Scheduler =
		MakeUnique<FMassifEnvironmentScheduler>(Elevation, Georeference, MasterSeed);
	MassifRegisterEnvironmentProcesses(*Scheduler, Params);

	if (!Scheduler->BuildSchedule(OutError))
	{
		return nullptr;
	}
	return Scheduler;
}

TUniquePtr<FMassifEnvironmentScheduler> MassifBuildEnvironment(
	TSharedRef<const IMassifElevationSource> Elevation,
	const FMassifGeoreference& Georeference,
	uint32 MasterSeed,
	const FMassifEnvironmentBakeParams& Params,
	FString& OutError)
{
	TUniquePtr<FMassifEnvironmentScheduler> Scheduler =
		MassifPrepareEnvironment(Elevation, Georeference, MasterSeed, Params, OutError);
	if (!Scheduler.IsValid())
	{
		return nullptr;
	}

	Scheduler->Execute();
	return Scheduler;
}

TSharedRef<const FMassifRiverNetwork> MassifExtractRivers(
	const FMassifEnvironmentState& State, const FMassifRiverParams& Params)
{
	const FMassifChannelHandle MeasuredChannel = State.FindChannel(MassifEnvironmentChannels::Altitude());
	const FMassifChannelHandle FilledChannel = State.FindChannel(MassifEnvironmentChannels::FilledAltitude());
	const FMassifChannelHandle FlowChannel = State.FindChannel(MassifEnvironmentChannels::FlowAccumulation());
	if (!MeasuredChannel.IsValid() || !FilledChannel.IsValid() || !FlowChannel.IsValid())
	{
		return FMassifRiverNetwork::Create({});
	}

	TArray<double> Measured;
	TArray<double> Filled;
	TArray<float> Accumulation;
	ReadCells(State, MeasuredChannel, Measured);
	ReadCells(State, FilledChannel, Filled);
	ReadCellsAsFloats(State, FlowChannel, Accumulation);

	return MassifBakeCpu::ExtractRivers(
		MakeBakeGrid(State.GetGrid(FilledChannel)), Params, Measured, Filled, Accumulation);
}
