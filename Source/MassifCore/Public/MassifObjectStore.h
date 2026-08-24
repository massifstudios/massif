#pragma once

#include "CoreMinimal.h"
#include "MassifObjectRecord.h"

class FMassifObjectTypeRegistry;

/**
 * How the store is cut into chunks.
 *
 * Same convention as the environment's channel grid — origin at the world
 * centre, column 0 west, row 0 north — but declared here rather than shared,
 * because MassifCore is below MassifWorldGen and a save format must not depend
 * on the generator that filled it.
 */
struct MASSIFCORE_API FMassifObjectChunkGrid
{
	int32 Width = 0;
	int32 Height = 0;
	double ChunkSizeMeters = 512.0;
	double WorldSizeXMeters = 0.0;
	double WorldSizeYMeters = 0.0;

	static FMassifObjectChunkGrid FromWorldSize(FVector2d WorldSizeMeters, double ChunkSizeMeters = 512.0);

	int32 ChunkCount() const
	{
		return Width * Height;
	}

	/** INDEX_NONE outside the grid. */
	int32 ChunkIndex(int32 Column, int32 Row) const;

	/** Chunk a world position falls in, clamped to the grid. */
	FIntPoint ContainingChunk(FVector2d PositionMeters) const;

	/** North-west corner of a chunk. */
	FVector2d ChunkCornerMeters(int32 Column, int32 Row) const;

	FVector2d ChunkCentreMeters(int32 Column, int32 Row) const;

	bool operator==(const FMassifObjectChunkGrid& Other) const;

	bool operator!=(const FMassifObjectChunkGrid& Other) const
	{
		return !(*this == Other);
	}
};

/**
 * Every generated object in the world, cut into chunks and written once.
 *
 * The store is what a save persists and what draws the world back, so two rules
 * hold it together (OBJECT_STORE.md §5, §7):
 *
 * - **Nothing is generated per chunk.** Both stored tiers come from world
 *   passes, and the store is filled by draining their placements into it. A
 *   chunk is a unit of storage and streaming, never of decision.
 * - **Nothing mutates a record after Finalize.** State that changes during play
 *   lives in the sim's sparse overlay, which keys into this by chunk and index —
 *   and those indices are only stable because the records never move again.
 */
class MASSIFCORE_API FMassifObjectStore
{
public:
	FMassifObjectStore() = default;
	explicit FMassifObjectStore(const FMassifObjectChunkGrid& InChunkGrid);

	const FMassifObjectChunkGrid& GetChunkGrid() const
	{
		return ChunkGrid;
	}

	/** Routes the record to the chunk its position falls in. Positions outside the world clamp in. */
	void Add(const FMassifObjectRecord& Record);

	/** False if the placement's type is not registered, in which case nothing is added. */
	bool AddPlacement(const FMassifObjectPlacement& Placement, const FMassifObjectTypeRegistry& Registry);

	/**
	 * Sorts every chunk into one canonical order and closes the store to writes.
	 *
	 * Two things need this. A hash over the store has to be the same whichever
	 * order a producer happened to walk its output in, and a chunk read off disk
	 * has to give the sim's overlay the same indices it was saved with.
	 */
	void Finalize();

	bool IsFinalized() const
	{
		return bFinalized;
	}

	/** Empty outside the grid, which is not an error. */
	TConstArrayView<FMassifObjectRecord> GetChunkRecords(int32 Column, int32 Row) const;

	int32 GetRecordCount() const
	{
		return RecordCount;
	}

	int64 GetResidentByteCount() const;

	/** Over the finalized records, so it does not depend on the order they arrived in. */
	uint64 ComputeContentHash() const;

	void Empty();

	/** Chunk counts precede the payloads, so a reader can seek to one chunk without the rest. */
	void Serialize(FArchive& Ar);

private:
	FMassifObjectChunkGrid ChunkGrid;

	/** One array per chunk, indexed as the grid indexes. */
	TArray<TArray<FMassifObjectRecord>> Chunks;

	int32 RecordCount = 0;
	bool bFinalized = false;
};
