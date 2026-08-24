#include "MassifObjectOverlay.h"
#include "MassifSimState.h"
#include "Misc/AutomationTest.h"

namespace
{
	FMassifObjectState MakeFelled(float FellDirectionDegrees)
	{
		FMassifObjectState State;
		State.bFelled = true;
		State.FellDirectionDegrees = FellDirectionDegrees;
		State.Vigour = 0.0f;
		return State;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectIndexPackingTest,
	"Massif.Sim.ObjectOverlay.IndexPacking",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectIndexPackingTest::RunTest(const FString& Parameters)
{
	// The far corner of a world far larger than the one this game has, so the
	// packing is not being tested only where the bits are all zero.
	const FMassifObjectIndex Written{ 39, 37, 1234567 };
	const FMassifObjectIndex Read = FMassifObjectIndex::FromKey(Written.ToKey());
	TestTrue(TEXT("An index survives packing"), Read == Written);

	const FMassifObjectIndex Extreme{ 65535, 65535, MAX_int32 };
	TestTrue(TEXT("The widest index survives packing"),
		FMassifObjectIndex::FromKey(Extreme.ToKey()) == Extreme);

	// Two records in the same chunk have to share a chunk key, or the fast path
	// would answer for one of them and not the other.
	const FMassifObjectIndex Neighbour{ 39, 37, 9 };
	TestEqual(TEXT("Records in one chunk share a chunk key"),
		Neighbour.ToChunkKey(), Written.ToChunkKey());
	TestEqual(TEXT("The chunk key is the one a lookup builds"),
		FMassifObjectIndex::MakeChunkKey(39, 37), Written.ToChunkKey());
	TestNotEqual(TEXT("A different chunk has a different key"),
		FMassifObjectIndex::MakeChunkKey(39, 38), Written.ToChunkKey());

	// Chunk in the high bits, so sorting by key groups a chunk together.
	TestTrue(TEXT("Keys sort by chunk before record"),
		FMassifObjectIndex({ 1, 0, MAX_int32 }).ToKey() < FMassifObjectIndex({ 1, 1, 0 }).ToKey());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectOverlayEntriesTest,
	"Massif.Sim.ObjectOverlay.Entries",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectOverlayEntriesTest::RunTest(const FString& Parameters)
{
	FMassifObjectOverlay Overlay;
	TestFalse(TEXT("An untouched chunk reports nothing"), Overlay.HasDeviationsInChunk(4, 7));
	TestNull(TEXT("An untouched object reads as the record left it"),
		Overlay.Find({ 4, 7, 12 }));

	Overlay.Set({ 4, 7, 12 }, MakeFelled(210.0f));
	Overlay.Set({ 4, 7, 900 }, MakeFelled(30.0f));

	const FMassifObjectState* Felled = Overlay.Find({ 4, 7, 12 });
	TestNotNull(TEXT("A felled tree has an entry"), Felled);
	TestTrue(TEXT("The felling direction survives"),
		Felled != nullptr && FMath::IsNearlyEqual(Felled->FellDirectionDegrees, 210.0f));

	// The whole point of the per-chunk counts: a reader skips a chunk in one
	// lookup rather than testing every instance in it.
	TestTrue(TEXT("The chunk now reports deviations"), Overlay.HasDeviationsInChunk(4, 7));
	TestFalse(TEXT("Its neighbour still does not"), Overlay.HasDeviationsInChunk(5, 7));

	// Overwriting is not a second entry.
	Overlay.Set({ 4, 7, 12 }, MakeFelled(45.0f));
	TestEqual(TEXT("Re-setting an entry does not add one"), Overlay.Num(), 2);

	TestTrue(TEXT("An entry can be dropped"), Overlay.Remove({ 4, 7, 12 }));
	TestFalse(TEXT("Dropping it twice does nothing"), Overlay.Remove({ 4, 7, 12 }));
	TestTrue(TEXT("The chunk still has the other entry"), Overlay.HasDeviationsInChunk(4, 7));

	// A chunk key that outlived its last entry would make every reader in that
	// chunk walk the slow path forever.
	Overlay.Remove({ 4, 7, 900 });
	TestFalse(TEXT("The chunk key leaves with the last entry"), Overlay.HasDeviationsInChunk(4, 7));
	TestEqual(TEXT("The overlay is empty again"), Overlay.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectOverlayHashTest,
	"Massif.Sim.ObjectOverlay.Hash",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectOverlayHashTest::RunTest(const FString& Parameters)
{
	FMassifObjectOverlay Forwards;
	FMassifObjectOverlay Backwards;
	const int32 EntryCount = 200;
	for (int32 Index = 0; Index < EntryCount; ++Index)
	{
		Forwards.Set({ Index % 7, Index % 5, Index }, MakeFelled(Index * 1.5f));
		const int32 Mirrored = EntryCount - 1 - Index;
		Backwards.Set({ Mirrored % 7, Mirrored % 5, Mirrored }, MakeFelled(Mirrored * 1.5f));
	}

	// A TMap walks its buckets in whatever order insertion left them, and two
	// players who did the same things in a different order are the same state.
	TestEqual(TEXT("Insertion order does not change the hash"),
		Forwards.ComputeContentHash(), Backwards.ComputeContentHash());

	Backwards.Set({ 0, 0, 0 }, MakeFelled(1.0f));
	TestNotEqual(TEXT("A changed entry changes the hash"),
		Forwards.ComputeContentHash(), Backwards.ComputeContentHash());

	// The overlay is sim state, so it has to reach the determinism contract.
	FMassifSimState WithOverlay;
	MassifResetSimState(WithOverlay, 1337);
	const uint64 EmptyHash = MassifComputeSimStateHash(WithOverlay);
	WithOverlay.ObjectOverlay.Set({ 2, 3, 4 }, MakeFelled(90.0f));
	TestNotEqual(TEXT("The overlay reaches the sim state hash"),
		MassifComputeSimStateHash(WithOverlay), EmptyHash);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectOverlayRebuildTest,
	"Massif.Sim.ObjectOverlay.RebuildChunkCounts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectOverlayRebuildTest::RunTest(const FString& Parameters)
{
	FMassifObjectOverlay Overlay;
	Overlay.Set({ 1, 1, 0 }, MakeFelled(0.0f));
	Overlay.Set({ 1, 1, 1 }, MakeFelled(0.0f));
	Overlay.Set({ 2, 9, 0 }, MakeFelled(0.0f));

	// Rebuilding has to land exactly where Set left things, or a loaded save
	// would answer the fast path differently from the session that wrote it.
	Overlay.RebuildChunkCounts();
	TestTrue(TEXT("A rebuilt count still finds the first chunk"), Overlay.HasDeviationsInChunk(1, 1));
	TestTrue(TEXT("And the second"), Overlay.HasDeviationsInChunk(2, 9));
	TestFalse(TEXT("And invents no third"), Overlay.HasDeviationsInChunk(0, 0));

	Overlay.Remove({ 1, 1, 0 });
	Overlay.Remove({ 1, 1, 1 });
	TestFalse(TEXT("Removal after a rebuild still empties the chunk"), Overlay.HasDeviationsInChunk(1, 1));

	return true;
}
