#include "MassifObjectRecord.h"
#include "MassifObjectTypeRegistry.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint16 TestBoulderTypeId = 900;
	constexpr uint16 TestTreeTypeId = 901;

	FMassifObjectTypeRegistry MakeTestRegistry()
	{
		FMassifObjectTypeRegistry Registry;

		FMassifObjectTypeInfo Boulder;
		Boulder.Name = TEXT("Test.Boulder");
		Boulder.Category = EMassifObjectCategory::Boulder;
		Boulder.SizeBandMeters = FVector2d(0.5, 4.0);
		Registry.Register(TestBoulderTypeId, Boulder);

		FMassifObjectTypeInfo Tree;
		Tree.Name = TEXT("Test.Tree");
		Tree.Category = EMassifObjectCategory::Tree;
		Tree.SizeBandMeters = FVector2d(0.1, 0.9);
		Tree.VariantCount = 4;
		Registry.Register(TestTreeTypeId, Tree);

		return Registry;
	}

	FMassifObjectPlacement MakeBoulderPlacement()
	{
		FMassifObjectPlacement Placement;
		Placement.PositionMeters = FVector3d(-9412.375, 6207.5, 2143.028);
		Placement.TypeId = TestBoulderTypeId;
		Placement.RadiusMeters = 1.75;
		Placement.Rotation = FRotator(21.0, 137.0, -64.0);
		Placement.BuriedFraction = 0.3;
		return Placement;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectRecordRoundTripTest,
	"Massif.Core.ObjectRecord.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectRecordRoundTripTest::RunTest(const FString& Parameters)
{
	const FMassifObjectTypeRegistry Registry = MakeTestRegistry();
	const FMassifObjectPlacement Written = MakeBoulderPlacement();

	FMassifObjectRecord Record;
	TestTrue(TEXT("A registered type encodes"), MassifObjectRecordCodec::Encode(Written, Registry, Record));

	FMassifObjectPlacement Read;
	TestTrue(TEXT("The record decodes"), MassifObjectRecordCodec::Decode(Record, Registry, Read));

	// Millimetres are the point of int32 positions: the error has to be the same
	// ten kilometres out as it is at the origin, which float metres would not be.
	TestTrue(FString::Printf(TEXT("Position survives to the millimetre (%s -> %s)"),
		*Written.PositionMeters.ToString(), *Read.PositionMeters.ToString()),
		FVector3d::Distance(Written.PositionMeters, Read.PositionMeters) < 0.001);

	// One step of the size byte over a 3.5 m band.
	TestTrue(TEXT("Radius survives its band's quantisation"),
		FMath::Abs(Read.RadiusMeters - Written.RadiusMeters) < 3.5 / 255.0);

	TestTrue(TEXT("Yaw survives at the resolution its two bytes give"),
		FMath::Abs(Read.Rotation.Yaw - Written.Rotation.Yaw) < 0.01);
	TestTrue(TEXT("Pitch and roll survive at the resolution their one byte gives"),
		FMath::Abs(FRotator::NormalizeAxis(Read.Rotation.Pitch - Written.Rotation.Pitch)) < 1.5
			&& FMath::Abs(FRotator::NormalizeAxis(Read.Rotation.Roll - Written.Rotation.Roll)) < 1.5);

	TestTrue(TEXT("Buried fraction survives"), FMath::Abs(Read.BuriedFraction - Written.BuriedFraction) < 0.005);
	TestEqual(TEXT("Type survives"), static_cast<int32>(Read.TypeId), static_cast<int32>(Written.TypeId));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectRecordTreeFieldsTest,
	"Massif.Core.ObjectRecord.TreeFields",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectRecordTreeFieldsTest::RunTest(const FString& Parameters)
{
	const FMassifObjectTypeRegistry Registry = MakeTestRegistry();

	FMassifObjectPlacement Written;
	Written.PositionMeters = FVector3d(120.0, -340.5, 1820.25);
	Written.TypeId = TestTreeTypeId;
	Written.RadiusMeters = 0.42;

	// Planted before the world started, which is what every generated tree is.
	Written.PlantedHalfMonths = -4800;
	Written.Vigour = 0.6;
	Written.VariantIndex = 3;
	Written.bDeadStanding = true;

	FMassifObjectRecord Record;
	TestTrue(TEXT("The tree encodes"), MassifObjectRecordCodec::Encode(Written, Registry, Record));

	FMassifObjectPlacement Read;
	TestTrue(TEXT("The tree decodes"), MassifObjectRecordCodec::Decode(Record, Registry, Read));

	TestEqual(TEXT("A planting date before the world survives its sign"),
		Read.PlantedHalfMonths, Written.PlantedHalfMonths);
	TestTrue(TEXT("Vigour survives"), FMath::Abs(Read.Vigour - Written.Vigour) < 0.005);
	TestEqual(TEXT("Variant survives the flag nibble"), Read.VariantIndex, Written.VariantIndex);
	TestTrue(TEXT("Dead standing survives"), Read.bDeadStanding);
	TestFalse(TEXT("Felled was not set by setting dead standing"), Read.bFelled);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectRecordLayoutTest,
	"Massif.Core.ObjectRecord.Layout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectRecordLayoutTest::RunTest(const FString& Parameters)
{
	// The record is hashed and written as bytes, so a padding hole would put
	// uninitialised memory into both. Two records built the same way field by
	// field have to compare equal as bytes for either to be sound.
	FMassifObjectRecord First;
	FMemory::Memset(&First, 0xAB, sizeof(FMassifObjectRecord));
	FMassifObjectRecord Second;
	FMemory::Memset(&Second, 0x00, sizeof(FMassifObjectRecord));

	const FMassifObjectTypeRegistry Registry = MakeTestRegistry();
	const FMassifObjectPlacement Placement = MakeBoulderPlacement();
	MassifObjectRecordCodec::Encode(Placement, Registry, First);
	MassifObjectRecordCodec::Encode(Placement, Registry, Second);

	TestEqual(TEXT("The record has no padding to leak"),
		FMemory::Memcmp(&First, &Second, sizeof(FMassifObjectRecord)), 0);
	TestEqual(TEXT("The record is 24 bytes"), static_cast<int32>(sizeof(FMassifObjectRecord)), 24);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectRecordUnknownTypeTest,
	"Massif.Core.ObjectRecord.UnknownType",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectRecordUnknownTypeTest::RunTest(const FString& Parameters)
{
	const FMassifObjectTypeRegistry Registry = MakeTestRegistry();

	FMassifObjectPlacement Placement = MakeBoulderPlacement();
	Placement.TypeId = 902;

	FMassifObjectRecord Record;
	TestFalse(TEXT("An unregistered type refuses to encode"),
		MassifObjectRecordCodec::Encode(Placement, Registry, Record));

	// A retired type leaves a hole in the id space, and a save written before it
	// was retired still holds records pointing at it.
	FMassifObjectRecord Retired;
	Retired.TypeId = 902;
	FMassifObjectPlacement Decoded;
	TestFalse(TEXT("A record of a retired type refuses to decode"),
		MassifObjectRecordCodec::Decode(Retired, Registry, Decoded));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectTypeRegistryTest,
	"Massif.Core.ObjectRecord.Registry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectTypeRegistryTest::RunTest(const FString& Parameters)
{
	const FMassifObjectTypeRegistry Registry = MakeTestRegistry();

	TestEqual(TEXT("A name resolves to its id"),
		static_cast<int32>(Registry.FindTypeId(TEXT("Test.Tree"))), static_cast<int32>(TestTreeTypeId));
	TestEqual(TEXT("An unknown name resolves to nothing"),
		static_cast<int32>(Registry.FindTypeId(TEXT("Test.Absent"))),
		static_cast<int32>(MassifInvalidObjectTypeId));

	const TArray<uint16> TypeIds = Registry.GetRegisteredTypeIds();
	TestEqual(TEXT("Both test types are registered"), TypeIds.Num(), 2);
	TestTrue(TEXT("Ids come back in ascending order"), TypeIds[0] < TypeIds[1]);

	// The builtin table is what saves are written against, so it has to hold
	// together on its own rather than only when a test populates it.
	const FMassifObjectTypeRegistry& Builtin = FMassifObjectTypeRegistry::Get();
	const FMassifObjectTypeInfo* Boulder = Builtin.FindTypeInfo(MassifObjectTypes::GraniteBoulder);
	TestNotNull(TEXT("The builtin boulder type is registered"), Boulder);

	return true;
}
