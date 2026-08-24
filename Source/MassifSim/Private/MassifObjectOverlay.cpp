#include "MassifObjectOverlay.h"

#include "MassifStateHash.h"

bool FMassifObjectOverlay::HasDeviationsInChunk(int32 ChunkColumn, int32 ChunkRow) const
{
	return DeviationCountByChunk.Contains(FMassifObjectIndex::MakeChunkKey(ChunkColumn, ChunkRow));
}

const FMassifObjectState* FMassifObjectOverlay::Find(FMassifObjectIndex Index) const
{
	if (!Index.IsValid())
	{
		return nullptr;
	}
	return Deviations.Find(Index.ToKey());
}

void FMassifObjectOverlay::Set(FMassifObjectIndex Index, const FMassifObjectState& State)
{
	checkf(Index.IsValid(), TEXT("An overlay entry has to name a record that exists."));

	const bool bIsNewEntry = Deviations.Find(Index.ToKey()) == nullptr;
	Deviations.Add(Index.ToKey(), State);
	if (bIsNewEntry)
	{
		DeviationCountByChunk.FindOrAdd(Index.ToChunkKey()) += 1;
	}
}

bool FMassifObjectOverlay::Remove(FMassifObjectIndex Index)
{
	if (!Index.IsValid() || Deviations.Remove(Index.ToKey()) == 0)
	{
		return false;
	}

	// The chunk key leaves with its last entry, or the fast path would keep
	// reporting deviations in a chunk that no longer has any.
	int32& ChunkCount = DeviationCountByChunk.FindChecked(Index.ToChunkKey());
	--ChunkCount;
	if (ChunkCount == 0)
	{
		DeviationCountByChunk.Remove(Index.ToChunkKey());
	}
	return true;
}

void FMassifObjectOverlay::Empty()
{
	Deviations.Empty();
	DeviationCountByChunk.Empty();
}

TArray<FMassifObjectIndex> FMassifObjectOverlay::GetIndicesInOrder() const
{
	TArray<int64> Keys;
	Deviations.GetKeys(Keys);
	Keys.Sort();

	TArray<FMassifObjectIndex> Indices;
	Indices.Reserve(Keys.Num());
	for (const int64 Key : Keys)
	{
		Indices.Add(FMassifObjectIndex::FromKey(Key));
	}
	return Indices;
}

uint64 FMassifObjectOverlay::ComputeContentHash() const
{
	// Key order, not map order: a TMap walks its entries in whatever order its
	// buckets ended up in, and two runs that played identically must still hash
	// the same.
	FMassifStateHash Hash;
	Hash.AppendInt(Deviations.Num());
	for (const FMassifObjectIndex& Index : GetIndicesInOrder())
	{
		const FMassifObjectState& State = Deviations.FindChecked(Index.ToKey());
		Hash.AppendInt(Index.ToKey());
		Hash.AppendInt(State.bRemoved ? 1 : 0);
		Hash.AppendInt(State.bFelled ? 1 : 0);
		Hash.AppendFloats({ State.FellDirectionDegrees, State.Vigour });
	}
	return Hash.Finalize();
}

void FMassifObjectOverlay::RebuildChunkCounts()
{
	DeviationCountByChunk.Empty();
	for (const TPair<int64, FMassifObjectState>& Entry : Deviations)
	{
		DeviationCountByChunk.FindOrAdd(FMassifObjectIndex::FromKey(Entry.Key).ToChunkKey()) += 1;
	}
}
