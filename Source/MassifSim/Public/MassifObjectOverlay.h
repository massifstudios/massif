#pragma once

#include "CoreMinimal.h"
#include "MassifObjectIndex.h"
#include "MassifObjectOverlay.generated.h"

/**
 * What play has done to one stored object.
 *
 * An entry is authoritative for every field it carries: a reader takes the
 * record, and where an entry exists it takes these values instead. Objects the
 * player has never touched have no entry at all, which is nearly all of them.
 */
USTRUCT()
struct MASSIFSIM_API FMassifObjectState
{
	GENERATED_BODY()

	/** Gone from the world: felled and hauled out, blasted, cleared for a piste. */
	UPROPERTY()
	bool bRemoved = false;

	UPROPERTY()
	bool bFelled = false;

	/** Which way it went down. Only meaningful when felled, and not derivable — the cut chose it. */
	UPROPERTY()
	float FellDirectionDegrees = 0.0f;

	/** 1 healthy, 0 dead. */
	UPROPERTY()
	float Vigour = 1.0f;
};

/**
 * The sparse half of the object store: everything about a generated object that
 * changed after generation (OBJECT_STORE.md §7).
 *
 * This is sim state and lives in FMassifSimState. The store beside it is world
 * data that presentation reads directly, and it stays immutable precisely so
 * that this can exist — the indices here only mean anything while records do
 * not move.
 *
 * Sized for the fact that almost nothing deviates. A resort fells thousands of
 * trees out of millions, so entries carry full state rather than deltas, and
 * the per-chunk counts let a reader skip a chunk in one lookup instead of
 * testing every instance in it.
 */
USTRUCT()
struct MASSIFSIM_API FMassifObjectOverlay
{
	GENERATED_BODY()

	/** False means no object in the chunk deviates, which is the common answer. */
	bool HasDeviationsInChunk(int32 ChunkColumn, int32 ChunkRow) const;

	/** Null where the object is still exactly what generation produced. */
	const FMassifObjectState* Find(FMassifObjectIndex Index) const;

	void Set(FMassifObjectIndex Index, const FMassifObjectState& State);

	/** True if there was an entry to drop. */
	bool Remove(FMassifObjectIndex Index);

	int32 Num() const
	{
		return Deviations.Num();
	}

	void Empty();

	/** Indices in key order, so hashing and iteration do not inherit the map's. */
	TArray<FMassifObjectIndex> GetIndicesInOrder() const;

	/** Over the entries in key order, for the determinism contract (ARCHITECTURE §4). */
	uint64 ComputeContentHash() const;

	/** Recomputes the per-chunk counts from the entries. For loaders that fill Deviations directly. */
	void RebuildChunkCounts();

private:
	/** Keyed by FMassifObjectIndex::ToKey. */
	UPROPERTY()
	TMap<int64, FMassifObjectState> Deviations;

	/** How many entries each chunk holds, keyed by FMassifObjectIndex::ToChunkKey. */
	UPROPERTY()
	TMap<int64, int32> DeviationCountByChunk;
};
