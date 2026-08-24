#pragma once

#include "CoreMinimal.h"

/**
 * Where one record sits in the object store: which chunk, and where in that
 * chunk's array.
 *
 * This only names anything because the store is immutable. A record never moves
 * after Finalize and the chunk order is what was saved, so an index taken at
 * world generation still points at the same object after a save and a load
 * (OBJECT_STORE.md §7).
 */
struct FMassifObjectIndex
{
	int32 ChunkColumn = 0;
	int32 ChunkRow = 0;
	int32 RecordIndex = INDEX_NONE;

	bool IsValid() const
	{
		return RecordIndex >= 0 && ChunkColumn >= 0 && ChunkRow >= 0
			&& ChunkColumn <= MAX_uint16 && ChunkRow <= MAX_uint16;
	}

	/**
	 * Packed key, chunk in the high half so that ordering by key groups a chunk's
	 * objects together and sorts them by their position in it.
	 */
	int64 ToKey() const
	{
		checkSlow(IsValid());
		return static_cast<int64>(
			(static_cast<uint64>(static_cast<uint16>(ChunkColumn)) << 48)
			| (static_cast<uint64>(static_cast<uint16>(ChunkRow)) << 32)
			| static_cast<uint64>(static_cast<uint32>(RecordIndex)));
	}

	/** The chunk half alone, which is what a per-chunk lookup is keyed on. */
	int64 ToChunkKey() const
	{
		return static_cast<int64>(static_cast<uint64>(ToKey()) & 0xFFFFFFFF00000000ull);
	}

	static int64 MakeChunkKey(int32 ChunkColumn, int32 ChunkRow)
	{
		return FMassifObjectIndex{ ChunkColumn, ChunkRow, 0 }.ToChunkKey();
	}

	static FMassifObjectIndex FromKey(int64 Key)
	{
		const uint64 Unsigned = static_cast<uint64>(Key);

		FMassifObjectIndex Index;
		Index.ChunkColumn = static_cast<int32>((Unsigned >> 48) & 0xFFFF);
		Index.ChunkRow = static_cast<int32>((Unsigned >> 32) & 0xFFFF);
		Index.RecordIndex = static_cast<int32>(Unsigned & 0xFFFFFFFF);
		return Index;
	}

	bool operator==(const FMassifObjectIndex& Other) const
	{
		return ChunkColumn == Other.ChunkColumn && ChunkRow == Other.ChunkRow
			&& RecordIndex == Other.RecordIndex;
	}
};
