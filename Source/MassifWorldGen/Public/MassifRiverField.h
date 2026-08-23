#pragma once

#include "CoreMinimal.h"
#include "MassifChannelFlow.h"
#include "MassifChannelProfile.h"

class FMassifRiverNetwork;
struct FMassifRiverNode;
struct FMassifRiverReach;

/**
 * How far beyond the water the field answers about a channel: the ground a
 * riparian species may stand on, and the band a semantic mask fades over.
 *
 * Nothing to do with the trench any more — that reaches as far as the station's
 * own cross-section does, and the field indexes itself to cover whichever of the
 * two is wider.
 */
inline constexpr double MassifDefaultBankReachMeters = 24.0;

/**
 * The annual answer rather than any half-month of the year: what the regime
 * does on average, which is what everything but the drawn water surface wants.
 */
inline constexpr int32 MassifAnnualTimeSlice = INDEX_NONE;

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

	/** The channel invert: the deepest point of the section. Always below the surface. */
	double BedAltitudeMeters = 0.0;

	/**
	 * How far the ground here is cut below the surface the elevation model
	 * measured: the trench the carve left, read across its own cross-section.
	 *
	 * A depth rather than an altitude, and that is the whole point. The carve
	 * works a section of thirty-three bed samples — a scour pool against one bank,
	 * a bar against the other, the slope the material stands at between them — and
	 * an altitude can only carry the deepest of them. Subtracted from whatever the
	 * ground is at a point, the section arrives on the terrain as the shape it was
	 * worked into rather than as a parabola fitted to one number of it.
	 *
	 * Never negative: cutting a bed is a subtraction, and a channel that silted up
	 * above its banks is not a dyke across the valley.
	 */
	double CutDepthMeters = 0.0;

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
 * The cut is the exception, and takes the deepest of them instead. Picking one
 * channel's bed steps the ground to the other's along the line the winner
 * changes on, and above a confluence two channels overlap for as far back as
 * their own widths reach. A maximum has no such line: the deeper of two
 * continuous surfaces is continuous, so two beds that agree nowhere still meet
 * without a step. Two channels close enough to share ground share one trench,
 * which is what they are.
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

	/**
	 * TimeSlice picks a half-month of the representative year the network was
	 * solved over, or the annual answer. Everything that is not drawing the water
	 * as it stands today wants the annual one: what a riparian species may stand
	 * on is a property of the regime, not of the day it is being asked about.
	 */
	FMassifRiverSample Sample(
		FVector2d PositionMeters, int32 TimeSlice = MassifAnnualTimeSlice) const;

	/**
	 * Only how far the point is from the nearest wetted edge, without working out
	 * the rest of the channel there. A mask or a mesh boundary sweeps a lattice
	 * asking nothing else, and most of what it asks about is dry ground.
	 */
	double SampleSignedDistanceMeters(
		FVector2d PositionMeters, int32 TimeSlice = MassifAnnualTimeSlice) const;

	/**
	 * Only the trench, without working out which channel cut it or what the water
	 * in it is doing. The terrain asks this and nothing else, once per vertex of
	 * every tile it builds, and it is the deepest cut of every channel reaching
	 * the point rather than the nearest one's.
	 *
	 * No slice: the bed is the work of every year the channel has seen and is the
	 * same in February as in June. What the year moves is the water in it.
	 */
	double SampleCutDepthMeters(FVector2d PositionMeters) const;

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

	/**
	 * One station's trench, as depth below the ground across its own section.
	 *
	 * Stored per node and referenced by the two segments that meet there, so a
	 * station's thirty-three samples are held once rather than once per span.
	 */
	struct FSectionCut
	{
		/** Half the span the samples cover. Zero where the station has no section. */
		float HalfSpanMeters = 0.0f;
		float DepthMeters[MassifChannelProfileOffsetCount] = {};

		/** Between the two samples the offset falls in, and nothing at all outside the span. */
		double DepthAt(double AcrossMeters) const;

		/** The ground a station's section was cut from, less the bed the carve left in it. */
		void SetFromProfile(const FMassifChannelProfile& Profile);

		/**
		 * A station with no section solved for it: the trench the discharge alone
		 * asks for, round rather than flat-bottomed. What a network assembled
		 * without the section solve falls back to.
		 */
		void SetFromChannel(double WidthMeters, double CutMeters);
	};

	/** What the water is doing at one station, in whichever slice was asked for. */
	struct FStationWater
	{
		double WaterSurfaceMeters = 0.0;
		double HalfWidthMeters = 0.0;
		double DepthMeters = 0.0;
		double SpeedMetersPerSecond = 0.0;
	};

	/**
	 * One station of one reach: the trench under it, the water the annual regime
	 * puts in it, and the same water half-month by half-month.
	 *
	 * Held per station and referenced by the two segments meeting there, rather
	 * than copied into both. A year of four figures and a section of thirty-three
	 * is most of what the field weighs.
	 */
	struct FStation
	{
		FStationWater Annual;

		/** The invert: the deepest point of the section, which the year does not move. */
		double BedAltitudeMeters = 0.0;

		FSectionCut Cut;
		FMassifStationSeason Season;
	};

	/** One span between two stations, and where it sits along its own reach. */
	struct FSegment
	{
		FVector2d StartMeters = FVector2d::ZeroVector;
		FVector2d EndMeters = FVector2d::ZeroVector;
		double LengthMeters = 0.0;

		double StartAlongMeters = 0.0;

		/** Both ends' stations, and how far from the centreline either of them reaches. */
		int32 StartStationIndex = INDEX_NONE;
		int32 EndStationIndex = INDEX_NONE;
		double InfluenceMeters = 0.0;

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
	void AppendStation(const FMassifRiverNode& Node);
	FSegment MakeSpanSegment(
		const FMassifRiverReach& Reach, TArrayView<const double> AlongMeters, int32 SpanIndex,
		int32 FirstStationIndex) const;

	/** One station's water in the slice asked for, or its annual answer. */
	FStationWater StationWaterAt(int32 StationIndex, int32 TimeSlice) const;

	/** The same, interpolated to a point along one span. */
	FStationWater WaterOnSegment(const FSegment& Segment, double Fraction, int32 TimeSlice) const;
	void MeasureGrid();
	void IndexSegments();

	/** The trench one segment leaves at a point, interpolated along it and across both its sections. */
	double CutDepthOfSegment(int32 SegmentIndex, FVector2d PositionMeters) const;

	void GetSegmentBox(const FSegment& Segment, FVector2d& OutMinMeters, FVector2d& OutMaxMeters) const;
	TArray<int32> GetSegmentCells(const FSegment& Segment) const;
	void AppendCellRow(int32 Row, int32 MinColumn, int32 MaxColumn, TArray<int32>& OutCells) const;

	void BuildCellOffsets();
	void IncrementCellCounts(const FSegment& Segment, TArray<int32>& InOutCellCounts) const;
	void FillCellSegments();
	void PlaceSegmentInCells(int32 SegmentIndex, TArray<int32>& InOutCursor);

	FNearestSegment FindNearestSegment(FVector2d PositionMeters, int32 TimeSlice) const;
	void ConsiderSegment(
		int32 SegmentIndex, FVector2d PositionMeters, int32 TimeSlice,
		FNearestSegment& InOutNearest) const;
	FMassifRiverSample MakeSample(
		const FNearestSegment& Nearest, FVector2d PositionMeters, int32 TimeSlice) const;
	bool RowHasSegments(int32 Row, int32 MinColumn, int32 MaxColumn) const;

	bool Contains(int32 Column, int32 Row) const
	{
		return Column >= 0 && Column < Width && Row >= 0 && Row < Height;
	}

	int32 ColumnAt(double PositionXMeters) const;
	int32 RowAt(double PositionYMeters) const;

	TArray<FSegment> Segments;
	TArray<FStation> Stations;

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
