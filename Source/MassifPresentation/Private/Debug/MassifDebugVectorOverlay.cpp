#include "MassifDebugVectorOverlay.h"

#include "DrawDebugHelpers.h"
#include "HAL/IConsoleManager.h"
#include "MassifDebugCamera.h"
#include "MassifDebugChannels.h"
#include "MassifEnvironmentState.h"
#include "MassifHabitat.h"
#include "MassifTerrainField.h"
#include "MassifUnits.h"

namespace
{
	TAutoConsoleVariable<int32> CVarEnvironmentVectors(
		TEXT("massif.Env.Vectors"), 0,
		TEXT("Draw the selected vector channel as arrows over the ground."),
		ECVF_Cheat);

	/** Coarse on purpose: one arrow per cell is a field of noise, not a direction field. */
	constexpr int32 OverlayArrowsPerSide = 24;
	constexpr double OverlayRadiusMeters = 600.0;

	/** Fraction of the lattice spacing the strongest arrow reaches, so neighbours never overlap. */
	constexpr double LongestArrowSpacingFraction = 0.45;

	/** Clear of the ground without floating off it. */
	constexpr double ArrowHeightOffsetMeters = 3.0;

	constexpr float ArrowHeadSizeCentimeters = 250.0f;
	constexpr float ArrowThicknessCentimeters = 12.0f;

	double LatticeSpacingMeters(double RadiusMeters, int32 ArrowsPerSide)
	{
		return 2.0 * RadiusMeters / FMath::Max(ArrowsPerSide - 1, 1);
	}

	FVector2d LatticePosition(
		FVector2d CentreMeters, double SpacingMeters, int32 ArrowsPerSide, int32 LatticeIndex)
	{
		const double Origin = -0.5 * SpacingMeters * (ArrowsPerSide - 1);
		return CentreMeters
			+ FVector2d(Origin + SpacingMeters * (LatticeIndex % ArrowsPerSide),
				Origin + SpacingMeters * (LatticeIndex / ArrowsPerSide));
	}

	double LargestLength(TArrayView<const FVector2d> Vectors)
	{
		double Largest = 0.0;
		for (FVector2d Vector : Vectors)
		{
			Largest = FMath::Max(Largest, Vector.Length());
		}
		return Largest;
	}

	FMassifVectorArrow MakeArrow(
		FVector2d PositionMeters, FVector2d VectorValue, double LargestMagnitude, double SpacingMeters)
	{
		FMassifVectorArrow Arrow;
		Arrow.PositionMeters = PositionMeters;
		Arrow.MagnitudeFraction = VectorValue.Length() / LargestMagnitude;
		Arrow.ExtentMeters = VectorValue.GetSafeNormal()
			* (Arrow.MagnitudeFraction * LongestArrowSpacingFraction * SpacingMeters);
		return Arrow;
	}

	FVector ArrowEndCentimeters(const FMassifTerrainField& Terrain, FVector2d PositionMeters, double SpacingMeters)
	{
		const double AltitudeMeters =
			Terrain.SampleHeightMeters(PositionMeters, SpacingMeters) + ArrowHeightOffsetMeters;
		return FVector(PositionMeters.X, PositionMeters.Y, AltitudeMeters) * MassifUnits::MetersToCentimeters;
	}

	void DrawArrow(
		UWorld* World,
		const FMassifTerrainField& Terrain,
		const FMassifVectorArrow& Arrow,
		EMassifChannelColorMap ColorMap,
		double SpacingMeters)
	{
		const FVector StartCentimeters = ArrowEndCentimeters(Terrain, Arrow.PositionMeters, SpacingMeters);
		const FVector EndCentimeters =
			ArrowEndCentimeters(Terrain, Arrow.PositionMeters + Arrow.ExtentMeters, SpacingMeters);
		const FColor Color =
			MassifDebugChannels::EvaluateColorMap(ColorMap, Arrow.MagnitudeFraction).ToFColor(true);

		DrawDebugDirectionalArrow(
			World, StartCentimeters, EndCentimeters, ArrowHeadSizeCentimeters, Color, false, -1.0f, 0,
			ArrowThicknessCentimeters);
	}
}

bool MassifDebugVectorOverlay::IsEnabled()
{
	return CVarEnvironmentVectors.GetValueOnGameThread() != 0;
}

FMassifChannelHandle MassifDebugVectorOverlay::ChooseChannel(
	const TArray<FMassifDebugView>& Views, int32 SelectedViewIndex)
{
	if (Views.IsValidIndex(SelectedViewIndex) && Views[SelectedViewIndex].IsVectorField())
	{
		return Views[SelectedViewIndex].Channel;
	}

	const FMassifDebugView* FirstVectorView = Views.FindByPredicate([](const FMassifDebugView& View)
	{
		return View.IsVectorField();
	});
	if (FirstVectorView == nullptr)
	{
		return FMassifChannelHandle();
	}
	return FirstVectorView->Channel;
}

TArray<FMassifVectorArrow> MassifDebugVectorOverlay::BuildArrows(
	const FMassifEnvironmentState& Environment,
	FMassifChannelHandle Channel,
	FVector2d CentreMeters,
	double RadiusMeters,
	int32 ArrowsPerSide)
{
	const double SpacingMeters = LatticeSpacingMeters(RadiusMeters, ArrowsPerSide);

	TArray<FVector2d> Positions;
	TArray<FVector2d> Vectors;
	for (int32 LatticeIndex = 0; LatticeIndex < ArrowsPerSide * ArrowsPerSide; ++LatticeIndex)
	{
		Positions.Add(LatticePosition(CentreMeters, SpacingMeters, ArrowsPerSide, LatticeIndex));
		Vectors.Add(Environment.SampleVector(Channel, Positions.Last()));
	}

	const double LargestMagnitude = LargestLength(Vectors);
	TArray<FMassifVectorArrow> Arrows;
	if (LargestMagnitude <= 0.0)
	{
		return Arrows;
	}

	for (int32 ArrowIndex = 0; ArrowIndex < Positions.Num(); ++ArrowIndex)
	{
		Arrows.Add(MakeArrow(Positions[ArrowIndex], Vectors[ArrowIndex], LargestMagnitude, SpacingMeters));
	}
	return Arrows;
}

void MassifDebugVectorOverlay::Draw(
	UWorld* World,
	const FMassifHabitatField& Habitat,
	const TArray<FMassifDebugView>& Views,
	int32 SelectedViewIndex)
{
	if (World == nullptr || !IsEnabled())
	{
		return;
	}

	const FMassifChannelHandle Channel = ChooseChannel(Views, SelectedViewIndex);
	if (!Channel.IsValid())
	{
		return;
	}

	const FVector2d CentreMeters = MassifDebugCamera::GetPositionMeters(World);
	const TArray<FMassifVectorArrow> Arrows = BuildArrows(
		Habitat.GetEnvironment(), Channel, CentreMeters, OverlayRadiusMeters, OverlayArrowsPerSide);

	const EMassifChannelColorMap ColorMap = Habitat.GetEnvironment().GetDescriptor(Channel).ColorMap;
	const double SpacingMeters = LatticeSpacingMeters(OverlayRadiusMeters, OverlayArrowsPerSide);
	for (const FMassifVectorArrow& Arrow : Arrows)
	{
		DrawArrow(World, Habitat.GetTerrainField(), Arrow, ColorMap, SpacingMeters);
	}
}
