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

	/**
	 * How far past the waterline the section is spread before it is stored.
	 *
	 * The bed a station solved meets the water surface exactly at the wetted edge,
	 * and the water mesh ends on that same edge — so left alone the two surfaces
	 * are coincident along every bank in the world and fight each other there.
	 * Spreading the section a few per cent wider puts the offset the waterline
	 * reads a little further in, where there is still a few centimetres of column,
	 * and gives the shoreline something to fade over.
	 */
	constexpr double ShoreOvershoot = 1.06;

	/** The trench a section of thirty-three samples holds, as a share of its depth midstream. */
	double ParabolicShare(int32 OffsetIndex)
	{
		const double AcrossFraction = 2.0 * OffsetIndex / (MassifChannelProfileOffsetCount - 1) - 1.0;
		return FMath::Max(1.0 - AcrossFraction * AcrossFraction, 0.0);
	}

	/**
	 * How far across its own section a point stands, from the offset to the nearest
	 * point of the centreline.
	 *
	 * The whole offset rather than the part of it across the flow. Mid-span the two
	 * are the same thing; past either end they are not, and taking only the part
	 * across the flow runs a station's trench on beyond the reach it belongs to at
	 * full depth. Measured this way a reach closes its own trench where it ends.
	 */
	double SignedOffsetMeters(FVector2d OffsetMeters, FVector2d Rightward)
	{
		const double LengthMeters = OffsetMeters.Length();
		if (OffsetMeters.Dot(Rightward) < 0.0)
		{
			return -LengthMeters;
		}
		return LengthMeters;
	}
}

double FMassifRiverField::FSectionCut::DepthAt(double AcrossMeters) const
{
	if (HalfSpanMeters <= 0.0f || FMath::Abs(AcrossMeters) >= HalfSpanMeters)
	{
		return 0.0;
	}

	const double SampleIndex = 0.5 * (AcrossMeters / HalfSpanMeters + 1.0)
		* (MassifChannelProfileOffsetCount - 1);
	const int32 LowIndex =
		FMath::Clamp(FMath::FloorToInt32(SampleIndex), 0, MassifChannelProfileOffsetCount - 2);
	return FMath::Lerp(
		static_cast<double>(DepthMeters[LowIndex]),
		static_cast<double>(DepthMeters[LowIndex + 1]),
		SampleIndex - LowIndex);
}

// ---------------------------------------------------------------------------
// Building the index
// ---------------------------------------------------------------------------

TSharedRef<const FMassifRiverField> FMassifRiverField::Build(
	const FMassifRiverNetwork& InNetwork, double InBankReachMeters)
{
	// Every segment is copied out of the network, so the field outlives it and a
	// caller never has to keep the two alive together.
	TSharedRef<FMassifRiverField> Field(new FMassifRiverField());
	Field->BankReachMeters = FMath::Max(InBankReachMeters, 0.0);
	Field->CollectSegments(InNetwork);
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

void FMassifRiverField::FSectionCut::SetFromProfile(const FMassifChannelProfile& Profile)
{
	HalfSpanMeters = static_cast<float>(Profile.HalfSpanMeters * ShoreOvershoot);
	for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
	{
		const double CutMeters = static_cast<double>(Profile.GroundAltitudeMeters[OffsetIndex])
			- Profile.BedAltitudeMeters[OffsetIndex];
		DepthMeters[OffsetIndex] = static_cast<float>(FMath::Max(CutMeters, 0.0));
	}
}

void FMassifRiverField::FSectionCut::SetFromChannel(double WidthMeters, double CutMeters)
{
	HalfSpanMeters = static_cast<float>(0.5 * FMath::Max(WidthMeters, 0.0) * ShoreOvershoot);
	for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
	{
		DepthMeters[OffsetIndex] =
			static_cast<float>(FMath::Max(CutMeters, 0.0) * ParabolicShare(OffsetIndex));
	}
}

void FMassifRiverField::AppendStation(const FMassifRiverNode& Node)
{
	FStation Station;
	Station.Annual.WaterSurfaceMeters = Node.WaterSurfaceMeters;
	Station.Annual.HalfWidthMeters = 0.5 * Node.WidthMeters;
	Station.Annual.DepthMeters = Node.DepthMeters;
	Station.Annual.SpeedMetersPerSecond = Node.SpeedMetersPerSecond;
	Station.BedAltitudeMeters = Node.BedAltitudeMeters;
	Station.Season = Node.Season;

	if (Node.Profile.HalfSpanMeters > 0.0)
	{
		Station.Cut.SetFromProfile(Node.Profile);
		Stations.Add(Station);
		return;
	}

	Station.Cut.SetFromChannel(Node.WidthMeters, Node.GroundAltitudeMeters - Node.BedAltitudeMeters);
	Stations.Add(Station);
}

/**
 * A slice outside the year, or a station whose year was never solved, falls back
 * to the annual answer. A flat year is a year; no year at all is a channel with
 * nothing in it.
 */
FMassifRiverField::FStationWater FMassifRiverField::StationWaterAt(
	int32 StationIndex, int32 TimeSlice) const
{
	const FStation& Station = Stations[StationIndex];
	if (TimeSlice < 0 || TimeSlice >= MassifSeasonalSliceCount
		|| Station.Season.HalfWidthMeters[TimeSlice] <= 0.0f)
	{
		return Station.Annual;
	}

	FStationWater Water;
	Water.WaterSurfaceMeters = Station.Season.WaterSurfaceMeters[TimeSlice];
	Water.HalfWidthMeters = Station.Season.HalfWidthMeters[TimeSlice];
	Water.DepthMeters = Station.Season.DepthMeters[TimeSlice];
	Water.SpeedMetersPerSecond = Station.Season.SpeedMetersPerSecond[TimeSlice];
	return Water;
}

FMassifRiverField::FStationWater FMassifRiverField::WaterOnSegment(
	const FSegment& Segment, double Fraction, int32 TimeSlice) const
{
	const FStationWater StartWater = StationWaterAt(Segment.StartStationIndex, TimeSlice);
	const FStationWater EndWater = StationWaterAt(Segment.EndStationIndex, TimeSlice);

	FStationWater Water;
	Water.WaterSurfaceMeters =
		FMath::Lerp(StartWater.WaterSurfaceMeters, EndWater.WaterSurfaceMeters, Fraction);
	Water.HalfWidthMeters = FMath::Lerp(StartWater.HalfWidthMeters, EndWater.HalfWidthMeters, Fraction);
	Water.DepthMeters = FMath::Lerp(StartWater.DepthMeters, EndWater.DepthMeters, Fraction);
	Water.SpeedMetersPerSecond =
		FMath::Lerp(StartWater.SpeedMetersPerSecond, EndWater.SpeedMetersPerSecond, Fraction);
	return Water;
}

FMassifRiverField::FSegment FMassifRiverField::MakeSpanSegment(
	const FMassifRiverReach& Reach, TArrayView<const double> AlongMeters, int32 SpanIndex,
	int32 FirstStationIndex) const
{
	FSegment Segment;
	Segment.StartMeters = Reach.Nodes[SpanIndex].PositionMeters;
	Segment.EndMeters = Reach.Nodes[SpanIndex + 1].PositionMeters;
	Segment.LengthMeters = AlongMeters[SpanIndex + 1] - AlongMeters[SpanIndex];
	Segment.StartAlongMeters = AlongMeters[SpanIndex];
	Segment.StartStationIndex = FirstStationIndex + SpanIndex;
	Segment.EndStationIndex = FirstStationIndex + SpanIndex + 1;
	Segment.GradientAlongFlow = MeasureGradient(Reach, AlongMeters, SpanIndex);

	// Far enough out that a point the segment can answer for always finds it in
	// its own cell: the bank the field reports over, or the section the carve
	// cut, whichever reaches further. The annual width, because it is the widest
	// the year gets and the index has to cover every slice of it.
	const FStation& StartStation = Stations[Segment.StartStationIndex];
	const FStation& EndStation = Stations[Segment.EndStationIndex];
	Segment.InfluenceMeters = FMath::Max(
		FMath::Max(StartStation.Annual.HalfWidthMeters, EndStation.Annual.HalfWidthMeters)
			+ BankReachMeters,
		FMath::Max(
			static_cast<double>(StartStation.Cut.HalfSpanMeters),
			static_cast<double>(EndStation.Cut.HalfSpanMeters)));
	return Segment;
}

void FMassifRiverField::AppendReachSegments(const FMassifRiverReach& Reach)
{
	if (Reach.Nodes.Num() < 2)
	{
		return;
	}

	TArray<double> AlongMeters;
	MeasureReachArclength(Reach, AlongMeters);

	// Every station first, so both segments meeting at one share its section and
	// its year rather than each carrying a copy.
	const int32 FirstStationIndex = Stations.Num();
	for (const FMassifRiverNode& Node : Reach.Nodes)
	{
		AppendStation(Node);
	}

	for (int32 SpanIndex = 0; SpanIndex + 1 < Reach.Nodes.Num(); ++SpanIndex)
	{
		Segments.Add(MakeSpanSegment(Reach, AlongMeters, SpanIndex, FirstStationIndex));
	}
}

/** The box a segment can answer inside: its own extent, grown by everything it reaches. */
void FMassifRiverField::GetSegmentBox(
	const FSegment& Segment, FVector2d& OutMinMeters, FVector2d& OutMaxMeters) const
{
	const double InfluenceMeters = Segment.InfluenceMeters;
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
		MaxInfluenceMeters = FMath::Max(MaxInfluenceMeters, Segment.InfluenceMeters);
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
	int32 SegmentIndex, FVector2d PositionMeters, int32 TimeSlice,
	FNearestSegment& InOutNearest) const
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
	const double SignedDistanceMeters =
		DistanceMeters - WaterOnSegment(Segment, Fraction, TimeSlice).HalfWidthMeters;

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

double FMassifRiverField::CutDepthOfSegment(int32 SegmentIndex, FVector2d PositionMeters) const
{
	const FSegment& Segment = Segments[SegmentIndex];
	const FVector2d Along = Segment.EndMeters - Segment.StartMeters;
	const double LengthSquared = Along.SquaredLength();
	if (LengthSquared <= UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return 0.0;
	}

	const double Fraction =
		FMath::Clamp((PositionMeters - Segment.StartMeters).Dot(Along) / LengthSquared, 0.0, 1.0);
	const double AcrossMeters = SignedOffsetMeters(
		PositionMeters - (Segment.StartMeters + Fraction * Along), AcrossFlow(Along.GetSafeNormal()));

	return FMath::Lerp(
		Stations[Segment.StartStationIndex].Cut.DepthAt(AcrossMeters),
		Stations[Segment.EndStationIndex].Cut.DepthAt(AcrossMeters),
		Fraction);
}

double FMassifRiverField::SampleCutDepthMeters(FVector2d PositionMeters) const
{
	if (Segments.IsEmpty())
	{
		return 0.0;
	}

	const int32 Column = ColumnAt(PositionMeters.X);
	const int32 Row = RowAt(PositionMeters.Y);
	if (!Contains(Column, Row))
	{
		return 0.0;
	}

	const int32 CellIndex = Row * Width + Column;
	double DeepestMeters = 0.0;
	for (int32 Slot = CellSegmentStart[CellIndex]; Slot < CellSegmentStart[CellIndex + 1]; ++Slot)
	{
		DeepestMeters =
			FMath::Max(DeepestMeters, CutDepthOfSegment(CellSegments[Slot], PositionMeters));
	}
	return DeepestMeters;
}

FMassifRiverField::FNearestSegment FMassifRiverField::FindNearestSegment(
	FVector2d PositionMeters, int32 TimeSlice) const
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
		ConsiderSegment(CellSegments[Slot], PositionMeters, TimeSlice, Nearest);
	}
	return Nearest;
}

FMassifRiverSample FMassifRiverField::MakeSample(
	const FNearestSegment& Nearest, FVector2d PositionMeters, int32 TimeSlice) const
{
	const FSegment& Segment = Segments[Nearest.SegmentIndex];
	const double Fraction = Nearest.Fraction;
	const FStationWater Water = WaterOnSegment(Segment, Fraction, TimeSlice);

	FMassifRiverSample Sample;
	Sample.SignedDistanceMeters = Nearest.SignedDistanceMeters;
	Sample.WaterSurfaceMeters = Water.WaterSurfaceMeters;
	Sample.BedAltitudeMeters = FMath::Lerp(
		Stations[Segment.StartStationIndex].BedAltitudeMeters,
		Stations[Segment.EndStationIndex].BedAltitudeMeters,
		Fraction);
	Sample.WidthMeters = 2.0 * Water.HalfWidthMeters;
	Sample.DepthMeters = Water.DepthMeters;
	Sample.SpeedMetersPerSecond = Water.SpeedMetersPerSecond;
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
	Sample.CutDepthMeters = SampleCutDepthMeters(PositionMeters);
	return Sample;
}

FMassifRiverSample FMassifRiverField::Sample(FVector2d PositionMeters, int32 TimeSlice) const
{
	const FNearestSegment Nearest = FindNearestSegment(PositionMeters, TimeSlice);
	if (Nearest.SegmentIndex == INDEX_NONE || Nearest.SignedDistanceMeters > BankReachMeters)
	{
		return FMassifRiverSample();
	}
	return MakeSample(Nearest, PositionMeters, TimeSlice);
}

double FMassifRiverField::SampleSignedDistanceMeters(FVector2d PositionMeters, int32 TimeSlice) const
{
	const FNearestSegment Nearest = FindNearestSegment(PositionMeters, TimeSlice);
	if (Nearest.SegmentIndex == INDEX_NONE || Nearest.SignedDistanceMeters > BankReachMeters)
	{
		return FMassifRiverSample().SignedDistanceMeters;
	}
	return Nearest.SignedDistanceMeters;
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
