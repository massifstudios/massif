#include "HAL/FileManager.h"
#include "MassifObjectRecord.h"
#include "MassifObjectTypeRegistry.h"
#include "MassifWorldSave.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
	constexpr double SaveTestWorldMeters = 2048.0;

	FString MakeTestSavePath(const FString& SaveName)
	{
		return FPaths::ProjectSavedDir() / TEXT("AutomationTemp") / SaveName + TEXT(".massifsave");
	}

	FMassifObjectStore MakeTestStore(int32 RecordCount)
	{
		FMassifObjectStore Store(FMassifObjectChunkGrid::FromWorldSize(
			FVector2d(SaveTestWorldMeters, SaveTestWorldMeters)));

		for (int32 Index = 0; Index < RecordCount; ++Index)
		{
			const double Angle = Index * 0.7391;
			const double Radius = 0.4 * SaveTestWorldMeters * (Index % 89) / 89.0;

			FMassifObjectPlacement Placement;
			Placement.PositionMeters =
				FVector3d(Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle), 1500.0 + Index * 0.25);
			Placement.TypeId = MassifObjectTypes::GraniteBoulder;
			Placement.RadiusMeters = 0.5 + 3.5 * (Index % 71) / 71.0;
			Placement.Rotation = FRotator(Index % 90, Index % 360, Index % 180);
			Store.AddPlacement(Placement, FMassifObjectTypeRegistry::Get());
		}

		Store.Finalize();
		return Store;
	}

	FMassifWorldSave MakeTestSave(int32 RecordCount)
	{
		FMassifWorldSave Save;
		Save.Header.MasterSeed = 1337;
		Save.Header.ElevationModelName = TEXT("oetztal");
		Save.Header.ElevationModelHash = 0x0123456789ABCDEFull;
		Save.Header.EngineVersion = TEXT("5.8.1");
		Save.Header.SavedAtUtc = FDateTime(2026, 8, 24, 1, 30, 0);

		MassifResetSimState(Save.SimState, Save.Header.MasterSeed);
		Save.SimState.TickCount = 4096;
		Save.SimState.ClockOffsetTicks = -77;

		FMassifObjectState Felled;
		Felled.bFelled = true;
		Felled.FellDirectionDegrees = 137.5f;
		Felled.Vigour = 0.0f;
		Save.SimState.ObjectOverlay.Set({ 1, 2, 3 }, Felled);

		FMassifObjectState Blasted;
		Blasted.bRemoved = true;
		Save.SimState.ObjectOverlay.Set({ 1, 2, 40 }, Blasted);

		Save.ObjectStore = MakeTestStore(RecordCount);
		return Save;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifWorldSaveRoundTripTest,
	"Massif.Sim.WorldSave.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifWorldSaveRoundTripTest::RunTest(const FString& Parameters)
{
	const FString FilePath = MakeTestSavePath(TEXT("RoundTrip"));
	FMassifWorldSave Written = MakeTestSave(4000);

	FString Error;
	TestTrue(FString::Printf(TEXT("The save writes (%s)"), *Error),
		MassifWriteWorldSave(FilePath, Written, Error));

	FMassifWorldSave Read;
	TestTrue(FString::Printf(TEXT("The save reads (%s)"), *Error),
		MassifReadWorldSave(FilePath, Read, Error));

	TestEqual(TEXT("The seed survives"), Read.Header.MasterSeed, Written.Header.MasterSeed);
	TestEqual(TEXT("The elevation model survives"),
		Read.Header.ElevationModelName, Written.Header.ElevationModelName);
	TestEqual(TEXT("Its hash survives"), Read.Header.ElevationModelHash, Written.Header.ElevationModelHash);

	// The sim state goes through the struct's reflection rather than field by
	// field, so this is what proves the reflected path actually round-trips.
	TestEqual(TEXT("The sim state survives"),
		MassifComputeSimStateHash(Read.SimState), MassifComputeSimStateHash(Written.SimState));
	TestEqual(TEXT("The tick count survives"), Read.SimState.TickCount, Written.SimState.TickCount);

	const FMassifObjectState* Felled = Read.SimState.ObjectOverlay.Find({ 1, 2, 3 });
	TestNotNull(TEXT("The felled tree survives"), Felled);
	TestTrue(TEXT("Its felling direction survives"),
		Felled != nullptr && FMath::IsNearlyEqual(Felled->FellDirectionDegrees, 137.5f, 0.001f));

	// The chunk counts are rebuilt on load rather than trusted from the file.
	TestTrue(TEXT("The overlay's fast path works after loading"),
		Read.SimState.ObjectOverlay.HasDeviationsInChunk(1, 2));

	TestEqual(TEXT("Every object survives"),
		Read.ObjectStore.GetRecordCount(), Written.ObjectStore.GetRecordCount());
	TestEqual(TEXT("The store reads back identical"),
		Read.ObjectStore.ComputeContentHash(), Written.ObjectStore.ComputeContentHash());

	IFileManager::Get().Delete(*FilePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifWorldSaveHeaderTest,
	"Massif.Sim.WorldSave.HeaderOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifWorldSaveHeaderTest::RunTest(const FString& Parameters)
{
	const FString FilePath = MakeTestSavePath(TEXT("HeaderOnly"));
	FMassifWorldSave Written = MakeTestSave(20000);

	FString Error;
	MassifWriteWorldSave(FilePath, Written, Error);

	// A load screen lists saves; reading a hundred megabytes of objects for each
	// row is the difference between a menu and a loading pause.
	FMassifWorldSaveHeader Header;
	TestTrue(FString::Printf(TEXT("The header reads alone (%s)"), *Error),
		MassifReadWorldSaveHeader(FilePath, Header, Error));
	TestEqual(TEXT("It is the header that was written"), Header.MasterSeed, Written.Header.MasterSeed);
	TestEqual(TEXT("The timestamp survives"), Header.SavedAtUtc, Written.Header.SavedAtUtc);

	// The store dominates the file: 24 bytes an object and little else.
	const int64 FileBytes = IFileManager::Get().FileSize(*FilePath);
	const int64 PayloadBytes = Written.ObjectStore.GetRecordCount() * 24;
	TestTrue(FString::Printf(TEXT("The file is its objects plus a little (%lld bytes for %lld of objects)"),
		FileBytes, PayloadBytes),
		FileBytes > PayloadBytes && FileBytes < PayloadBytes + 8192);

	IFileManager::Get().Delete(*FilePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifWorldSaveRefusalTest,
	"Massif.Sim.WorldSave.RefusesMismatches",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifWorldSaveRefusalTest::RunTest(const FString& Parameters)
{
	const FMassifWorldSave Save = MakeTestSave(10);

	FString Error;
	TestTrue(TEXT("The model it was built on is accepted"),
		MassifWorldSaveMatchesElevation(Save.Header, TEXT("oetztal"), Save.Header.ElevationModelHash, Error));

	// ARCHITECTURE §9: no heightfield is saved, so a changed model would put the
	// resort on different ground. That is refused, not warned about.
	TestFalse(TEXT("A different model is refused"),
		MassifWorldSaveMatchesElevation(Save.Header, TEXT("stubai"), Save.Header.ElevationModelHash, Error));
	TestFalse(TEXT("The same model with a changed hash is refused"),
		MassifWorldSaveMatchesElevation(Save.Header, TEXT("oetztal"), 12345, Error));
	TestTrue(TEXT("The refusal says why"), Error.Contains(TEXT("changed")));

	FMassifWorldSave Read;
	TestFalse(TEXT("A file that is not a save is refused"),
		MassifReadWorldSave(MakeTestSavePath(TEXT("DoesNotExist")), Read, Error));

	return true;
}

namespace
{
	/** A block of known bytes, standing in for whatever a module with a bake writes. */
	class FCountingSection : public IMassifSaveSection
	{
	public:
		explicit FCountingSection(int32 InByteCount)
			: ByteCount(InByteCount)
		{
		}

		virtual bool Serialize(FArchive& Ar, FString& OutError) override
		{
			++SerializeCount;
			Ar << ByteCount;

			Payload.SetNum(ByteCount);
			for (int32 Index = 0; Index < ByteCount; ++Index)
			{
				Payload[Index] = static_cast<uint8>(Index % 251);
			}
			Ar.Serialize(Payload.GetData(), ByteCount);
			return true;
		}

		int32 ByteCount = 0;
		int32 SerializeCount = 0;
		TArray<uint8> Payload;
	};

	/** A section that gives up part-way, to prove the reader still lands on the end of the block. */
	class FRefusingSection : public IMassifSaveSection
	{
	public:
		virtual bool Serialize(FArchive& Ar, FString& OutError) override
		{
			int32 ByteCount = 0;
			Ar << ByteCount;
			OutError = TEXT("Refused on purpose.");
			return false;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifWorldSaveSectionTest,
	"Massif.Sim.WorldSave.CarriesASection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifWorldSaveSectionTest::RunTest(const FString& Parameters)
{
	// The solved environment is two orders of magnitude larger than the rest of the
	// save and belongs to a module this one does not depend on, so it travels as a
	// block behind its own length. What that length buys is everything below: a
	// reader that wants none of it, and one that refuses what it finds, both end up
	// at the end of the file rather than inside the block.
	const FString FilePath = MakeTestSavePath(TEXT("Section"));
	const int32 SectionByteCount = 4096;

	FMassifWorldSave Written = MakeTestSave(64);
	const TSharedRef<FCountingSection> WrittenSection = MakeShared<FCountingSection>(SectionByteCount);
	Written.Environment = WrittenSection;

	FString Error;
	if (!TestTrue(TEXT("A save with a section is written"),
		MassifWriteWorldSave(FilePath, Written, Error)))
	{
		AddError(Error);
		return false;
	}

	FMassifWorldSave Read;
	const TSharedRef<FCountingSection> ReadSection = MakeShared<FCountingSection>(0);
	Read.Environment = ReadSection;
	TestTrue(TEXT("It is read back"), MassifReadWorldSave(FilePath, Read, Error));
	TestEqual(TEXT("The section read its block"), ReadSection->ByteCount, SectionByteCount);
	TestEqual(TEXT("The payload came back whole"), ReadSection->Payload, WrittenSection->Payload);
	TestTrue(TEXT("Nothing was reported wrong"), Read.EnvironmentError.IsEmpty());

	// What a save list does, and what massif.RebuildEnvironment does: read the save
	// and step over the bake without ever decoding it.
	FMassifWorldSave Skipped;
	TestTrue(TEXT("A reader with no section still reads the save"),
		MassifReadWorldSave(FilePath, Skipped, Error));
	TestEqual(TEXT("It got every object"),
		Skipped.ObjectStore.GetRecordCount(), Written.ObjectStore.GetRecordCount());
	TestEqual(TEXT("It got the sim state"), Skipped.SimState.TickCount, Written.SimState.TickCount);

	FMassifWorldSave Refused;
	Refused.Environment = MakeShared<FRefusingSection>();
	TestTrue(TEXT("A save whose section refuses its block still loads"),
		MassifReadWorldSave(FilePath, Refused, Error));
	TestEqual(TEXT("The rest of it is intact"),
		Refused.ObjectStore.GetRecordCount(), Written.ObjectStore.GetRecordCount());
	TestTrue(TEXT("The refusal is reported"), Refused.EnvironmentError.Contains(TEXT("purpose")));

	IFileManager::Get().Delete(*FilePath);
	return true;
}
