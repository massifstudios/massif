#include "MassifEnvironmentBakeGrid.h"

#include "MassifHeightmap.h"

const int32 MassifBakeNeighbours::OffsetX[MassifBakeNeighbours::Count] = { 1, 1, 0, -1, -1, -1, 0, 1 };
const int32 MassifBakeNeighbours::OffsetY[MassifBakeNeighbours::Count] = { 0, 1, 1, 1, 0, -1, -1, -1 };
const double MassifBakeNeighbours::DistanceInCells[MassifBakeNeighbours::Count] = {
	1.0, UE_DOUBLE_SQRT_2, 1.0, UE_DOUBLE_SQRT_2, 1.0, UE_DOUBLE_SQRT_2, 1.0, UE_DOUBLE_SQRT_2 };
const double MassifBakeNeighbours::ContourWidthInCells[MassifBakeNeighbours::Count] = {
	0.5, 0.354, 0.5, 0.354, 0.5, 0.354, 0.5, 0.354 };

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
