#include "MassifRiverField.h"

#include "MassifRiverNetwork.h"

namespace
{
	/**
	 * How much of a reach the gradient at a point is measured over. One span is
	 * six metres of a surface handed down as a running minimum, so its own drop is
	 * mostly the step where the minimum last locked on; a channel's steepness is a
	 * property of a stretch of it rather than of one node pair.
	 */
	constexpr double GradientWindowMeters = 40.0;

	/** Bounds on the index's cell size, so neither a brook nor a lake sets it absurdly. */
	constexpr double MinCellSizeMeters = 8.0;
	constexpr double MaxCellSizeMeters = 128.0;

	/** Rotated a quarter turn from the flow, so it points at the right bank. */
	FVector2d AcrossFlow(FVector2d FlowDirection)
	{
		return FVector2d(FlowDirection.Y, -FlowDirection.X);
	}
}

// ---------------------------------------------------------------------------
// Building the index
// ---------------------------------------------------------------------------

TSharedRef<const FMassifRiverField> FMassifRiverField::Build(
	TSharedRef<const FMassifRiverNetwork> InNetwork, double InBankReachMeters)
{
	TSharedRef<FMassifRiverField> Field(new FMassifRiverField());
	Field->BankReachMeters = FMath::Max(InBankReachMeters, 0.0);
	Field->CollectSegments(InNetwork.Get());
	Field->MeasureGrid();
	Field->IndexSegments();
	return Field;
}

void FMassifRiverField::CollectSegments(const FMassifRiverNetwork& InNetwork)
{
	for (const FMassifRiverReach& Reach : InNetwork.GetReaches())
	{
		AppendReachSegments(Reach);
	}
}

namespace
{
	/** Cumulative chord length along a reach, which the gradient window is measured in. */
	void MeasureReachArclength(const FMassifRiverReach& Reach, TArray<double>& OutAlongMeters)
	{
		OutAlongMeters.Reset(Reach.Nodes.Num());
		OutAlongMeters.Add(0.0);
		for (int32 NodeIndex = 1; NodeIndex < Reach.Nodes.Num(); ++NodeIndex)
		{
			const double ChordMeters =
				(Reach.Nodes[NodeIndex].PositionMeters - Reach.Nodes[NodeIndex - 1].PositionMeters).Length();
			OutAlongMeters.Add(OutAlongMeters.Last() + ChordMeters);
		}
	}

	int32 WalkBackToWindow(TArrayView<const double> AlongMeters, int32 NodeIndex, double LimitMeters)
	{
		int32 WalkIndex = NodeIndex;
		while (WalkIndex > 0 && AlongMeters[WalkIndex] > LimitMeters)
		{
			WalkIndex -= 1;
		}
		return WalkIndex;
	}

	int32 WalkForwardToWindow(TArrayView<const double> AlongMeters, int32 NodeIndex, double LimitMeters)
	{
		int32 WalkIndex = NodeIndex;
		while (WalkIndex + 1 < AlongMeters.Num() && AlongMeters[WalkIndex] < LimitMeters)
		{
			WalkIndex += 1;
		}
		return WalkIndex;
	}

	/** Drop per metre travelled, over the window around one span. Positive going downhill. */
	double MeasureGradient(
		const FMassifRiverReach& Reach, TArrayView<const double> AlongMeters, int32 SpanIndex)
	{
		const double CentreMeters = 0.5 * (AlongMeters[SpanIndex] + AlongMeters[SpanIndex + 1]);
		const int32 FirstIndex = WalkBackToWindow(AlongMeters, SpanIndex, CentreMeters - 0.5 * GradientWindowMeters);
		const int32 LastIndex =
			WalkForwardToWindow(AlongMeters, SpanIndex + 1, CentreMeters + 0.5 * GradientWindowMeters);

		const double RunMeters = AlongMeters[LastIndex] - AlongMeters[FirstIndex];
		if (RunMeters <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return 0.0;
		}

		const double DropMeters =
			Reach.Nodes[FirstIndex].WaterSurfaceMeters - Reach.Nodes[LastIndex].WaterSurfaceMeters;
		return DropMeters / RunMeters;
	}
}

void FMassifRiverField::AppendReachSegments(const FMassifRiverReach& Reach)
{
	if (Reach.Nodes.Num() < 2)
	{
		return;
	}

	TArray<double> AlongMeters;
	MeasureReachArclength(Reach, AlongMeters);
	for (int32 SpanIndex = 0; SpanIndex + 1 < Reach.Nodes.Num(); ++SpanIndex)
	{
		const FMassifRiverNode& Start = Reach.Nodes[SpanIndex];
		const FMassifRiverNode& End = Reach.Nodes[SpanIndex + 1];

		FSegment Segment;
		Segment.StartMeters = Start.PositionMeters;
		Segment.EndMeters = End.PositionMeters;
		Segment.LengthMeters = AlongMeters[SpanIndex + 1] - AlongMeters[SpanIndex];
		Segment.StartAlongMeters = AlongMeters[SpanIndex];
		Segment.StartWaterSurfaceMeters = Start.WaterSurfaceMeters;
		Segment.EndWaterSurfaceMeters = End.WaterSurfaceMeters;
		Segment.StartBedAltitudeMeters = Start.BedAltitudeMeters;
		Segment.EndBedAltitudeMeters = End.BedAltitudeMeters;
		Segment.StartHalfWidthMeters = 0.5 * Start.WidthMeters;
		Segment.EndHalfWidthMeters = 0.5 * End.WidthMeters;
		Segment.StartDepthMeters = Start.DepthMeters;
		Segment.EndDepthMeters = End.DepthMeters;
		Segment.StartSpeedMetersPerSecond = Start.SpeedMetersPerSecond;
		Segment.EndSpeedMetersPerSecond = End.SpeedMetersPerSecond;
		Segment.GradientAlongFlow = MeasureGradient(Reach, AlongMeters, SpanIndex);
		Segments.Add(Segment);
	}
}

/** The box a segment can answer inside: its own extent, grown by half its width and the bank reach. */
void FMassifRiverField::GetSegmentBox(
	const FSegment& Segment, FVector2d& OutMinMeters, FVector2d& OutMaxMeters) const
{
	const double InfluenceMeters =
		FMath::Max(Segment.StartHalfWidthMeters, Segment.EndHalfWidthMeters) + BankReachMeters;
	OutMinMeters.X = FMath::Min(Segment.StartMeters.X, Segment.EndMeters.X) - InfluenceMeters;
	OutMinMeters.Y = FMath::Min(Segment.StartMeters.Y, Segment.EndMeters.Y) - InfluenceMeters;
	OutMaxMeters.X = FMath::Max(Segment.StartMeters.X, Segment.EndMeters.X) + InfluenceMeters;
	OutMaxMeters.Y = FMath::Max(Segment.StartMeters.Y, Segment.EndMeters.Y) + InfluenceMeters;
}

void FMassifRiverField::MeasureGrid()
{
	CellSizeMeters = MinCellSizeMeters;
	if (Segments.IsEmpty())
	{
		return;
	}

	FVector2d MinMeters(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
	FVector2d MaxMeters(-TNumericLimits<double>::Max(), -TNumericLimits<double>::Max());
	for (const FSegment& Segment : Segments)
	{
		FVector2d SegmentMinMeters = FVector2d::ZeroVector;
		FVector2d SegmentMaxMeters = FVector2d::ZeroVector;
		GetSegmentBox(Segment, SegmentMinMeters, SegmentMaxMeters);
		MaxInfluenceMeters = FMath::Max(
			MaxInfluenceMeters,
			FMath::Max(Segment.StartHalfWidthMeters, Segment.EndHalfWidthMeters) + BankReachMeters);
		MinMeters = FVector2d(
			FMath::Min(MinMeters.X, SegmentMinMeters.X), FMath::Min(MinMeters.Y, SegmentMinMeters.Y));
		MaxMeters = FVector2d(
			FMath::Max(MaxMeters.X, SegmentMaxMeters.X), FMath::Max(MaxMeters.Y, SegmentMaxMeters.Y));
	}

	// Cells wider than the widest influence, so a channel spans few of them and a
	// query still reads only the one cell the point falls in.
	CellSizeMeters = FMath::Clamp(2.0 * MaxInfluenceMeters, MinCellSizeMeters, MaxCellSizeMeters);
	GridOriginMeters = MinMeters;
	Width = FMath::CeilToInt32((MaxMeters.X - MinMeters.X) / CellSizeMeters) + 1;
	Height = FMath::CeilToInt32((MaxMeters.Y - MinMeters.Y) / CellSizeMeters) + 1;
}

int32 FMassifRiverField::ColumnAt(double PositionXMeters) const
{
	return FMath::FloorToInt32((PositionXMeters - GridOriginMeters.X) / CellSizeMeters);
}

int32 FMassifRiverField::RowAt(double PositionYMeters) const
{
	return FMath::FloorToInt32((PositionYMeters - GridOriginMeters.Y) / CellSizeMeters);
}

/** Every cell of the grid the segment can answer in, clamped to the grid's own bounds. */
TArray<int32> FMassifRiverField::GetSegmentCells(const FSegment& Segment) const
{
	FVector2d MinMeters = FVector2d::ZeroVector;
	FVector2d MaxMeters = FVector2d::ZeroVector;
	GetSegmentBox(Segment, MinMeters, MaxMeters);

	const int32 MinColumn = FMath::Clamp(ColumnAt(MinMeters.X), 0, Width - 1);
	const int32 MinRow = FMath::Clamp(RowAt(MinMeters.Y), 0, Height - 1);
	const int32 MaxColumn = FMath::Clamp(ColumnAt(MaxMeters.X), 0, Width - 1);
	const int32 MaxRow = FMath::Clamp(RowAt(MaxMeters.Y), 0, Height - 1);

	TArray<int32> Cells;
	Cells.Reserve((MaxColumn - MinColumn + 1) * (MaxRow - MinRow + 1));
	for (int32 Row = MinRow; Row <= MaxRow; ++Row)
	{
		AppendCellRow(Row, MinColumn, MaxColumn, Cells);
	}
	return Cells;
}

void FMassifRiverField::AppendCellRow(
	int32 Row, int32 MinColumn, int32 MaxColumn, TArray<int32>& OutCells) const
{
	for (int32 Column = MinColumn; Column <= MaxColumn; ++Column)
	{
		OutCells.Add(Row * Width + Column);
	}
}

void FMassifRiverField::BuildCellOffsets()
{
	const int32 CellCount = Width * Height;
	TArray<int32> CellCounts;
	CellCounts.Init(0, CellCount);
	for (const FSegment& Segment : Segments)
	{
		IncrementCellCounts(Segment, CellCounts);
	}

	CellSegmentStart.SetNumUninitialized(CellCount + 1);
	CellSegmentStart[0] = 0;
	for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
	{
		CellSegmentStart[CellIndex + 1] = CellSegmentStart[CellIndex] + CellCounts[CellIndex];
	}
}

void FMassifRiverField::IncrementCellCounts(const FSegment& Segment, TArray<int32>& InOutCellCounts) const
{
	for (int32 CellIndex : GetSegmentCells(Segment))
	{
		InOutCellCounts[CellIndex] += 1;
	}
}

void FMassifRiverField::FillCellSegments()
{
	TArray<int32> Cursor = CellSegmentStart;
	CellSegments.SetNumUninitialized(CellSegmentStart.Last());
	for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
	{
		PlaceSegmentInCells(SegmentIndex, Cursor);
	}
}

void FMassifRiverField::PlaceSegmentInCells(int32 SegmentIndex, TArray<int32>& InOutCursor)
{
	for (int32 CellIndex : GetSegmentCells(Segments[SegmentIndex]))
	{
		CellSegments[InOutCursor[CellIndex]++] = SegmentIndex;
	}
}

/**
 * A segment is registered in every cell its own influence covers, so a point
 * whose nearest channel can reach it always finds that channel listed in its own
 * cell and a query never has to look at a neighbouring one.
 */
void FMassifRiverField::IndexSegments()
{
	if (Segments.IsEmpty() || Width <= 0 || Height <= 0)
	{
		return;
	}

	BuildCellOffsets();
	FillCellSegments();
}

// ---------------------------------------------------------------------------
// Querying it
// ---------------------------------------------------------------------------

void FMassifRiverField::ConsiderSegment(
	int32 SegmentIndex, FVector2d PositionMeters, FNearestSegment& InOutNearest) const
{
	const FSegment& Segment = Segments[SegmentIndex];
	const FVector2d Along = Segment.EndMeters - Segment.StartMeters;
	const double LengthSquared = Along.SquaredLength();

	double Fraction = 0.0;
	if (LengthSquared > UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		Fraction = FMath::Clamp((PositionMeters - Segment.StartMeters).Dot(Along) / LengthSquared, 0.0, 1.0);
	}

	const double DistanceMeters = (PositionMeters - (Segment.StartMeters + Fraction * Along)).Length();
	const double HalfWidthMeters =
		FMath::Lerp(Segment.StartHalfWidthMeters, Segment.EndHalfWidthMeters, Fraction);
	const double SignedDistanceMeters = DistanceMeters - HalfWidthMeters;

	// Measured from the bank rather than from the centreline, so the trunk keeps
	// the surface where a tributary happens to run alongside it. Strictly less, so
	// a tie keeps the lower segment index and the answer does not depend on the
	// order the cell was filled in.
	if (SignedDistanceMeters >= InOutNearest.SignedDistanceMeters)
	{
		return;
	}

	InOutNearest.SegmentIndex = SegmentIndex;
	InOutNearest.Fraction = Fraction;
	InOutNearest.DistanceMeters = DistanceMeters;
	InOutNearest.SignedDistanceMeters = SignedDistanceMeters;
}

FMassifRiverField::FNearestSegment FMassifRiverField::FindNearestSegment(FVector2d PositionMeters) const
{
	FNearestSegment Nearest;
	if (Segments.IsEmpty())
	{
		return Nearest;
	}

	const int32 Column = ColumnAt(PositionMeters.X);
	const int32 Row = RowAt(PositionMeters.Y);
	if (!Contains(Column, Row))
	{
		return Nearest;
	}

	const int32 CellIndex = Row * Width + Column;
	for (int32 Slot = CellSegmentStart[CellIndex]; Slot < CellSegmentStart[CellIndex + 1]; ++Slot)
	{
		ConsiderSegment(CellSegments[Slot], PositionMeters, Nearest);
	}
	return Nearest;
}

FMassifRiverSample FMassifRiverField::MakeSample(
	const FNearestSegment& Nearest, FVector2d PositionMeters) const
{
	const FSegment& Segment = Segments[Nearest.SegmentIndex];
	const double Fraction = Nearest.Fraction;

	FMassifRiverSample Sample;
	Sample.SignedDistanceMeters = Nearest.SignedDistanceMeters;
	Sample.WaterSurfaceMeters =
		FMath::Lerp(Segment.StartWaterSurfaceMeters, Segment.EndWaterSurfaceMeters, Fraction);
	Sample.BedAltitudeMeters =
		FMath::Lerp(Segment.StartBedAltitudeMeters, Segment.EndBedAltitudeMeters, Fraction);
	Sample.WidthMeters =
		2.0 * FMath::Lerp(Segment.StartHalfWidthMeters, Segment.EndHalfWidthMeters, Fraction);
	Sample.DepthMeters = FMath::Lerp(Segment.StartDepthMeters, Segment.EndDepthMeters, Fraction);
	Sample.SpeedMetersPerSecond =
		FMath::Lerp(Segment.StartSpeedMetersPerSecond, Segment.EndSpeedMetersPerSecond, Fraction);
	Sample.FlowDirection = (Segment.EndMeters - Segment.StartMeters).GetSafeNormal();
	if (Sample.FlowDirection.IsNearlyZero())
	{
		// Two nodes on top of each other: any direction will do, and east is one.
		Sample.FlowDirection = FVector2d(1.0, 0.0);
	}
	Sample.AlongMeters = Segment.StartAlongMeters + Fraction * Segment.LengthMeters;
	Sample.GradientAlongFlow = Segment.GradientAlongFlow;

	const FVector2d CentreMeters = FMath::Lerp(Segment.StartMeters, Segment.EndMeters, Fraction);
	const double HalfWidthMeters = FMath::Max(0.5 * Sample.WidthMeters, UE_DOUBLE_SMALL_NUMBER);
	const double OffsetMeters = (PositionMeters - CentreMeters).Dot(AcrossFlow(Sample.FlowDirection));
	Sample.AcrossFraction = FMath::Clamp(OffsetMeters / HalfWidthMeters, -1.0, 1.0);
	return Sample;
}

FMassifRiverSample FMassifRiverField::Sample(FVector2d PositionMeters) const
{
	const FNearestSegment Nearest = FindNearestSegment(PositionMeters);
	if (Nearest.SegmentIndex == INDEX_NONE || Nearest.SignedDistanceMeters > BankReachMeters)
	{
		return FMassifRiverSample();
	}
	return MakeSample(Nearest, PositionMeters);
}

bool FMassifRiverField::IntersectsBox(FVector2d MinMeters, FVector2d MaxMeters) const
{
	if (Segments.IsEmpty())
	{
		return false;
	}

	const int32 MinColumn = FMath::Max(ColumnAt(MinMeters.X), 0);
	const int32 MinRow = FMath::Max(RowAt(MinMeters.Y), 0);
	const int32 MaxColumn = FMath::Min(ColumnAt(MaxMeters.X), Width - 1);
	const int32 MaxRow = FMath::Min(RowAt(MaxMeters.Y), Height - 1);
	for (int32 Row = MinRow; Row <= MaxRow; ++Row)
	{
		if (RowHasSegments(Row, MinColumn, MaxColumn))
		{
			return true;
		}
	}
	return false;
}

bool FMassifRiverField::RowHasSegments(int32 Row, int32 MinColumn, int32 MaxColumn) const
{
	for (int32 Column = MinColumn; Column <= MaxColumn; ++Column)
	{
		const int32 CellIndex = Row * Width + Column;
		if (CellSegmentStart[CellIndex] != CellSegmentStart[CellIndex + 1])
		{
			return true;
		}
	}
	return false;
}
