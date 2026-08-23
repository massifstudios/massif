#include "MassifChannelFlow.h"

#include "Async/ParallelFor.h"
#include "MassifChannelHydraulics.h"
#include "MassifRiverNetwork.h"

namespace
{
	double SlopeOrFloor(double DropMeters, double RunMeters, const FMassifChannelFlowParams& Params)
	{
		if (RunMeters <= UE_DOUBLE_SMALL_NUMBER)
		{
			return Params.MinEnergySlope;
		}
		return FMath::Max(DropMeters / RunMeters, Params.MinEnergySlope);
	}

	/**
	 * The slope the depth at each station is solved against, measured over a window
	 * of the surface rather than over one span.
	 *
	 * The surface is stored to a few centimetres and a span is a handful of metres,
	 * so one span's drop is mostly quantisation. A window long enough to leave that
	 * behind is what stops a staircase reading as a run of alternating pools and
	 * rapids.
	 */
	void MeasureEnergySlopes(
		const TArray<FMassifRiverNode>& Nodes, const FMassifChannelFlowParams& Params,
		TArrayView<const double> SpanMeters, TArray<double>& OutSlopes)
	{
		OutSlopes.SetNumUninitialized(Nodes.Num());
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			const int32 FirstIndex = FMath::Max(NodeIndex - Params.SlopeWindowNodes, 0);
			const int32 LastIndex = FMath::Min(NodeIndex + Params.SlopeWindowNodes, Nodes.Num() - 1);
			double RunMeters = 0.0;
			for (int32 SpanIndex = FirstIndex + 1; SpanIndex <= LastIndex; ++SpanIndex)
			{
				RunMeters += SpanMeters[SpanIndex];
			}

			const double DropMeters =
				Nodes[FirstIndex].WaterSurfaceMeters - Nodes[LastIndex].WaterSurfaceMeters;
			OutSlopes[NodeIndex] = SlopeOrFloor(DropMeters, RunMeters, Params);
		}
	}

	/** The level the station's own section carries its discharge at, never over its lower bank. */
	double NormalSurfaceMeters(const FMassifRiverNode& Node, const FMassifChannelFlowParams& Params)
	{
		const FMassifChannelProfile& Profile = Node.Profile;
		return MassifChannelHydraulics::SolveSurfaceAltitudeMeters(
			Profile, Profile.Roughness, Profile.EnergySlope, Node.DischargeCubicMetersPerSecond,
			Params.MinDepthMeters, Params.SurfaceIterations);
	}

	/**
	 * The friction slope a section is actually losing head at, which is the slope at
	 * which its own geometry would carry this discharge uniformly.
	 */
	double FrictionSlope(const FMassifRiverNode& Node, double SurfaceAltitudeMeters)
	{
		const FMassifSectionFlow Section =
			MassifChannelHydraulics::MeasureSection(Node.Profile, SurfaceAltitudeMeters);
		if (!Section.IsWet())
		{
			return Node.Profile.EnergySlope;
		}

		const double Conveyance =
			Section.AreaSquareMeters * FMath::Pow(Section.HydraulicRadiusMeters(), 2.0 / 3.0)
				/ Node.Profile.Roughness;
		if (Conveyance <= UE_DOUBLE_SMALL_NUMBER)
		{
			return Node.Profile.EnergySlope;
		}
		return FMath::Square(Node.DischargeCubicMetersPerSecond / Conveyance);
	}

	/**
	 * Carries the surface one station upstream against friction.
	 *
	 * The head lost between two stations is the mean friction slope over the run
	 * between them, so a station whose section cannot pass the water at the slope
	 * it is given sits higher than normal depth and the water ponds behind it. The
	 * velocity head is left out: over stations a few metres apart on a gravel bed
	 * it is far smaller than the friction term, and carrying it would need the
	 * energy equation solved for a level that is not monotone in it.
	 */
	double BackwaterSurfaceMeters(
		const FMassifRiverNode& Node, const FMassifChannelFlowParams& Params, double DownstreamSurfaceMeters,
		double DownstreamFrictionSlope, double SpanMeters, double NormalMeters)
	{
		const double MeanSlope = 0.5 * (FrictionSlope(Node, DownstreamSurfaceMeters) + DownstreamFrictionSlope);
		const double PondedMeters = FMath::Max(NormalMeters, DownstreamSurfaceMeters + MeanSlope * SpanMeters);

		// A pool fills to its own bank and no further, unless what is below it stands
		// higher still — at which point the water is over the bank because the reach
		// downstream cannot take it, which is what a flood is.
		const double CeilingMeters = FMath::Max(
			MassifChannelHydraulics::SurfaceCeilingMeters(Node.Profile, Params.MinDepthMeters),
			DownstreamSurfaceMeters);
		return FMath::Min(PondedMeters, CeilingMeters);
	}

	/** Every station at its own normal depth, with none of them left standing over the one below. */
	void SolveNormalDepth(TArray<FMassifRiverNode>& InOutNodes, const FMassifChannelFlowParams& Params)
	{
		for (FMassifRiverNode& Node : InOutNodes)
		{
			Node.WaterSurfaceMeters = NormalSurfaceMeters(Node, Params);
		}
		for (int32 NodeIndex = InOutNodes.Num() - 2; NodeIndex >= 0; --NodeIndex)
		{
			InOutNodes[NodeIndex].WaterSurfaceMeters = FMath::Max(
				InOutNodes[NodeIndex].WaterSurfaceMeters, InOutNodes[NodeIndex + 1].WaterSurfaceMeters);
		}
	}

	/** The outlet at normal depth, then upstream against friction from there. */
	void SolveBackwater(
		TArray<FMassifRiverNode>& InOutNodes, const FMassifChannelFlowParams& Params,
		TArrayView<const double> SpanMeters)
	{
		double DownstreamSurfaceMeters = NormalSurfaceMeters(InOutNodes.Last(), Params);
		InOutNodes.Last().WaterSurfaceMeters = DownstreamSurfaceMeters;

		double DownstreamFrictionSlope = FrictionSlope(InOutNodes.Last(), DownstreamSurfaceMeters);
		for (int32 NodeIndex = InOutNodes.Num() - 2; NodeIndex >= 0; --NodeIndex)
		{
			FMassifRiverNode& Node = InOutNodes[NodeIndex];
			const double SurfaceMeters = BackwaterSurfaceMeters(
				Node, Params, DownstreamSurfaceMeters, DownstreamFrictionSlope, SpanMeters[NodeIndex + 1],
				NormalSurfaceMeters(Node, Params));

			Node.WaterSurfaceMeters = SurfaceMeters;
			DownstreamFrictionSlope = FrictionSlope(Node, SurfaceMeters);
			DownstreamSurfaceMeters = SurfaceMeters;
		}
	}

	void SolveReachFlow(FMassifRiverReach& InOutReach, const FMassifChannelFlowParams& Params)
	{
		TArray<double> SpanMeters;
		TArray<double> Slopes;
		MassifChannelHydraulics::MeasureStationSpans(InOutReach.Nodes, SpanMeters);

		for (int32 Pass = 0; Pass < Params.PassCount; ++Pass)
		{
			MeasureEnergySlopes(InOutReach.Nodes, Params, SpanMeters, Slopes);
			for (int32 NodeIndex = 0; NodeIndex < InOutReach.Nodes.Num(); ++NodeIndex)
			{
				InOutReach.Nodes[NodeIndex].Profile.EnergySlope = Slopes[NodeIndex];
			}
			if (Params.Fidelity == EMassifFidelity::Cheap)
			{
				SolveNormalDepth(InOutReach.Nodes, Params);
				continue;
			}
			SolveBackwater(InOutReach.Nodes, Params, SpanMeters);
		}

		for (FMassifRiverNode& Node : InOutReach.Nodes)
		{
			MassifChannelHydraulics::ApplySectionToNode(Node, Params.MinDepthMeters);
		}
	}
}

void MassifSolveChannelFlow(
	const FMassifChannelFlowParams& Params, TArray<FMassifRiverReach>& InOutReaches)
{
	if (Params.Fidelity == EMassifFidelity::Off)
	{
		return;
	}

	ParallelFor(InOutReaches.Num(), [&Params, &InOutReaches](int32 ReachIndex)
	{
		if (InOutReaches[ReachIndex].Nodes.IsEmpty())
		{
			return;
		}
		SolveReachFlow(InOutReaches[ReachIndex], Params);
	});
}
