#pragma once

#include "CoreMinimal.h"

class FMassifHabitatField;
class FMassifObjectStore;
class FMassifObjectTypeRegistry;

/**
 * Plants every tree in the world, once, at generation.
 *
 * The other 44 species of the flora table keep the view-time scatter they have
 * always had. Trees leave it for two reasons (OBJECT_STORE.md §4):
 *
 * - **They are stored.** A player can fell a tree, and an object needs an
 *   identity before anything can be done to one. A scatter that re-derives its
 *   output every time the camera passes has nowhere to record that a particular
 *   tree is gone, and no way to keep a save from silently regrowing the forest
 *   when the flora table is next edited.
 * - **A forest is not a per-tile phenomenon.** Competition, stand structure,
 *   seed dispersal and avalanche-path gaps all reach past a tile edge, and a
 *   scatter that cannot see its neighbours cannot model any of them. None of
 *   that is implemented here yet; this pass is what makes it possible to.
 *
 * It is a whole-world pass rather than a process inside the environment graph,
 * which is where §4 puts it. The reason is ordering: habitat is read off the
 * *carved* terrain field, and the rivers it is carved with are traced out of the
 * solved environment, so a graph process would run before the channels exist and
 * plant trees down the middle of every river. `MassifExtractRivers` is a
 * whole-world step for the same reason and states it the same way.
 */
namespace MassifTreePass
{
	struct FResult
	{
		int32 PlantedCount = 0;
		double Seconds = 0.0;
	};

	/**
	 * Adds every tree to the store. The store must not be finalized yet.
	 *
	 * Deterministic: each species is planted from a seed derived from the master
	 * seed and its name, tile by tile over the whole world, and the store sorts
	 * each chunk into a canonical order afterwards — so the result does not depend
	 * on the order the tiles happened to complete in.
	 */
	FResult PlantWorld(
		const FMassifHabitatField& Habitat,
		uint32 MasterSeed,
		const FMassifObjectTypeRegistry& Registry,
		FMassifObjectStore& Store);
}
