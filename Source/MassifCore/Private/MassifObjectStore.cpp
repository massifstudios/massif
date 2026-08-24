#include "MassifObjectStore.h"

#include "MassifCustomVersion.h"
#include "MassifObjectTypeRegistry.h"
#include "MassifStateHash.h"
#include "Serialization/Archive.h"

namespace
{
	/**
	 * Total order over records, so a chunk sorts the same way whatever order its
	 * records arrived in. Position first because that is what makes the ordering
	 * spatially coherent, and type last only to break the tie of two objects the
	 * solve put in exactly the same place.
	 */
	bool RecordSortsBefore(const FMassifObjectRecord& Left, const FMassifObjectRecord& Right)
	{
		if (Left.XMillimeters != Right.XMillimeters)
		{
			return Left.XMillimeters < Right.XMillimeters;
		}
		if (Left.YMillimeters != Right.YMillimeters)
		{
			return Left.YMillimeters < Right.YMillimeters;
		}
		if (Left.ZMillimeters != Right.ZMillimeters)
		{
			return Left.ZMillimeters < Right.ZMillimeters;
		}
		if (Left.TypeId != Right.TypeId)
		{
			return Left.TypeId < Right.TypeId;
		}
		return Left.Size < Right.Size;
	}

	void SerializeChunkPayload(FArchive& Ar, TArray<FMassifObjectRecord>& Chunk, int32 Count)
	{
		if (Ar.IsLoading())
		{
			Chunk.SetNumUninitialized(Count);
		}
		if (Count == 0)
		{
			return;
		}

		// One block rather than field by field: the record is padding-free POD,
		// which is what the layout assertions in MassifObjectRecord.h defend. This
		// is little-endian on disk, as every platform the game targets is.
		Ar.Serialize(Chunk.GetData(), Count * sizeof(FMassifObjectRecord));
	}
}

FMassifObjectChunkGrid FMassifObjectChunkGrid::FromWorldSize(FVector2d WorldSizeMeters, double ChunkSizeMeters)
{
	FMassifObjectChunkGrid Grid;
	Grid.ChunkSizeMeters = FMath::Max(ChunkSizeMeters, UE_DOUBLE_KINDA_SMALL_NUMBER);
	Grid.WorldSizeXMeters = WorldSizeMeters.X;
	Grid.WorldSizeYMeters = WorldSizeMeters.Y;
	Grid.Width = FMath::Max(1, FMath::CeilToInt32(Grid.WorldSizeXMeters / Grid.ChunkSizeMeters));
	Grid.Height = FMath::Max(1, FMath::CeilToInt32(Grid.WorldSizeYMeters / Grid.ChunkSizeMeters));
	return Grid;
}

int32 FMassifObjectChunkGrid::ChunkIndex(int32 Column, int32 Row) const
{
	if (Column < 0 || Column >= Width || Row < 0 || Row >= Height)
	{
		return INDEX_NONE;
	}
	return Row * Width + Column;
}

FIntPoint FMassifObjectChunkGrid::ContainingChunk(FVector2d PositionMeters) const
{
	const int32 Column = FMath::FloorToInt32((PositionMeters.X + 0.5 * WorldSizeXMeters) / ChunkSizeMeters);
	const int32 Row = FMath::FloorToInt32((0.5 * WorldSizeYMeters - PositionMeters.Y) / ChunkSizeMeters);
	return FIntPoint(FMath::Clamp(Column, 0, Width - 1), FMath::Clamp(Row, 0, Height - 1));
}

FVector2d FMassifObjectChunkGrid::ChunkCornerMeters(int32 Column, int32 Row) const
{
	return FVector2d(
		Column * ChunkSizeMeters - 0.5 * WorldSizeXMeters, 0.5 * WorldSizeYMeters - Row * ChunkSizeMeters);
}

bool FMassifObjectChunkGrid::operator==(const FMassifObjectChunkGrid& Other) const
{
	return Width == Other.Width && Height == Other.Height
		&& ChunkSizeMeters == Other.ChunkSizeMeters
		&& WorldSizeXMeters == Other.WorldSizeXMeters
		&& WorldSizeYMeters == Other.WorldSizeYMeters;
}

FMassifObjectStore::FMassifObjectStore(const FMassifObjectChunkGrid& InChunkGrid)
	: ChunkGrid(InChunkGrid)
{
	Chunks.SetNum(ChunkGrid.ChunkCount());
}

void FMassifObjectStore::Add(const FMassifObjectRecord& Record)
{
	checkf(!bFinalized, TEXT("The object store is closed to writes once it is finalized."));

	const FVector2d PositionMeters(
		MassifObjectRecordCodec::DecodeMillimeters(Record.XMillimeters),
		MassifObjectRecordCodec::DecodeMillimeters(Record.YMillimeters));
	const FIntPoint Chunk = ChunkGrid.ContainingChunk(PositionMeters);
	const int32 Index = ChunkGrid.ChunkIndex(Chunk.X, Chunk.Y);
	if (Index == INDEX_NONE)
	{
		return;
	}

	Chunks[Index].Add(Record);
	++RecordCount;
}

bool FMassifObjectStore::AddPlacement(
	const FMassifObjectPlacement& Placement, const FMassifObjectTypeRegistry& Registry)
{
	FMassifObjectRecord Record;
	if (!MassifObjectRecordCodec::Encode(Placement, Registry, Record))
	{
		return false;
	}

	Add(Record);
	return true;
}

void FMassifObjectStore::Finalize()
{
	for (TArray<FMassifObjectRecord>& Chunk : Chunks)
	{
		Chunk.Sort(RecordSortsBefore);
		Chunk.Shrink();
	}
	bFinalized = true;
}

TConstArrayView<FMassifObjectRecord> FMassifObjectStore::GetChunkRecords(int32 Column, int32 Row) const
{
	const int32 Index = ChunkGrid.ChunkIndex(Column, Row);
	if (Index == INDEX_NONE)
	{
		return TConstArrayView<FMassifObjectRecord>();
	}
	return Chunks[Index];
}

int64 FMassifObjectStore::GetResidentByteCount() const
{
	int64 ByteCount = Chunks.Num() * static_cast<int64>(sizeof(TArray<FMassifObjectRecord>));
	for (const TArray<FMassifObjectRecord>& Chunk : Chunks)
	{
		ByteCount += Chunk.Max() * static_cast<int64>(sizeof(FMassifObjectRecord));
	}
	return ByteCount;
}

uint64 FMassifObjectStore::ComputeContentHash() const
{
	checkf(bFinalized, TEXT("Hashing an unfinalized object store would hash the order it was filled in."));

	FMassifStateHash Hash;
	Hash.AppendInt(ChunkGrid.Width);
	Hash.AppendInt(ChunkGrid.Height);
	Hash.Append(&ChunkGrid.ChunkSizeMeters, sizeof(double));
	Hash.AppendInt(RecordCount);

	// Records are byte-comparable by construction — the layout assertions in
	// MassifObjectRecord.h exist to keep them that way — so a chunk hashes as one
	// block rather than field by field.
	for (const TArray<FMassifObjectRecord>& Chunk : Chunks)
	{
		Hash.AppendInt(Chunk.Num());
		if (Chunk.Num() > 0)
		{
			Hash.Append(Chunk.GetData(), Chunk.Num() * sizeof(FMassifObjectRecord));
		}
	}
	return Hash.Finalize();
}

void FMassifObjectStore::Empty()
{
	Chunks.Reset();
	Chunks.SetNum(ChunkGrid.ChunkCount());
	RecordCount = 0;
	bFinalized = false;
}

void FMassifObjectStore::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FMassifCustomVersion::GUID);

	if (Ar.IsSaving())
	{
		checkf(bFinalized, TEXT("An unfinalized object store would save indices the overlay cannot rely on."));
	}

	Ar << ChunkGrid.Width;
	Ar << ChunkGrid.Height;
	Ar << ChunkGrid.ChunkSizeMeters;
	Ar << ChunkGrid.WorldSizeXMeters;
	Ar << ChunkGrid.WorldSizeYMeters;

	if (Ar.IsLoading())
	{
		Chunks.Reset();
		Chunks.SetNum(ChunkGrid.ChunkCount());
		bFinalized = true;
	}

	// The counts are the chunk index: a chunk is Count fixed-size records, so
	// every payload offset follows from the counts before it and one chunk can be
	// read without the rest.
	TArray<int32> ChunkCounts;
	ChunkCounts.SetNumUninitialized(Chunks.Num());
	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		ChunkCounts[Index] = Chunks[Index].Num();
		Ar << ChunkCounts[Index];
	}

	RecordCount = 0;
	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		SerializeChunkPayload(Ar, Chunks[Index], ChunkCounts[Index]);
		RecordCount += ChunkCounts[Index];
	}
}
