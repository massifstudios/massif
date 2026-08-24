#pragma once

#include "CoreMinimal.h"

class FMassifRiverNetwork;
class UWorld;

/**
 * Draws the river network exactly as the extraction left it.
 *
 * A debug view of a channel shows where the water solve says water gathers; it
 * cannot show where the extraction then decided a river is, how far apart its
 * stations stand, how wide it made the channel, or whether the surface it solved
 * sits in the ground or over it. Those are four different ways for a river to be
 * wrong in the same place, and telling them apart is what this is for.
 *
 * So nothing here is smoothed, projected onto the terrain, or fitted to anything:
 * every line is drawn at the altitude the network itself holds. A centreline
 * hanging in the air is a surface solved above its own ground, and it looks like
 * one.
 *
 * Around the camera only. A world holds hundreds of kilometres of channel at six
 * metres a station, and all of it at once is a wall of lines rather than a view.
 */
namespace MassifDebugRiverOverlay
{
	/** Whether the overlay is drawn. Toggled by massif.Rivers.Overlay. */
	bool IsEnabled();

	/** Draws one frame of it. Does nothing while the overlay is off. */
	void Draw(UWorld* World, const FMassifRiverNetwork& Rivers);
}
