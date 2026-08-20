#include "MassifEnvironmentBake.h"

#include "MassifEnvironmentBakeGrid.h"
#include "MassifHeightmap.h"
#include "MassifSeed.h"
#include "MassifStateHash.h"

namespace
{
	constexpr double QuantisationSteps = 65535.0;

	/** Flow accumulation spans a hillside to a river, so it is quantised in log space. */
	bool IsChannelLogarithmic(EMassifBakeChannel Channel)
	{
		return Channel == EMassifBakeChannel::FlowAccumulation;
	}

	double CompressForQuantisation(double Value, bool bLogarithmic)
	{
		if (!bLogarithmic)
		{
			return Value;
		}
		return FMath::Loge(1.0 + FMath::Max(Value, 0.0));
	}

	double ExpandFromQuantisation(double Value, bool bLogarithmic)
	{
		if (!bLogarithmic)
		{
			return Value;
		}
		return FMath::Exp(Value) - 1.0;
	}
}

const TCHAR* MassifBakeChannelName(EMassifBakeChannel Channel)
{
	switch (Channel)
	{
		case EMassifBakeChannel::StandingWaterDepth: return TEXT("Standing water");
		case EMassifBakeChannel::FlowAccumulation: return TEXT("Flow accumulation");
		case EMassifBakeChannel::Wetness: return TEXT("Wetness");
		case EMassifBakeChannel::SoilDepth: return TEXT("Soil depth");
		default: return TEXT("Unknown");
	}
}

const TCHAR* MassifBakeChannelUnit(EMassifBakeChannel Channel)
{
	switch (Channel)
	{
		case EMassifBakeChannel::StandingWaterDepth: return TEXT("m");
		case EMassifBakeChannel::FlowAccumulation: return TEXT("m2");
		case EMassifBakeChannel::Wetness: return TEXT("0-1");
		case EMassifBakeChannel::SoilDepth: return TEXT("m");
		default: return TEXT("");
	}
}

FMassifBakeGrid FMassifBakeGrid::FromHeightmap(const FMassifHeightmap& Heightmap, double CellSizeMeters)
{
	FMassifBakeGrid Grid;
	Grid.CellSizeMeters = FMath::Max(CellSizeMeters, UE_DOUBLE_KINDA_SMALL_NUMBER);
	Grid.WorldSizeXMeters = Heightmap.GetWorldSizeXMeters();
	Grid.WorldSizeYMeters = Heightmap.GetWorldSizeYMeters();
	Grid.Width = FMath::Max(2, FMath::RoundToInt32(Grid.WorldSizeXMeters / Grid.CellSizeMeters));
	Grid.Height = FMath::Max(2, FMath::RoundToInt32(Grid.WorldSizeYMeters / Grid.CellSizeMeters));
	return Grid;
}

FVector2d FMassifBakeGrid::CellCentreMeters(int32 Column, int32 Row) const
{
	// Row 0 is north, as in the elevation model, so rows run against +Y.
	const double X = -0.5 * WorldSizeXMeters + (Column + 0.5) * CellSizeMeters;
	const double Y = 0.5 * WorldSizeYMeters - (Row + 0.5) * CellSizeMeters;
	return FVector2d(X, Y);
}

void FMassifBakeSurfaces::Allocate(int32 CellCount)
{
	MeasuredAltitudeMeters.SetNumZeroed(CellCount);
	FilledAltitudeMeters.SetNumZeroed(CellCount);
	StandingWaterDepthMeters.SetNumZeroed(CellCount);
	FlowAccumulationSquareMeters.SetNumZeroed(CellCount);
	SoilDepthMeters.SetNumZeroed(CellCount);
	Wetness.SetNumZeroed(CellCount);
}

const int32 MassifBakeNeighbours::OffsetX[MassifBakeNeighbours::Count] = { 1, 1, 0, -1, -1, -1, 0, 1 };
const int32 MassifBakeNeighbours::OffsetY[MassifBakeNeighbours::Count] = { 0, 1, 1, 1, 0, -1, -1, -1 };
const double MassifBakeNeighbours::DistanceInCells[MassifBakeNeighbours::Count] = {
	1.0, UE_DOUBLE_SQRT_2, 1.0, UE_DOUBLE_SQRT_2, 1.0, UE_DOUBLE_SQRT_2, 1.0, UE_DOUBLE_SQRT_2 };
const double MassifBakeNeighbours::ContourWidthInCells[MassifBakeNeighbours::Count] = {
	0.5, 0.354, 0.5, 0.354, 0.5, 0.354, 0.5, 0.354 };

TSharedRef<const FMassifEnvironmentBake> FMassifEnvironmentBake::Build(
	const FMassifHeightmap& Heightmap, uint32 MasterSeed, const FMassifEnvironmentBakeParams& Params)
{
	return BuildOnCpu(Heightmap, MasterSeed, Params);
}

TSharedRef<const FMassifEnvironmentBake> FMassifEnvironmentBake::BuildOnCpu(
	const FMassifHeightmap& Heightmap, uint32 MasterSeed, const FMassifEnvironmentBakeParams& Params)
{
	const FMassifBakeGrid Grid = FMassifBakeGrid::FromHeightmap(Heightmap, Params.CellSizeMeters);
	const uint32 DropletSeed = FMassifSeed(MasterSeed).MakeChannelSeed(TEXT("Environment.Droplets"));

	FMassifBakeSurfaces Surfaces;
	Surfaces.Allocate(Grid.CellCount());

	MassifBakeCpu::SampleAltitudes(Heightmap, Grid, Surfaces.MeasuredAltitudeMeters);
	MassifBakeCpu::FillDepressions(Grid, Params, Surfaces.MeasuredAltitudeMeters, Surfaces.FilledAltitudeMeters);
	MassifBakeCpu::MeasureStandingWater(
		Surfaces.MeasuredAltitudeMeters, Surfaces.FilledAltitudeMeters, Surfaces.StandingWaterDepthMeters);
	MassifBakeCpu::RouteFlow(
		Grid, Params, Surfaces.FilledAltitudeMeters, Surfaces.FlowAccumulationSquareMeters);
	MassifBakeCpu::RunDroplets(
		Grid, Params, DropletSeed, Surfaces.FilledAltitudeMeters, Surfaces.SoilDepthMeters);
	MassifBakeCpu::ComputeWetness(
		Grid, Params, Surfaces.MeasuredAltitudeMeters, Surfaces.FlowAccumulationSquareMeters,
		Surfaces.StandingWaterDepthMeters, Surfaces.Wetness);

	TSharedRef<FMassifEnvironmentBake> Bake(new FMassifEnvironmentBake());
	Bake->Rivers = MassifBakeCpu::ExtractRivers(
		Grid, Params.Rivers, Surfaces.MeasuredAltitudeMeters, Surfaces.FilledAltitudeMeters,
		Surfaces.FlowAccumulationSquareMeters);
	Bake->Width = Grid.Width;
	Bake->Height = Grid.Height;
	Bake->CellSizeMeters = Grid.CellSizeMeters;
	Bake->WorldSizeXMeters = Grid.WorldSizeXMeters;
	Bake->WorldSizeYMeters = Grid.WorldSizeYMeters;
	Bake->StoreChannel(EMassifBakeChannel::StandingWaterDepth, Surfaces.StandingWaterDepthMeters, false);
	Bake->StoreChannel(EMassifBakeChannel::FlowAccumulation, Surfaces.FlowAccumulationSquareMeters, true);
	Bake->StoreChannel(EMassifBakeChannel::Wetness, Surfaces.Wetness, false);
	Bake->StoreChannel(EMassifBakeChannel::SoilDepth, Surfaces.SoilDepthMeters, false);
	return Bake;
}

void FMassifEnvironmentBake::StoreChannel(
	EMassifBakeChannel ChannelId, TArrayView<const float> Values, bool bLogarithmic)
{
	FChannel& Channel = Channels[static_cast<int32>(ChannelId)];
	Channel.bLogarithmic = bLogarithmic;
	Channel.MinValue = TNumericLimits<double>::Max();
	Channel.MaxValue = TNumericLimits<double>::Lowest();
	for (float Value : Values)
	{
		Channel.MinValue = FMath::Min(Channel.MinValue, static_cast<double>(Value));
		Channel.MaxValue = FMath::Max(Channel.MaxValue, static_cast<double>(Value));
	}

	Channel.Samples.SetNumZeroed(Values.Num());
	const double CompressedMin = CompressForQuantisation(Channel.MinValue, bLogarithmic);
	const double CompressedSpan = CompressForQuantisation(Channel.MaxValue, bLogarithmic) - CompressedMin;
	if (CompressedSpan <= UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return;
	}

	for (int32 SampleIndex = 0; SampleIndex < Values.Num(); ++SampleIndex)
	{
		const double Compressed = CompressForQuantisation(Values[SampleIndex], bLogarithmic);
		const double Fraction = FMath::Clamp((Compressed - CompressedMin) / CompressedSpan, 0.0, 1.0);
		Channel.Samples[SampleIndex] = static_cast<uint16>(FMath::RoundToInt32(Fraction * QuantisationSteps));
	}
}

const FMassifEnvironmentBake::FChannel& FMassifEnvironmentBake::GetChannel(EMassifBakeChannel Channel) const
{
	return Channels[static_cast<int32>(Channel)];
}

double FMassifEnvironmentBake::DecodeCell(const FChannel& Channel, int32 Column, int32 Row) const
{
	const int32 SampleIndex = Row * Width + Column;
	if (!Channel.Samples.IsValidIndex(SampleIndex))
	{
		return 0.0;
	}

	const double Fraction = Channel.Samples[SampleIndex] / QuantisationSteps;
	const double CompressedMin = CompressForQuantisation(Channel.MinValue, Channel.bLogarithmic);
	const double CompressedMax = CompressForQuantisation(Channel.MaxValue, Channel.bLogarithmic);
	return ExpandFromQuantisation(FMath::Lerp(CompressedMin, CompressedMax, Fraction), Channel.bLogarithmic);
}

double FMassifEnvironmentBake::ReadCell(EMassifBakeChannel Channel, int32 Column, int32 Row) const
{
	const int32 ClampedColumn = FMath::Clamp(Column, 0, Width - 1);
	const int32 ClampedRow = FMath::Clamp(Row, 0, Height - 1);
	return DecodeCell(GetChannel(Channel), ClampedColumn, ClampedRow);
}

double FMassifEnvironmentBake::Sample(EMassifBakeChannel ChannelId, FVector2d PositionMeters) const
{
	if (Width <= 0 || Height <= 0)
	{
		return 0.0;
	}

	const double ColumnCoordinate = FMath::Clamp(
		(PositionMeters.X + 0.5 * WorldSizeXMeters) / CellSizeMeters - 0.5, 0.0, Width - 1.0);
	const double RowCoordinate = FMath::Clamp(
		(0.5 * WorldSizeYMeters - PositionMeters.Y) / CellSizeMeters - 0.5, 0.0, Height - 1.0);

	const int32 Column = FMath::Min(FMath::FloorToInt32(ColumnCoordinate), Width - 1);
	const int32 Row = FMath::Min(FMath::FloorToInt32(RowCoordinate), Height - 1);
	const int32 NextColumn = FMath::Min(Column + 1, Width - 1);
	const int32 NextRow = FMath::Min(Row + 1, Height - 1);

	const FChannel& Channel = GetChannel(ChannelId);
	const double NorthEdge = FMath::Lerp(
		DecodeCell(Channel, Column, Row), DecodeCell(Channel, NextColumn, Row), ColumnCoordinate - Column);
	const double SouthEdge = FMath::Lerp(
		DecodeCell(Channel, Column, NextRow), DecodeCell(Channel, NextColumn, NextRow), ColumnCoordinate - Column);
	return FMath::Lerp(NorthEdge, SouthEdge, RowCoordinate - Row);
}

FVector2d FMassifEnvironmentBake::GetCellCentreMeters(int32 Column, int32 Row) const
{
	return FVector2d(
		-0.5 * WorldSizeXMeters + (Column + 0.5) * CellSizeMeters,
		0.5 * WorldSizeYMeters - (Row + 0.5) * CellSizeMeters);
}

double FMassifEnvironmentBake::GetChannelMinimum(EMassifBakeChannel Channel) const
{
	return GetChannel(Channel).MinValue;
}

double FMassifEnvironmentBake::GetChannelMaximum(EMassifBakeChannel Channel) const
{
	return GetChannel(Channel).MaxValue;
}

uint64 FMassifEnvironmentBake::ComputeContentHash() const
{
	FMassifStateHash Hash;
	Hash.AppendInt(Width);
	Hash.AppendInt(Height);
	for (int32 ChannelIndex = 0; ChannelIndex < static_cast<int32>(EMassifBakeChannel::Count); ++ChannelIndex)
	{
		const FChannel& Channel = Channels[ChannelIndex];
		Hash.Append(Channel.Samples.GetData(), Channel.Samples.Num() * sizeof(uint16));
		Hash.AppendInt(FMath::RoundToInt64(Channel.MinValue * 1000.0));
		Hash.AppendInt(FMath::RoundToInt64(Channel.MaxValue * 1000.0));
	}
	return Hash.Finalize();
}
