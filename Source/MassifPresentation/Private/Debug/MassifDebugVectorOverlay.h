#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentChannel.h"

class FMassifEnvironmentState;
class FMassifHabitatField;
class UWorld;
struct FMassifDebugView;

/** One arrow of a direction field, in world metres. */
struct FMassifVectorArrow
{
	FVector2d PositionMeters = FVector2d::ZeroVector;

	/** From the position to the head, already scaled against the strongest arrow on the lattice. */
	FVector2d ExtentMeters = FVector2d::ZeroVector;

	/** Where this arrow sits between nothing and the strongest arrow drawn, for its colour. */
	double MagnitudeFraction = 0.0;
};

/**
 * Draws a Vector2 channel as arrows over the ground.
 *
 * A scalar heatmap of a direction field's magnitude cannot show it being
 * steered the wrong way round a ridge, which is the whole reason this exists
 * (ARCHITECTURE §5.7). Sampled on a coarse world-space lattice around the
 * camera, never one arrow per cell.
 */
namespace MassifDebugVectorOverlay
{
	/** Whether the overlay is drawn. Toggled by massif.Env.Vectors. */
	bool IsEnabled();

	/**
	 * The channel the overlay draws: the selected view where that is a vector
	 * field, otherwise the first registered one, so a direction field can be read
	 * against a scalar heatmap of something else.
	 */
	FMassifChannelHandle ChooseChannel(const TArray<FMassifDebugView>& Views, int32 SelectedViewIndex);

	/**
	 * A square lattice of arrows centred on the point, scaled against the
	 * strongest reading on the lattice so the patch under the camera reads
	 * whatever the rest of the world holds.
	 */
	TArray<FMassifVectorArrow> BuildArrows(
		const FMassifEnvironmentState& Environment,
		FMassifChannelHandle Channel,
		FVector2d CentreMeters,
		double RadiusMeters,
		int32 ArrowsPerSide);

	/** Draws one frame of arrows around the camera. Does nothing while the overlay is off. */
	void Draw(
		UWorld* World,
		const FMassifHabitatField& Habitat,
		const TArray<FMassifDebugView>& Views,
		int32 SelectedViewIndex);
}
