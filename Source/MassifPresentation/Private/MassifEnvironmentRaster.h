#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentGraph.h"
#include "MassifEnvironmentState.h"

/**
 * The grid a world-wide presentation texture is rasterised on.
 *
 * The environment holds a grid per channel, but the surface map and the debug
 * views are both sampled through the terrain material's one world-to-UV
 * transform, so they have to agree on the extent they cover. The hydrology
 * channels are the finest the graph solves, and wetness is what a map of the
 * whole world is sized against.
 */
namespace MassifEnvironmentRaster
{
	inline const FMassifChannelGrid& GetGrid(const FMassifEnvironmentState& Environment)
	{
		return Environment.GetGrid(Environment.FindChannel(MassifEnvironmentChannels::Wetness()));
	}
}
