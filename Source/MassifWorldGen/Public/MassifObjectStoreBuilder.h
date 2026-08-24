#pragma once

#include "CoreMinimal.h"

class FMassifEnvironmentState;
class FMassifObjectStore;
class FMassifObjectTypeRegistry;

/**
 * Turns what the environment's world passes placed into the store that persists
 * it (OBJECT_STORE.md §5).
 *
 * This is the only seam where a point set becomes records. Both sides are
 * deliberately dumb about each other: a process writes placements in the units
 * it solved in and knows nothing about type ids, and the store knows nothing
 * about how a block came to rest where it did.
 */
namespace MassifObjectStoreBuilder
{
	/** Which object type each of a point set's classes becomes. */
	struct FPointSetMapping
	{
		FName PointSetName;

		/** One type id per class index the point set declares, in the same order. */
		TArray<uint16> TypeIdsByClass;
	};

	/**
	 * Appends one point set's objects. Returns how many were added; a class with
	 * no mapping, or one mapped to a type nothing registered, adds none.
	 *
	 * Does not finalize, so several sets can drain into the same store.
	 */
	MASSIFWORLDGEN_API int32 DrainPointSet(
		const FMassifEnvironmentState& Environment, const FPointSetMapping& Mapping,
		const FMassifObjectTypeRegistry& Registry, FMassifObjectStore& Store);

	/**
	 * Drains every point set the builtin mappings cover into an open store. A set
	 * with no mapping is skipped rather than guessed at.
	 *
	 * Does not finalize, because the environment's point sets are not the only
	 * producer: the tree pass runs off the carved terrain rather than inside the
	 * graph, so it plants into the same store afterwards.
	 */
	MASSIFWORLDGEN_API void FillFromEnvironment(
		const FMassifEnvironmentState& Environment, const FMassifObjectTypeRegistry& Registry,
		FMassifObjectStore& Store);

	/** A store holding nothing but the environment's own placements, closed to writes. */
	MASSIFWORLDGEN_API FMassifObjectStore BuildFromEnvironment(
		const FMassifEnvironmentState& Environment, const FMassifObjectTypeRegistry& Registry);

	/** The chunk grid a store over this world uses, so a caller can open one itself. */
	MASSIFWORLDGEN_API FMassifObjectStore MakeEmptyStore(const FMassifEnvironmentState& Environment);
}
