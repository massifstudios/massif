#include "Algo/Reverse.h"
#include "MassifObjectRecord.h"
#include "MassifObjectStore.h"
#include "MassifObjectTypeRegistry.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
	constexpr double StoreTestWorldMeters = 4096.0;
	constexpr double StoreTestChunkMeters = 512.0;

	FMassifObjectChunkGrid MakeTestGrid()
	{
		return FMassifObjectChunkGrid::FromWorldSize(
			FVector2d(StoreTestWorldMeters, StoreTestWorldMeters), StoreTestChunkMeters);
	}

	FMassifObjectRecord MakeRecordAt(double XMeters, double YMeters, double ZMeters, uint8 Size)
	{
		FMassifObjectRecord Record;
		Record.XMillimeters = MassifObjectRecordCodec::EncodeMillimeters(XMeters);
		Record.YMillimeters = MassifObjectRecordCodec::EncodeMillimeters(YMeters);
		Record.ZMillimeters = MassifObjectRecordCodec::EncodeMillimeters(ZMeters);
		Record.TypeId = MassifObjectTypes::GraniteBoulder;
		Record.Size = Size;
		return Record;
	}

	/** A spread of records over the whole world, deterministic without touching a random stream. */
	TArray<FMassifObjectRecord> MakeScatteredRecords(int32 Count)
	{
		TArray<FMassifObjectRecord> Records;
		Records.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Angle = Index * 0.7391;
			const double Radius = 0.45 * StoreTestWorldMeters * (Index % 97) / 97.0;
			Records.Add(MakeRecordAt(
				Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle), 1000.0 + Index,
				static_cast<uint8>(Index % 251)));
		}
		return Records;
	}

	/** How many records a chunk holds, counting into OutMisplaced any that do not belong to it. */
	int32 CountChunkRecords(const FMassifObjectStore& Store, int32 Column, int32 Row, int32& OutMisplaced)
	{
		const FMassifObjectChunkGrid& Grid = Store.GetChunkGrid();
		const TConstArrayView<FMassifObjectRecord> Records = Store.GetChunkRecords(Column, Row);
		for (const FMassifObjectRecord& Record : Records)
		{
			const FVector2d PositionMeters(
				MassifObjectRecordCodec::DecodeMillimeters(Record.XMillimeters),
				MassifObjectRecordCodec::DecodeMillimeters(Record.YMillimeters));
			if (Grid.ContainingChunk(PositionMeters) != FIntPoint(Column, Row))
			{
				++OutMisplaced;
			}
		}
		return Records.Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectStoreChunkingTest,
	"Massif.Core.ObjectStore.Chunking",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectStoreChunkingTest::RunTest(const FString& Parameters)
{
	const FMassifObjectChunkGrid Grid = MakeTestGrid();
	TestEqual(TEXT("A 4096 m world cuts into 8 chunks a side"), Grid.Width, 8);

	FMassifObjectStore Store(Grid);
	const TArray<FMassifObjectRecord> Records = MakeScatteredRecords(2000);
	for (const FMassifObjectRecord& Record : Records)
	{
		Store.Add(Record);
	}
	Store.Finalize();

	TestEqual(TEXT("Every record was kept"), Store.GetRecordCount(), Records.Num());

	// A record in the wrong chunk is invisible to the reader that would draw it,
	// so every one has to land where its own position says it should.
	int32 Seen = 0;
	int32 Misplaced = 0;
	for (int32 Row = 0; Row < Grid.Height; ++Row)
	{
		for (int32 Column = 0; Column < Grid.Width; ++Column)
		{
			Seen += CountChunkRecords(Store, Column, Row, Misplaced);
		}
	}

	TestEqual(TEXT("Every record is reachable through its chunk"), Seen, Records.Num());
	TestEqual(TEXT("No record sits in a chunk its position does not fall in"), Misplaced, 0);

	TestEqual(TEXT("A chunk outside the grid reads as empty"),
		Store.GetChunkRecords(-1, 0).Num() + Store.GetChunkRecords(Grid.Width, 0).Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectStoreOrderTest,
	"Massif.Core.ObjectStore.Order",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectStoreOrderTest::RunTest(const FString& Parameters)
{
	TArray<FMassifObjectRecord> Records = MakeScatteredRecords(500);

	FMassifObjectStore Forwards(MakeTestGrid());
	for (const FMassifObjectRecord& Record : Records)
	{
		Forwards.Add(Record);
	}
	Forwards.Finalize();

	Algo::Reverse(Records);
	FMassifObjectStore Backwards(MakeTestGrid());
	for (const FMassifObjectRecord& Record : Records)
	{
		Backwards.Add(Record);
	}
	Backwards.Finalize();

	// The whole reason Finalize sorts: a world pass writing its output from a
	// pool of workers has no fixed order, and the save must not depend on it.
	TestEqual(TEXT("Fill order does not change the store's hash"),
		Forwards.ComputeContentHash(), Backwards.ComputeContentHash());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectStoreRoundTripTest,
	"Massif.Core.ObjectStore.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectStoreRoundTripTest::RunTest(const FString& Parameters)
{
	FMassifObjectStore Written(MakeTestGrid());
	for (const FMassifObjectRecord& Record : MakeScatteredRecords(3000))
	{
		Written.Add(Record);
	}
	Written.Finalize();

	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	Written.Serialize(Writer);

	FMemoryReader Reader(Bytes);
	FMassifObjectStore Read;
	Read.Serialize(Reader);

	TestTrue(TEXT("The grid survives the round trip"), Read.GetChunkGrid() == Written.GetChunkGrid());
	TestEqual(TEXT("Every record survives"), Read.GetRecordCount(), Written.GetRecordCount());
	TestEqual(TEXT("The store reads back identical"), Read.ComputeContentHash(), Written.ComputeContentHash());

	// The sim's overlay keys into the store by chunk and index, so a record that
	// loads at a different index than it saved at silently reassigns state.
	const TConstArrayView<FMassifObjectRecord> WrittenChunk = Written.GetChunkRecords(4, 4);
	const TConstArrayView<FMassifObjectRecord> ReadChunk = Read.GetChunkRecords(4, 4);
	TestEqual(TEXT("Chunk 4,4 holds the same count"), ReadChunk.Num(), WrittenChunk.Num());
	TestTrue(TEXT("Indices within a chunk are unchanged"),
		WrittenChunk.Num() > 0
			&& FMemory::Memcmp(ReadChunk.GetData(), WrittenChunk.GetData(),
				WrittenChunk.Num() * sizeof(FMassifObjectRecord)) == 0);

	// 24 bytes a record, one int32 a chunk for the index, and a grid header.
	const int64 PayloadBytes = Written.GetRecordCount() * static_cast<int64>(sizeof(FMassifObjectRecord));
	const int64 IndexBytes = Written.GetChunkGrid().ChunkCount() * static_cast<int64>(sizeof(int32));
	TestTrue(FString::Printf(TEXT("The file is its records plus a small index (%lld bytes for %lld of payload)"),
		static_cast<int64>(Bytes.Num()), PayloadBytes),
		Bytes.Num() <= PayloadBytes + IndexBytes + 128);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectStorePlacementTest,
	"Massif.Core.ObjectStore.Placement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectStorePlacementTest::RunTest(const FString& Parameters)
{
	const FMassifObjectTypeRegistry& Registry = FMassifObjectTypeRegistry::Get();

	FMassifObjectPlacement Placement;
	Placement.PositionMeters = FVector3d(1234.5, -678.25, 2100.0);
	Placement.TypeId = MassifObjectTypes::GraniteBoulder;
	Placement.RadiusMeters = 2.0;
	Placement.Rotation = FRotator(0.0, 45.0, 0.0);

	FMassifObjectStore Store(MakeTestGrid());
	TestTrue(TEXT("A placement of a builtin type is stored"), Store.AddPlacement(Placement, Registry));

	Placement.TypeId = 40000;
	TestFalse(TEXT("A placement of an unregistered type is refused"), Store.AddPlacement(Placement, Registry));
	TestEqual(TEXT("The refused placement was not stored"), Store.GetRecordCount(), 1);

	Store.Finalize();

	const FIntPoint Chunk = Store.GetChunkGrid().ContainingChunk(FVector2d(1234.5, -678.25));
	const TConstArrayView<FMassifObjectRecord> Records = Store.GetChunkRecords(Chunk.X, Chunk.Y);
	TestEqual(TEXT("The boulder is in the chunk its position falls in"), Records.Num(), 1);

	return true;
}
