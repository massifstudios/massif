#pragma once

#include "CoreMinimal.h"
#include "MassifChannelHydraulics.h"
#include "MassifChannelProfile.h"
#include "MassifElevationSource.h"
#include "MassifRiverNetwork.h"

/**
 * Closed-form channels and the reaches that run down them, shared by every test
 * of the channel solves.
 *
 * Shared rather than repeated in each test's own anonymous namespace: the module
 * builds as a unity, so two files declaring the same helper collide, and two
 * declaring nearly the same one silently disagree about what a test reach is.
 */
namespace MassifTestChannel
{
	/** Altitude of the bed at Y = 0, which every test reach's surface is set against. */
	inline constexpr double ReferenceAltitudeMeters = 1000.0;

	/**
	 * A prismatic trapezoidal channel running due south, so the section every
	 * station takes is the same one and the normal depth in it is a closed form.
	 *
	 * The channel is cut into a plane that falls south at a fixed slope, and the
	 * whole thing translates down that slope, so what the solve sees at any station
	 * is a trench of one shape at one gradient.
	 */
	class FTrapezoidChannelSource final : public IMassifElevationSource
	{
	public:
		FTrapezoidChannelSource(double InBaseHalfWidthMeters, double InSideSlope, double InBedSlope)
			: BaseHalfWidthMeters(InBaseHalfWidthMeters)
			, SideSlope(InSideSlope)
			, BedSlope(InBedSlope)
		{
		}

		double SampleAltitudeMeters(FVector2d PositionMeters, double SampleSpacingMeters) const override
		{
			const double BedMeters = ReferenceAltitudeMeters + BedSlope * PositionMeters.Y;
			const double AcrossMeters = FMath::Abs(PositionMeters.X);
			if (AcrossMeters <= BaseHalfWidthMeters)
			{
				return BedMeters;
			}
			return BedMeters + (AcrossMeters - BaseHalfWidthMeters) / SideSlope;
		}

		FVector2d GetWorldSizeMeters() const override
		{
			return FVector2d(4000.0, 4000.0);
		}

		double GetFinestSpacingMeters() const override
		{
			return 0.01;
		}

		double BaseHalfWidthMeters = 0.0;

		/** Metres across per metre up: 2 is a bank at about 27 degrees. */
		double SideSlope = 0.0;

		double BedSlope = 0.0;
	};

	/**
	 * The same trapezoid with its gradient changing at Y = 0, so a reach running
	 * south crosses from one slope to another halfway down.
	 *
	 * What the carve is about: the flow's capacity changes at the break and the bed
	 * has to answer for the difference, by dropping what it can no longer carry or
	 * by taking up what it now can.
	 */
	class FSlopeBreakChannelSource final : public IMassifElevationSource
	{
	public:
		FSlopeBreakChannelSource(
			double InBaseHalfWidthMeters, double InSideSlope, double InUpperSlope, double InLowerSlope)
			: BaseHalfWidthMeters(InBaseHalfWidthMeters)
			, SideSlope(InSideSlope)
			, UpperSlope(InUpperSlope)
			, LowerSlope(InLowerSlope)
		{
		}

		double SampleAltitudeMeters(FVector2d PositionMeters, double SampleSpacingMeters) const override
		{
			const double BedMeters = ReferenceAltitudeMeters + SlopeAt(PositionMeters.Y) * PositionMeters.Y;
			const double AcrossMeters = FMath::Abs(PositionMeters.X);
			if (AcrossMeters <= BaseHalfWidthMeters)
			{
				return BedMeters;
			}
			return BedMeters + (AcrossMeters - BaseHalfWidthMeters) / SideSlope;
		}

		FVector2d GetWorldSizeMeters() const override
		{
			return FVector2d(4000.0, 4000.0);
		}

		double GetFinestSpacingMeters() const override
		{
			return 0.01;
		}

		double BaseHalfWidthMeters = 0.0;
		double SideSlope = 0.0;

		/** Upstream is the north half, so the upper slope is the one at positive Y. */
		double UpperSlope = 0.0;
		double LowerSlope = 0.0;

	private:
		double SlopeAt(double NorthingMeters) const
		{
			if (NorthingMeters >= 0.0)
			{
				return UpperSlope;
			}
			return LowerSlope;
		}
	};

	/**
	 * A prismatic trapezoid at one gradient, pinched to a narrower base over a
	 * stretch about Y = 0.
	 *
	 * Water arriving faster than the pinch can pass has to stand higher upstream of
	 * it, which is backwater: a control that the reach above it knows nothing about
	 * from its own slope and section.
	 */
	class FConstrictedChannelSource final : public IMassifElevationSource
	{
	public:
		FConstrictedChannelSource(
			double InBaseHalfWidthMeters, double InPinchHalfWidthMeters, double InPinchHalfLengthMeters,
			double InSideSlope, double InBedSlope)
			: BaseHalfWidthMeters(InBaseHalfWidthMeters)
			, PinchHalfWidthMeters(InPinchHalfWidthMeters)
			, PinchHalfLengthMeters(InPinchHalfLengthMeters)
			, SideSlope(InSideSlope)
			, BedSlope(InBedSlope)
		{
		}

		double SampleAltitudeMeters(FVector2d PositionMeters, double SampleSpacingMeters) const override
		{
			const double BedMeters = ReferenceAltitudeMeters + BedSlope * PositionMeters.Y;
			const double HalfWidthMeters = HalfWidthAt(PositionMeters.Y);
			const double AcrossMeters = FMath::Abs(PositionMeters.X);
			if (AcrossMeters <= HalfWidthMeters)
			{
				return BedMeters;
			}
			return BedMeters + (AcrossMeters - HalfWidthMeters) / SideSlope;
		}

		FVector2d GetWorldSizeMeters() const override
		{
			return FVector2d(4000.0, 4000.0);
		}

		double GetFinestSpacingMeters() const override
		{
			return 0.01;
		}

		double BaseHalfWidthMeters = 0.0;
		double PinchHalfWidthMeters = 0.0;
		double PinchHalfLengthMeters = 0.0;
		double SideSlope = 0.0;
		double BedSlope = 0.0;

	private:
		double HalfWidthAt(double NorthingMeters) const
		{
			if (FMath::Abs(NorthingMeters) <= PinchHalfLengthMeters)
			{
				return PinchHalfWidthMeters;
			}
			return BaseHalfWidthMeters;
		}
	};

	/**
	 * Dead level ground at one altitude.
	 *
	 * A section across it is flat, so any lean the bed comes out with is the solve's
	 * doing and not the terrain's. This is also what a diffusion-generated map looks
	 * like to the carve: a surface with no channel anywhere in it.
	 */
	class FLevelGroundSource final : public IMassifElevationSource
	{
	public:
		double SampleAltitudeMeters(FVector2d PositionMeters, double SampleSpacingMeters) const override
		{
			return ReferenceAltitudeMeters;
		}

		FVector2d GetWorldSizeMeters() const override
		{
			return FVector2d(4000.0, 4000.0);
		}

		double GetFinestSpacingMeters() const override
		{
			return 0.01;
		}
	};

	/** What a test reach is set up with, before the section has anything to say about it. */
	struct FReachSpec
	{
		/**
		 * Where the measured water surface sits over the ground at each station.
		 *
		 * Along a real channel the elevation model's altitude is the top of the water,
		 * so the surface is measured and only the column under it is first solved for.
		 * A surface set at the ground therefore asks for the whole channel to be cut,
		 * and one set where the section already carries the flow asks for none of it.
		 */
		double SurfaceAboveGroundMeters = 0.0;

		/** The hydraulic-geometry depth, which bounds how far the first fit may cut. */
		double HydraulicDepthMeters = 0.0;

		double DischargeCubicMetersPerSecond = 0.0;

		/** The hydraulic-geometry width, which sets how wide a section is taken. */
		double WidthMeters = 0.0;

		/** Drop per metre of the surface where the ground has none of its own to give it one. */
		double SurfaceSlope = 0.0;
	};

	inline FMassifRiverNode MakeNodeAt(
		const IMassifElevationSource& Source, const FReachSpec& Spec, FVector2d PositionMeters,
		double AlongMeters)
	{
		FMassifRiverNode Node;
		Node.PositionMeters = PositionMeters;
		Node.GroundAltitudeMeters = Source.SampleAltitudeMeters(PositionMeters, 1.0);
		Node.WaterSurfaceMeters =
			Node.GroundAltitudeMeters + Spec.SurfaceAboveGroundMeters - Spec.SurfaceSlope * AlongMeters;
		Node.BedAltitudeMeters = Node.GroundAltitudeMeters;
		Node.DischargeCubicMetersPerSecond = Spec.DischargeCubicMetersPerSecond;
		Node.WidthMeters = Spec.WidthMeters;
		Node.DepthMeters = Spec.HydraulicDepthMeters;
		return Node;
	}

	/** A reach running due south down whatever the source is, station by station. */
	inline FMassifRiverReach MakeStraightReach(
		const IMassifElevationSource& Source, const FReachSpec& Spec, int32 NodeCount = 41,
		double SpacingMeters = 4.0)
	{
		FMassifRiverReach Reach;
		Reach.Nodes.Reserve(NodeCount);
		for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
		{
			// Upstream first: the reach starts north and runs south.
			const double NorthingMeters = 0.5 * (NodeCount - 1) * SpacingMeters - NodeIndex * SpacingMeters;
			Reach.Nodes.Add(
				MakeNodeAt(Source, Spec, FVector2d(0.0, NorthingMeters), NodeIndex * SpacingMeters));
		}
		return Reach;
	}

	/**
	 * A reach along a circular arc of the given radius, heading south at its head.
	 *
	 * A left turn curves towards positive X, which is the inside of the bend, so the
	 * outer bank is the one at the far end of every station's own section.
	 */
	inline FMassifRiverReach MakeArcReach(
		const IMassifElevationSource& Source, const FReachSpec& Spec, double RadiusMeters, bool bTurnsLeft,
		int32 NodeCount = 41, double SpacingMeters = 4.0)
	{
		double TurnSign = -1.0;
		if (bTurnsLeft)
		{
			TurnSign = 1.0;
		}

		FMassifRiverReach Reach;
		Reach.Nodes.Reserve(NodeCount);
		for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
		{
			const double AlongMeters = NodeIndex * SpacingMeters;
			const double AngleRadians = PI + AlongMeters / RadiusMeters;
			const FVector2d PositionMeters(
				TurnSign * RadiusMeters * (1.0 + FMath::Cos(AngleRadians)),
				RadiusMeters * FMath::Sin(AngleRadians));
			Reach.Nodes.Add(MakeNodeAt(Source, Spec, PositionMeters, AlongMeters));
		}
		return Reach;
	}

	/** Manning's discharge through a trapezoid filled to a given depth: what a solve has to reproduce. */
	inline double TrapezoidDischarge(
		double BaseHalfWidthMeters, double SideSlope, double DepthMeters, double Roughness, double BedSlope)
	{
		const double BaseWidthMeters = 2.0 * BaseHalfWidthMeters;
		const double AreaSquareMeters = DepthMeters * (BaseWidthMeters + SideSlope * DepthMeters);
		const double PerimeterMeters =
			BaseWidthMeters + 2.0 * DepthMeters * FMath::Sqrt(1.0 + SideSlope * SideSlope);
		const double HydraulicRadiusMeters = AreaSquareMeters / PerimeterMeters;
		return AreaSquareMeters * FMath::Pow(HydraulicRadiusMeters, 2.0 / 3.0) * FMath::Sqrt(BedSlope)
			/ Roughness;
	}

	/** The station in the middle of a reach, clear of the ends a slope window is truncated at. */
	inline const FMassifChannelProfile& MiddleProfile(const FMassifRiverReach& Reach)
	{
		return Reach.Nodes[Reach.Nodes.Num() / 2].Profile;
	}

	inline FMassifChannelSolveParams MakeProfileTestParams()
	{
		FMassifChannelSolveParams Params;
		// The closed-form sources have no quantisation and no staircase in them, so the
		// slope is exact between neighbours and a window would only reach outside the
		// reach.
		Params.SlopeWindowNodes = 1;
		return Params;
	}

	/** The first fit alone: a section at every station, with the discharge fitted into it. */
	inline TArray<FMassifRiverReach> SolveProfiles(
		const IMassifElevationSource& Source, FMassifRiverReach&& Reach)
	{
		TArray<FMassifRiverReach> Reaches;
		Reaches.Add(MoveTemp(Reach));
		MassifSolveChannelProfiles(Source, MakeProfileTestParams(), Reaches);
		return Reaches;
	}

	/** Lowest point of a station's bed, which is the invert the terrain is cut to. */
	inline double ThalwegAltitudeMeters(const FMassifChannelProfile& Profile)
	{
		double LowestMeters = Profile.BedAltitudeMeters[0];
		for (int32 OffsetIndex = 1; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			LowestMeters = FMath::Min(LowestMeters, static_cast<double>(Profile.BedAltitudeMeters[OffsetIndex]));
		}
		return LowestMeters;
	}

	/**
	 * How far the deep point of the channel sits from the line the reach was traced
	 * along, positive towards the right bank.
	 *
	 * What a bend does to a channel: the water is thrown against the outer bank and
	 * the deep point goes with it, which is meander migration seen in one section.
	 */
	inline double ThalwegOffsetMeters(const FMassifChannelProfile& Profile)
	{
		return Profile.OffsetMeters(MassifChannelHydraulics::FindThalwegOffset(Profile));
	}

}
