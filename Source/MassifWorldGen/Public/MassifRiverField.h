#pragma once

#include "CoreMinimal.h"

class FMassifRiverNetwork;
struct FMassifRiverReach;

/** What the river network says about one point of ground. */
struct FMassifRiverSample
{
	/**
	 * Metres out from the wetted edge of the nearest channel: negative inside the
	 * water, zero at the bank, positive on dry ground. Everything else here is
	 * only meaningful within the field's bank reach of a channel.
	 */
	double SignedDistanceMeters = TNumericLimits<double>::Max();

	/** Where the water's top sits over this point. */
	double WaterSurfaceMeters = 0.0;

	/** The channel invert: what the terrain is cut down to. Always below the surface. */
	double BedAltitudeMeters = 0.0;

	double WidthMeters = 0.0;
	double DepthMeters = 0.0;
	double SpeedMetersPerSecond = 0.0;

	/** Unit vector pointing downstream at the nearest point on the centreline. */
	FVector2d FlowDirection = FVector2d(1.0, 0.0);

	/** Distance already run from the head of the reach, which a scrolling surface travels along. */
	double AlongMeters = 0.0;

	/** Where across the channel the point sits: -1 left bank, 0 midstream, 1 right bank. */
	double AcrossFraction = 0.0;

	/** Drop per metre travelled downstream, measured over a fixed window rather than one segment. */
	double GradientAlongFlow = 0.0;

	bool IsInChannel() const
	{
		return SignedDistanceMeters <= 0.0;
	}
};

/**
 * The river network as a function of world position:
 *
 *     Sample(Position) → the channel nearest that point, and where the point is in it
 *
 * One answer, read by everything: the terrain field cuts the ground to the bed
 * it reports, the water surface is meshed from it, the habitat learns that a
 * channel is there, and the semantic masks keep vegetation out of it. Those
 * four agree by construction rather than by four thresholds being tuned to
 * match.
 *
 * Backed by a uniform grid over the reach segments. A segment is registered in
 * every cell its own reach could affect, so a query reads one cell and the
 * segments in it and nothing else — the cost does not grow with the size of the
 * network.
 *
 * Where two channels overlap, the one whose bank is furthest away wins: a
 * tributary running close to the trunk does not take the surface off it. That
 * is what makes the water surface single valued at a confluence, and single
 * valued is what stops it being drawn as two sheets in the same place.
 *
 * Never serialized: a pure function of the network, rebuilt with it at load.
 *
 * Thread-safe once built: sampling touches only immutable state.
 */
class MASSIFWORLDGEN_API FMassifRiverField
{
public:
	/**
	 * BankReachMeters is how far beyond the water the field still answers: the
	 * band the terrain rises back to its measured height over, and the ground a
	 * riparian species may stand on.
	 */
	static TSharedRef<const FMassifRiverField> Build(
		const FMassifRiverNetwork& InNetwork, double InBankReachMeters);

	FMassifRiverField(const FMassifRiverField&) = delete;
	FMassifRiverField& operator=(const FMassifRiverField&) = delete;

	FMassifRiverSample Sample(FVector2d PositionMeters) const;

	/**
	 * Whether any channel could reach into the box. Conservative — it may say yes
	 * where a query would find nothing — so it is a rejection test, which is what
	 * keeps a tile with no river in it from being meshed or rasterised at all.
	 */
	bool IntersectsBox(FVector2d MinMeters, FVector2d MaxMeters) const;

	double GetBankReachMeters() const
	{
		return BankReachMeters;
	}

	int32 GetSegmentCount() const
	{
		return Segments.Num();
	}

	/** Widest a channel gets plus the bank reach: how far from a centreline the field can answer. */
	double GetMaxInfluenceMeters() const
	{
		return MaxInfluenceMeters;
	}

private:
	FMassifRiverField() = default;

	/** One span between two nodes, with both ends' attributes so a query interpolates without the network. */
	struct FSegment
	{
		FVector2d StartMeters = FVector2d::ZeroVector;
		FVector2d EndMeters = FVector2d::ZeroVector;
		double LengthMeters = 0.0;

		double StartAlongMeters = 0.0;

		double StartWaterSurfaceMeters = 0.0;
		double EndWaterSurfaceMeters = 0.0;
		double StartBedAltitudeMeters = 0.0;
		double EndBedAltitudeMeters = 0.0;
		double StartHalfWidthMeters = 0.0;
		double EndHalfWidthMeters = 0.0;
		double StartDepthMeters = 0.0;
		double EndDepthMeters = 0.0;
		double StartSpeedMetersPerSecond = 0.0;
		double EndSpeedMetersPerSecond = 0.0;

		/** Measured over a window along the reach, so one noisy span cannot make a waterfall. */
		double GradientAlongFlow = 0.0;
	};

	/** The nearest segment to a point, and where on it. */
	struct FNearestSegment
	{
		int32 SegmentIndex = INDEX_NONE;
		double Fraction = 0.0;
		double DistanceMeters = 0.0;
		double SignedDistanceMeters = TNumericLimits<double>::Max();
	};

	void CollectSegments(const FMassifRiverNetwork& Network);
	void AppendReachSegments(const FMassifRiverReach& Reach);
	void MeasureGrid();
	void IndexSegments();

	void GetSegmentBox(const FSegment& Segment, FVector2d& OutMinMeters, FVector2d& OutMaxMeters) const;
	TArray<int32> GetSegmentCells(const FSegment& Segment) const;
	void AppendCellRow(int32 Row, int32 MinColumn, int32 MaxColumn, TArray<int32>& OutCells) const;

	void BuildCellOffsets();
	void IncrementCellCounts(const FSegment& Segment, TArray<int32>& InOutCellCounts) const;
	void FillCellSegments();
	void PlaceSegmentInCells(int32 SegmentIndex, TArray<int32>& InOutCursor);

	FNearestSegment FindNearestSegment(FVector2d PositionMeters) const;
	void ConsiderSegment(int32 SegmentIndex, FVector2d PositionMeters, FNearestSegment& InOutNearest) const;
	FMassifRiverSample MakeSample(const FNearestSegment& Nearest, FVector2d PositionMeters) const;
	bool RowHasSegments(int32 Row, int32 MinColumn, int32 MaxColumn) const;

	bool Contains(int32 Column, int32 Row) const
	{
		return Column >= 0 && Column < Width && Row >= 0 && Row < Height;
	}

	int32 ColumnAt(double PositionXMeters) const;
	int32 RowAt(double PositionYMeters) const;

	TArray<FSegment> Segments;

	/** Compressed rows: the segments of cell N are CellSegments over [Start[N], Start[N + 1]). */
	TArray<int32> CellSegmentStart;
	TArray<int32> CellSegments;

	FVector2d GridOriginMeters = FVector2d::ZeroVector;
	double CellSizeMeters = 0.0;
	double BankReachMeters = 0.0;
	double MaxInfluenceMeters = 0.0;
	int32 Width = 0;
	int32 Height = 0;
};
