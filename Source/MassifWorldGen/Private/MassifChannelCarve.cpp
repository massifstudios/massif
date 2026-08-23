#include "MassifChannelCarve.h"

#include "Async/ParallelFor.h"
#include "MassifChannelHydraulics.h"
#include "MassifRiverNetwork.h"

namespace
{
	/** Meyer-Peter & Müller's bedload, and the two figures the material reduces to. */
	struct FBedloadModel
	{
		double CriticalShieldsNumber = 0.047;
		double Coefficient = 8.0;

		/** The shear at which Shields' number is one: what a stress is measured against. */
		double ShieldsScalePascals = 1.0;

		/** sqrt((s - 1) g D^3): what a dimensionless transport rate of one is, in m2/s. */
		double UnitRateSquareMetersPerSecond = 1.0;

		/** Volume of grain crossing a metre of bed each second, under this much shear. */
		double RatePerWidth(double ShearPascals) const
		{
			const double ShieldsNumber = ShearPascals / ShieldsScalePascals;
			const double Excess = ShieldsNumber - CriticalShieldsNumber;
			if (Excess <= 0.0)
			{
				return 0.0;
			}
			return Coefficient * FMath::Pow(Excess, 1.5) * UnitRateSquareMetersPerSecond;
		}
	};

	FBedloadModel MakeBedloadModel(const FMassifChannelCarveParams& Params)
	{
		const double SubmergedDensity =
			Params.SedimentDensityKgPerCubicMeter - MassifChannelHydraulics::WaterDensityKgPerCubicMeter;
		const double RelativeDensity =
			SubmergedDensity / MassifChannelHydraulics::WaterDensityKgPerCubicMeter;

		FBedloadModel Model;
		Model.CriticalShieldsNumber = Params.CriticalShieldsNumber;
		Model.Coefficient = Params.TransportCoefficient;
		Model.ShieldsScalePascals = SubmergedDensity
			* MassifChannelHydraulics::GravityMetersPerSecondSquared * Params.GrainDiameterMeters;
		Model.UnitRateSquareMetersPerSecond = FMath::Sqrt(
			RelativeDensity * MassifChannelHydraulics::GravityMetersPerSecondSquared
			* FMath::Pow(Params.GrainDiameterMeters, 3.0));
		return Model;
	}

	// -----------------------------------------------------------------------
	// What does not change from sweep to sweep
	// -----------------------------------------------------------------------

	/** Menger curvature through three stations. Positive where the channel turns left. */
	double SignedCurvaturePerMeter(FVector2d Before, FVector2d At, FVector2d After)
	{
		const FVector2d Incoming = At - Before;
		const FVector2d Outgoing = After - At;
		const double Denominator = Incoming.Length() * Outgoing.Length() * (After - Before).Length();
		if (Denominator <= UE_DOUBLE_SMALL_NUMBER)
		{
			return 0.0;
		}
		const double Cross = Incoming.X * Outgoing.Y - Incoming.Y * Outgoing.X;
		return 2.0 * Cross / Denominator;
	}

	void MeasureCurvature(const TArray<FMassifRiverNode>& Nodes, TArray<double>& OutCurvature)
	{
		OutCurvature.Init(0.0, Nodes.Num());
		for (int32 NodeIndex = 1; NodeIndex + 1 < Nodes.Num(); ++NodeIndex)
		{
			OutCurvature[NodeIndex] = SignedCurvaturePerMeter(
				Nodes[NodeIndex - 1].PositionMeters, Nodes[NodeIndex].PositionMeters,
				Nodes[NodeIndex + 1].PositionMeters);
		}
	}

	/** How readily the ground at each station gives way: 1 loose fill, down to the floor on rock. */
	void SampleErodibility(
		const IMassifBedMaterialSource& BedMaterial, const FMassifChannelCarveParams& Params,
		const TArray<FMassifRiverNode>& Nodes, TArray<double>& OutErodibility)
	{
		OutErodibility.SetNumUninitialized(Nodes.Num());
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			const double Hardness =
				FMath::Clamp(BedMaterial.SampleRockHardness(Nodes[NodeIndex].PositionMeters), 0.0, 1.0);
			OutErodibility[NodeIndex] = FMath::Max(1.0 - Hardness, Params.MinErodibility);
		}
	}

	// -----------------------------------------------------------------------
	// One station's flow, and what it can carry
	// -----------------------------------------------------------------------

	/** What one station is doing at the start of a sweep. */
	struct FStationFlow
	{
		double SurfaceAltitudeMeters = 0.0;
		FMassifSectionFlow Section;

		/** Grain crossing the whole section each second, which is what Exner differences. */
		double TransportCubicMetersPerSecond = 0.0;

		/** Busiest offset of the section, which is where scour is concentrated. */
		double PeakRateSquareMetersPerSecond = 0.0;
	};

	void MeasureOffsetTransport(
		const FMassifChannelProfile& Profile, const FBedloadModel& Model, TArrayView<double> OutRates,
		FStationFlow& InOutStation)
	{
		const double SpacingMeters = Profile.OffsetSpacingMeters();
		for (int32 OffsetIndex = InOutStation.Section.FirstWetOffset;
			OffsetIndex <= InOutStation.Section.LastWetOffset; ++OffsetIndex)
		{
			const double DepthMeters =
				InOutStation.SurfaceAltitudeMeters - Profile.BedAltitudeMeters[OffsetIndex];
			const double ShearPascals =
				MassifChannelHydraulics::ShearStressPascals(DepthMeters, Profile.EnergySlope);

			OutRates[OffsetIndex] = Model.RatePerWidth(ShearPascals);
			InOutStation.TransportCubicMetersPerSecond += OutRates[OffsetIndex] * SpacingMeters;
			InOutStation.PeakRateSquareMetersPerSecond =
				FMath::Max(InOutStation.PeakRateSquareMetersPerSecond, OutRates[OffsetIndex]);
		}
	}

	FStationFlow SolveStation(
		const FMassifRiverNode& Node, const FMassifChannelCarveParams& Params, const FBedloadModel& Model,
		TArrayView<double> OutRates)
	{
		const FMassifChannelProfile& Profile = Node.Profile;
		for (double& Rate : OutRates)
		{
			Rate = 0.0;
		}

		FStationFlow Station;
		Station.SurfaceAltitudeMeters = MassifChannelHydraulics::SolveSurfaceAltitudeMeters(
			Profile, Profile.Roughness, Profile.EnergySlope, Node.DischargeCubicMetersPerSecond,
			Params.MinDepthMeters, Params.SurfaceIterations);
		Station.Section = MassifChannelHydraulics::MeasureSection(Profile, Station.SurfaceAltitudeMeters);
		if (!Station.Section.IsWet())
		{
			return Station;
		}

		MeasureOffsetTransport(Profile, Model, OutRates, Station);
		return Station;
	}

	// -----------------------------------------------------------------------
	// Moving the bed
	// -----------------------------------------------------------------------

	/**
	 * Exner over one station: what comes in less what goes out, spread over the
	 * width it is spread over.
	 *
	 * Limited smoothly rather than clipped. A hard cap makes a station near balance
	 * step the same distance every sweep and chatter across the answer instead of
	 * arriving at it; a smooth one lets the step fall away as the imbalance does,
	 * so the sweep count buys a settled bed rather than a busier one.
	 */
	/**
	 * Hardness slows the cut and nothing else: what settles out of the water settles
	 * at the same rate whatever it lands on.
	 */
	double BedChangeLimitMeters(const FMassifChannelCarveParams& Params, double RawMeters, double Erodibility)
	{
		if (RawMeters < 0.0)
		{
			return Params.MaxBedChangeMetersPerStep * Erodibility;
		}
		return Params.MaxBedChangeMetersPerStep;
	}

	double BedChangeMeters(
		const FMassifChannelCarveParams& Params, const FStationFlow& Station, double SupplyCubicMetersPerSecond,
		double SpanMeters, double Erodibility)
	{
		if (!Station.Section.IsWet() || SpanMeters <= UE_DOUBLE_SMALL_NUMBER)
		{
			return 0.0;
		}

		const double SolidVolumeRate = SupplyCubicMetersPerSecond - Station.TransportCubicMetersPerSecond;
		const double AreaChangeRate = SolidVolumeRate / (SpanMeters * (1.0 - Params.BedPorosity));
		const double RawMeters = AreaChangeRate * Params.StepSeconds / Station.Section.TopWidthMeters;

		const double LimitMeters = BedChangeLimitMeters(Params, RawMeters, Erodibility);
		return LimitMeters * FMath::Tanh(RawMeters / LimitMeters);
	}

	/** Scour follows the flow: the offset already carrying most takes most of the cut. */
	void LowerBedWhereItCarries(
		FMassifChannelProfile& InOutProfile, const FStationFlow& Station, TArrayView<const double> Rates,
		double ChangeMeters)
	{
		if (Station.PeakRateSquareMetersPerSecond <= UE_DOUBLE_SMALL_NUMBER)
		{
			return;
		}
		for (int32 OffsetIndex = Station.Section.FirstWetOffset;
			OffsetIndex <= Station.Section.LastWetOffset; ++OffsetIndex)
		{
			const double Share = Rates[OffsetIndex] / Station.PeakRateSquareMetersPerSecond;
			InOutProfile.BedAltitudeMeters[OffsetIndex] += static_cast<float>(ChangeMeters * Share);
		}
	}

	/** Fill lies flat: what settles out covers the whole wetted bed evenly. */
	void RaiseBedEvenly(
		FMassifChannelProfile& InOutProfile, const FStationFlow& Station, double ChangeMeters)
	{
		for (int32 OffsetIndex = Station.Section.FirstWetOffset;
			OffsetIndex <= Station.Section.LastWetOffset; ++OffsetIndex)
		{
			InOutProfile.BedAltitudeMeters[OffsetIndex] += static_cast<float>(ChangeMeters);
		}
	}

	void ApplyBedChange(
		FMassifChannelProfile& InOutProfile, const FStationFlow& Station, TArrayView<const double> Rates,
		double ChangeMeters)
	{
		if (ChangeMeters >= 0.0)
		{
			RaiseBedEvenly(InOutProfile, Station, ChangeMeters);
			return;
		}
		LowerBedWhereItCarries(InOutProfile, Station, Rates, ChangeMeters);
	}

	// -----------------------------------------------------------------------
	// Leaning the bed into the bend
	// -----------------------------------------------------------------------

	/**
	 * The straight line through the wetted bed, as a rise per metre across towards
	 * the right bank, taken about the line the reach was traced along.
	 *
	 * About the centreline rather than about the middle of the water. Taken about
	 * the water, the axis follows the channel as the bend moves it, so the lean is
	 * never reached and the channel migrates one offset further out every sweep
	 * until it leaves the section it was cut in — while the polyline everything else
	 * draws from stays exactly where it was.
	 */
	double MeanBedAltitudeMeters(const FMassifChannelProfile& Profile, const FMassifSectionFlow& Section)
	{
		double TotalMeters = 0.0;
		for (int32 OffsetIndex = Section.FirstWetOffset; OffsetIndex <= Section.LastWetOffset; ++OffsetIndex)
		{
			TotalMeters += Profile.BedAltitudeMeters[OffsetIndex];
		}
		return TotalMeters / (Section.LastWetOffset - Section.FirstWetOffset + 1);
	}

	double MeasureTransverseSlope(const FMassifChannelProfile& Profile, const FMassifSectionFlow& Section)
	{
		// Against the section's own mean, or the fit is dominated by how high above
		// the sea the reach happens to run rather than by the shape of its bed.
		const double MeanMeters = MeanBedAltitudeMeters(Profile, Section);
		double CrossSum = 0.0;
		double SquareSum = 0.0;
		for (int32 OffsetIndex = Section.FirstWetOffset; OffsetIndex <= Section.LastWetOffset; ++OffsetIndex)
		{
			const double AcrossMeters = Profile.OffsetMeters(OffsetIndex);
			CrossSum += AcrossMeters * (Profile.BedAltitudeMeters[OffsetIndex] - MeanMeters);
			SquareSum += AcrossMeters * AcrossMeters;
		}
		if (SquareSum <= UE_DOUBLE_SMALL_NUMBER)
		{
			return 0.0;
		}
		return CrossSum / SquareSum;
	}

	/**
	 * How much of a tilt the section has room for before the side it lifts reaches
	 * the ground it was cut from.
	 *
	 * A bar builds until it is out of the water and then stops taking material, and
	 * the tilt has to stop with it. Scaled rather than clipped, so what comes off one
	 * bank still equals what goes onto the other: a tilt whose lifted side is cut off
	 * at the ground moves the channel a little further out every sweep, and over a
	 * hundred of them walks it clean out of its own valley.
	 */
	double TiltRoomShare(
		const FMassifChannelProfile& Profile, const FMassifSectionFlow& Section, double SlopeChange)
	{
		double Share = 1.0;
		for (int32 OffsetIndex = Section.FirstWetOffset; OffsetIndex <= Section.LastWetOffset; ++OffsetIndex)
		{
			const double RiseMeters = SlopeChange * Profile.OffsetMeters(OffsetIndex);
			if (RiseMeters <= UE_DOUBLE_SMALL_NUMBER)
			{
				continue;
			}

			const double RoomMeters = FMath::Max(
				Profile.GroundAltitudeMeters[OffsetIndex] - Profile.BedAltitudeMeters[OffsetIndex], 0.0);
			Share = FMath::Min(Share, RoomMeters / RiseMeters);
		}
		return Share;
	}

	void TiltBed(
		FMassifChannelProfile& InOutProfile, const FMassifSectionFlow& Section, double SlopeChange)
	{
		const double Share = TiltRoomShare(InOutProfile, Section, SlopeChange);
		for (int32 OffsetIndex = Section.FirstWetOffset; OffsetIndex <= Section.LastWetOffset; ++OffsetIndex)
		{
			InOutProfile.BedAltitudeMeters[OffsetIndex] +=
				static_cast<float>(Share * SlopeChange * InOutProfile.OffsetMeters(OffsetIndex));
		}
	}

	/**
	 * Leans the wetted bed towards the slope the bend's own circulation holds it at.
	 *
	 * Only the straight part of the section is touched: the correction is a rotation
	 * about the middle of the water, so the trench the station has cut for itself
	 * survives it and only the tilt across the channel changes. A reach with no bend
	 * in it is left exactly as it was.
	 */
	/**
	 * How steeply the bend holds the bed across the channel.
	 *
	 * Positive curvature turns left, so the outer bank is the right one and the bed
	 * has to fall towards it. Held under a fall of a few channel depths across the
	 * whole width, which is what stops the lean and the slumping from quarrying the
	 * section between them.
	 */
	double BendTargetSlope(
		const FMassifSectionFlow& Section, const FMassifChannelCarveParams& Params,
		double HydraulicDepthMeters, double CurvaturePerMeter)
	{
		const double RawSlope =
			-Params.TransverseSlopeCoefficient * Section.HydraulicDepthMeters() * CurvaturePerMeter;
		const double MaxDropMeters = Params.MaxBendReliefDepthFactor * HydraulicDepthMeters;
		const double MaxSlope = MaxDropMeters / FMath::Max(Section.TopWidthMeters, UE_DOUBLE_SMALL_NUMBER);
		return FMath::Clamp(RawSlope, -MaxSlope, MaxSlope);
	}

	void ApplyBendSort(
		FMassifChannelProfile& InOutProfile, const FMassifSectionFlow& Section,
		const FMassifChannelCarveParams& Params, double HydraulicDepthMeters, double CurvaturePerMeter)
	{
		if (!Section.IsWet())
		{
			return;
		}

		const double TargetSlope =
			BendTargetSlope(Section, Params, HydraulicDepthMeters, CurvaturePerMeter);
		const double CurrentSlope = MeasureTransverseSlope(InOutProfile, Section);
		TiltBed(InOutProfile, Section, Params.TransverseRelaxation * (TargetSlope - CurrentSlope));
	}

	// -----------------------------------------------------------------------
	// What the material will stand at
	// -----------------------------------------------------------------------

	/**
	 * Slumps anything standing steeper than the material's angle of repose.
	 *
	 * Volume conserving: what one offset loses the offset beside it gains, so an
	 * undercut bank falls into the channel rather than evaporating out of it.
	 */
	void ReposeSweep(
		FMassifChannelProfile& InOutProfile, const FMassifSectionFlow& Section, double MaxRiseMeters)
	{
		// The water and the bank it stands against, and no further. Slumping moves
		// what the channel has undercut; run over the whole profile it quarries the
		// hillside beyond the bank as well, and a deep enough channel pulls a funnel
		// of untouched ground in from both sides of the section.
		const int32 FirstOffset = FMath::Max(Section.FirstWetOffset - 1, 0);
		const int32 LastOffset = FMath::Min(Section.LastWetOffset + 1, MassifChannelProfileOffsetCount - 1);

		// Every move is measured against the bed as the sweep found it, so the answer
		// does not depend on which bank the sweep started from. Taken one offset at a
		// time it would carry material along in the direction it happens to run, and a
		// straight channel would come out leaning.
		double MoveMeters[MassifChannelProfileOffsetCount] = {};
		for (int32 OffsetIndex = FirstOffset; OffsetIndex < LastOffset; ++OffsetIndex)
		{
			const double RiseMeters =
				InOutProfile.BedAltitudeMeters[OffsetIndex + 1] - InOutProfile.BedAltitudeMeters[OffsetIndex];
			const double ExcessMeters = FMath::Abs(RiseMeters) - MaxRiseMeters;
			const double ShareMeters = 0.25 * FMath::Max(ExcessMeters, 0.0) * FMath::Sign(RiseMeters);
			MoveMeters[OffsetIndex] += ShareMeters;
			MoveMeters[OffsetIndex + 1] -= ShareMeters;
		}
		for (int32 OffsetIndex = FirstOffset; OffsetIndex <= LastOffset; ++OffsetIndex)
		{
			InOutProfile.BedAltitudeMeters[OffsetIndex] += static_cast<float>(MoveMeters[OffsetIndex]);
		}
	}

	void RelaxToRepose(
		FMassifChannelProfile& InOutProfile, const FMassifSectionFlow& Section, double MaxRiseMeters,
		int32 SweepCount)
	{
		if (!Section.IsWet())
		{
			return;
		}
		for (int32 Sweep = 0; Sweep < SweepCount; ++Sweep)
		{
			ReposeSweep(InOutProfile, Section, MaxRiseMeters);
		}
	}

	/** The bed stays under the ground it was cut from, and above what the carve is allowed to reach. */
	void HoldBedInsideGround(FMassifChannelProfile& InOutProfile, double MaxIncisionMeters)
	{
		for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			const double GroundMeters = InOutProfile.GroundAltitudeMeters[OffsetIndex];
			InOutProfile.BedAltitudeMeters[OffsetIndex] = static_cast<float>(FMath::Clamp(
				static_cast<double>(InOutProfile.BedAltitudeMeters[OffsetIndex]),
				GroundMeters - MaxIncisionMeters, GroundMeters));
		}
	}

	// -----------------------------------------------------------------------
	// A sweep of a reach
	// -----------------------------------------------------------------------

	/** Everything a reach's sweeps reuse, so a sweep allocates nothing. */
	struct FReachScratch
	{
		TArray<double> SpanMeters;
		TArray<double> CurvaturePerMeter;
		TArray<double> Erodibility;

		TArray<FStationFlow> Stations;
		TArray<double> OffsetRates;
		TArray<double> ChangeMeters;
		TArray<double> SmoothedMeters;
		TArray<float> DiffusedBedMeters;

		void Prepare(
			const IMassifBedMaterialSource& BedMaterial, const FMassifChannelCarveParams& Params,
			const TArray<FMassifRiverNode>& Nodes)
		{
			MassifChannelHydraulics::MeasureStationSpans(Nodes, SpanMeters);
			MeasureCurvature(Nodes, CurvaturePerMeter);
			SampleErodibility(BedMaterial, Params, Nodes, Erodibility);
			Stations.SetNum(Nodes.Num());
			OffsetRates.SetNumZeroed(Nodes.Num() * MassifChannelProfileOffsetCount);
			ChangeMeters.SetNumZeroed(Nodes.Num());
			SmoothedMeters.SetNumZeroed(Nodes.Num());
			DiffusedBedMeters.SetNumZeroed(Nodes.Num() * MassifChannelProfileOffsetCount);
		}

		TArrayView<double> RatesAt(int32 NodeIndex)
		{
			return TArrayView<double>(
				OffsetRates.GetData() + NodeIndex * MassifChannelProfileOffsetCount,
				MassifChannelProfileOffsetCount);
		}
	};

	/**
	 * The slope each station is losing head at, from the levels this sweep found,
	 * for the sweep after it to solve against.
	 *
	 * Remeasured rather than kept, because a reach that cuts into its own bed loses
	 * the gradient it was cutting with, and that is what stops it. Held at the one
	 * it started with, a deepening trench gains shear as it goes — the water in it
	 * gets deeper faster than the channel narrows — and cuts until it hits a limit
	 * rather than until it is graded.
	 */
	void UpdateEnergySlopes(
		TArray<FMassifRiverNode>& InOutNodes, const FMassifChannelCarveParams& Params,
		const FReachScratch& Scratch)
	{
		for (int32 NodeIndex = 0; NodeIndex < InOutNodes.Num(); ++NodeIndex)
		{
			const int32 FirstIndex = FMath::Max(NodeIndex - Params.SlopeWindowNodes, 0);
			const int32 LastIndex = FMath::Min(NodeIndex + Params.SlopeWindowNodes, InOutNodes.Num() - 1);
			double RunMeters = 0.0;
			for (int32 SpanIndex = FirstIndex + 1; SpanIndex <= LastIndex; ++SpanIndex)
			{
				RunMeters += Scratch.SpanMeters[SpanIndex];
			}

			const double DropMeters = Scratch.Stations[FirstIndex].SurfaceAltitudeMeters
				- Scratch.Stations[LastIndex].SurfaceAltitudeMeters;
			InOutNodes[NodeIndex].Profile.EnergySlope =
				FMath::Max(DropMeters / FMath::Max(RunMeters, UE_DOUBLE_SMALL_NUMBER), Params.MinEnergySlope);
		}
	}

	/** Where the water stands at every station, and what it can carry there. */
	void MeasureReachFlow(
		const TArray<FMassifRiverNode>& Nodes, const FMassifChannelCarveParams& Params,
		const FBedloadModel& Model, FReachScratch& InOutScratch)
	{
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			InOutScratch.Stations[NodeIndex] =
				SolveStation(Nodes[NodeIndex], Params, Model, InOutScratch.RatesAt(NodeIndex));
		}
	}

	/**
	 * What each station gains or loses: what arrives from the station above less
	 * what leaves.
	 *
	 * The head is fed at its own capacity, which leaves it in balance and makes it
	 * the datum the rest of the reach grades to. Nothing is routed between reaches,
	 * so a confluence neither starves nor buries the channel below it.
	 */
	void MeasureBedChange(const FMassifChannelCarveParams& Params, FReachScratch& InOutScratch)
	{
		for (int32 NodeIndex = 0; NodeIndex < InOutScratch.Stations.Num(); ++NodeIndex)
		{
			const int32 SupplyIndex = FMath::Max(NodeIndex - 1, 0);
			InOutScratch.ChangeMeters[NodeIndex] = BedChangeMeters(
				Params, InOutScratch.Stations[NodeIndex],
				InOutScratch.Stations[SupplyIndex].TransportCubicMetersPerSecond,
				InOutScratch.SpanMeters[NodeIndex], InOutScratch.Erodibility[NodeIndex]);
		}
	}

	/**
	 * Spreads each station's gain or loss into the stations either side of it.
	 *
	 * Bed altitudes are floats over a thousand metres of relief, so two stations of
	 * one shape do not measure exactly the same load, and the difference between
	 * neighbours is the one place that last digit lands. Left alone it becomes a
	 * sawtooth: every other station cuts while the ones between them fill. What
	 * distinguishes it from a real imbalance is that it alternates, and one pass of
	 * this removes exactly that.
	 */
	void SmoothBedChange(FReachScratch& InOutScratch, int32 PassCount)
	{
		const int32 LastIndex = InOutScratch.ChangeMeters.Num() - 1;
		for (int32 Pass = 0; Pass < PassCount; ++Pass)
		{
			for (int32 NodeIndex = 0; NodeIndex <= LastIndex; ++NodeIndex)
			{
				const double BeforeMeters = InOutScratch.ChangeMeters[FMath::Max(NodeIndex - 1, 0)];
				const double AfterMeters = InOutScratch.ChangeMeters[FMath::Min(NodeIndex + 1, LastIndex)];
				InOutScratch.SmoothedMeters[NodeIndex] =
					0.25 * (BeforeMeters + AfterMeters) + 0.5 * InOutScratch.ChangeMeters[NodeIndex];
			}
			Swap(InOutScratch.ChangeMeters, InOutScratch.SmoothedMeters);
		}
	}

	void ShapeStationBed(
		FMassifRiverNode& InOutNode, const FMassifChannelCarveParams& Params, const FStationFlow& Station,
		TArrayView<const double> Rates, double ChangeMeters, double CurvaturePerMeter)
	{
		const double ReposeRise = FMath::Tan(FMath::DegreesToRadians(Params.BankFrictionAngleDegrees))
			* InOutNode.Profile.OffsetSpacingMeters();

		// Held inside the ground before the tilt as well as after it, so the room the
		// tilt measures for its bar is the room that is really there. Measured against
		// a bed still standing above the ground, the tilt lifts material that is then
		// cut away, and what it took off the other bank stays gone.
		ApplyBedChange(InOutNode.Profile, Station, Rates, ChangeMeters);
		HoldBedInsideGround(InOutNode.Profile, Params.MaxIncisionBelowGroundMeters);
		ApplyBendSort(
			InOutNode.Profile, Station.Section, Params, InOutNode.DepthMeters, CurvaturePerMeter);
		RelaxToRepose(InOutNode.Profile, Station.Section, ReposeRise, Params.BankSweepCount);
		HoldBedInsideGround(InOutNode.Profile, Params.MaxIncisionBelowGroundMeters);
	}

	void ShapeReachBed(
		TArray<FMassifRiverNode>& InOutNodes, const FMassifChannelCarveParams& Params,
		FReachScratch& InOutScratch)
	{
		for (int32 NodeIndex = 0; NodeIndex < InOutNodes.Num(); ++NodeIndex)
		{
			ShapeStationBed(
				InOutNodes[NodeIndex], Params, InOutScratch.Stations[NodeIndex],
				InOutScratch.RatesAt(NodeIndex), InOutScratch.ChangeMeters[NodeIndex],
				InOutScratch.CurvaturePerMeter[NodeIndex]);
		}
	}

	/** The bed this station would relax to, held against the pre-sweep beds either side. */
	void DiffuseStationBed(
		const TArray<FMassifRiverNode>& Nodes, const FMassifChannelCarveParams& Params, int32 NodeIndex,
		FReachScratch& InOutScratch)
	{
		const float* AboveMeters = Nodes[NodeIndex - 1].Profile.BedAltitudeMeters;
		const float* HereMeters = Nodes[NodeIndex].Profile.BedAltitudeMeters;
		const float* BelowMeters = Nodes[NodeIndex + 1].Profile.BedAltitudeMeters;
		float* OutMeters = InOutScratch.DiffusedBedMeters.GetData() + NodeIndex * MassifChannelProfileOffsetCount;

		for (int32 OffsetIndex = 0; OffsetIndex < MassifChannelProfileOffsetCount; ++OffsetIndex)
		{
			const double CurvatureMeters = static_cast<double>(AboveMeters[OffsetIndex])
				- 2.0 * HereMeters[OffsetIndex] + BelowMeters[OffsetIndex];
			OutMeters[OffsetIndex] =
				static_cast<float>(HereMeters[OffsetIndex] + Params.BedDiffusion * CurvatureMeters);
		}
	}

	void ApplyDiffusedBed(
		FMassifChannelProfile& InOutProfile, const FMassifChannelCarveParams& Params, int32 NodeIndex,
		const FReachScratch& Scratch)
	{
		const float* DiffusedMeters =
			Scratch.DiffusedBedMeters.GetData() + NodeIndex * MassifChannelProfileOffsetCount;
		FMemory::Memcpy(
			InOutProfile.BedAltitudeMeters, DiffusedMeters,
			MassifChannelProfileOffsetCount * sizeof(float));
		HoldBedInsideGround(InOutProfile, Params.MaxIncisionBelowGroundMeters);
	}

	/**
	 * Relaxes the bed along the reach, the way the repose sweep relaxes it across.
	 *
	 * The threshold of motion is a cliff: a station a hair under it carries nothing
	 * and hands nothing on, and the station below carries what it can out of its own
	 * bed instead. Along a uniform reach every station has the same geometry, so
	 * which side of the threshold each one lands on is decided by the last digit of
	 * a float altitude over a kilometre of relief — and the bed comes out as a row
	 * of pits between stations that never moved at all.
	 *
	 * A pit one station wide is a bed slope no gravel holds against the water
	 * running over it, whatever put it there. Diffusion takes those out over the
	 * sweeps while leaving a wedge that spans many stations, because it works on the
	 * curvature and a wedge has almost none.
	 *
	 * Jacobi, so a station's move does not depend on which end the sweep started
	 * from, and per offset, so the shape across the channel is carried along rather
	 * than averaged away.
	 */
	void DiffuseBedAlongReach(
		TArray<FMassifRiverNode>& InOutNodes, const FMassifChannelCarveParams& Params,
		FReachScratch& InOutScratch)
	{
		const int32 LastIndex = InOutNodes.Num() - 1;
		if (LastIndex < 2 || Params.BedDiffusion <= 0.0)
		{
			return;
		}

		for (int32 NodeIndex = 1; NodeIndex < LastIndex; ++NodeIndex)
		{
			DiffuseStationBed(InOutNodes, Params, NodeIndex, InOutScratch);
		}
		for (int32 NodeIndex = 1; NodeIndex < LastIndex; ++NodeIndex)
		{
			ApplyDiffusedBed(InOutNodes[NodeIndex].Profile, Params, NodeIndex, InOutScratch);
		}
	}

	/**
	 * One sweep of the reach: measure it all, work out what each station owes the
	 * one below, then move every bed at once.
	 *
	 * All at once rather than station by station downstream. Moving a bed before
	 * reading the station under it makes what a station receives depend on the order
	 * the reach was walked in, and that difference is what the sawtooth grows from.
	 */
	void CarveSweep(
		FMassifRiverReach& InOutReach, const FMassifChannelCarveParams& Params, const FBedloadModel& Model,
		FReachScratch& InOutScratch)
	{
		MeasureReachFlow(InOutReach.Nodes, Params, Model, InOutScratch);
		UpdateEnergySlopes(InOutReach.Nodes, Params, InOutScratch);
		MeasureBedChange(Params, InOutScratch);
		SmoothBedChange(InOutScratch, Params.ChangeSmoothingPasses);
		ShapeReachBed(InOutReach.Nodes, Params, InOutScratch);
		DiffuseBedAlongReach(InOutReach.Nodes, Params, InOutScratch);
	}

	void CarveReach(
		const IMassifBedMaterialSource& BedMaterial, const FMassifChannelCarveParams& Params,
		const FBedloadModel& Model, FMassifRiverReach& InOutReach)
	{
		if (InOutReach.Nodes.IsEmpty())
		{
			return;
		}

		FReachScratch Scratch;
		Scratch.Prepare(BedMaterial, Params, InOutReach.Nodes);
		for (int32 Step = 0; Step < Params.StepCount; ++Step)
		{
			CarveSweep(InOutReach, Params, Model, Scratch);
		}
		for (FMassifRiverNode& Node : InOutReach.Nodes)
		{
			MassifChannelHydraulics::ApplySectionToNode(Node, Params.MinDepthMeters);
		}
	}
}

FMassifUniformBedMaterial::FMassifUniformBedMaterial(double InRockHardness)
	: RockHardness(FMath::Clamp(InRockHardness, 0.0, 1.0))
{
}

double FMassifUniformBedMaterial::SampleRockHardness(FVector2d PositionMeters) const
{
	return RockHardness;
}

void MassifCarveChannels(
	const IMassifBedMaterialSource& BedMaterial,
	const FMassifChannelCarveParams& Params,
	TArray<FMassifRiverReach>& InOutReaches)
{
	if (Params.Fidelity == EMassifFidelity::Off)
	{
		return;
	}

	const FBedloadModel Model = MakeBedloadModel(Params);
	ParallelFor(InOutReaches.Num(), [&BedMaterial, &Params, &Model, &InOutReaches](int32 ReachIndex)
	{
		CarveReach(BedMaterial, Params, Model, InOutReaches[ReachIndex]);
	});
}
