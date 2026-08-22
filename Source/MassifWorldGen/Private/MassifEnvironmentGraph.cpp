#include "MassifEnvironmentGraph.h"

#include "MassifEnvironmentBakeGrid.h"
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

	const FName& Wetness()
	{
		static const FName Name(TEXT("Hydrology.Wetness"));
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

	void WriteConstant(FMassifChannelWriteScope& Scope, double Value)
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
				WriteConstant(*Scope, 0.0);
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
				WriteConstant(*Scope, Grid.CellAreaSquareMeters());
				return;
			}

			TArray<double> Filled;
			ReadCells(Context.GetState(), Context.GetInput(MassifEnvironmentChannels::FilledAltitude()), Filled);

			TArray<float> AccumulationSquareMeters;
			MassifBakeCpu::RouteFlow(Grid, Params, Filled, AccumulationSquareMeters);
			WriteCells(*Scope, AccumulationSquareMeters);
		}
	};

	/** Where droplets running down the surface dropped what they were carrying. */
	class FSoilDepthProcess final : public FBakeProcess
	{
	public:
		using FBakeProcess::FBakeProcess;

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = MassifEnvironmentProcesses::SoilDepth();
			Declaration.InputChannels = {MassifEnvironmentChannels::FilledAltitude()};
			Declaration.SupportedFidelities = {EMassifFidelity::Standard};
			Declaration.OutputChannels.Add(MakeChannel(
				MassifEnvironmentChannels::SoilDepth(), EMassifChannelEncoding::Uint16, Params.CellSizeMeters,
				TEXT("m"), EMassifChannelColorMap::Magma));
			return Declaration;
		}

		void Execute(FMassifEnvironmentContext& Context, EMassifFidelity Fidelity) const override
		{
			const TUniquePtr<FMassifChannelWriteScope> Scope =
				Context.OpenOutput(MassifEnvironmentChannels::SoilDepth());
			if (Fidelity == EMassifFidelity::Off)
			{
				// Regolith on ground nothing has moved, everywhere.
				WriteConstant(*Scope, Params.BaseSoilDepthMeters);
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

	/** Soil moisture from the topographic wetness index, saturated under standing water. */
	class FWetnessProcess final : public FBakeProcess
	{
	public:
		using FBakeProcess::FBakeProcess;

		FMassifProcessDeclaration Declare() const override
		{
			FMassifProcessDeclaration Declaration;
			Declaration.Name = MassifEnvironmentProcesses::Wetness();
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
				WriteConstant(*Scope, 0.0);
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
}

void MassifRegisterEnvironmentProcesses(
	FMassifEnvironmentScheduler& Scheduler, const FMassifEnvironmentBakeParams& Params)
{
	Scheduler.RegisterProcess(MakeShared<FAltitudeProcess>(Params));
	Scheduler.RegisterProcess(MakeShared<FDepressionFillProcess>(Params));
	Scheduler.RegisterProcess(MakeShared<FStandingWaterProcess>(Params));
	Scheduler.RegisterProcess(MakeShared<FFlowRoutingProcess>(Params));
	Scheduler.RegisterProcess(MakeShared<FSoilDepthProcess>(Params));
	Scheduler.RegisterProcess(MakeShared<FWetnessProcess>(Params));
}

TUniquePtr<FMassifEnvironmentScheduler> MassifBuildEnvironment(
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
