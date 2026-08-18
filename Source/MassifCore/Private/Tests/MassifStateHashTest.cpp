#include "MassifStateHash.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifStateHashTest,
	"Massif.Core.StateHash",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifStateHashTest::RunTest(const FString& Parameters)
{
	const TArray<float> Values = { 1.0f, 2.5f, -3.75f };

	FMassifStateHash HashA;
	HashA.AppendFloats(Values);
	HashA.AppendInt(42);

	FMassifStateHash HashB;
	HashB.AppendFloats(Values);
	HashB.AppendInt(42);

	TestEqual(TEXT("Identical input produces identical hash"), HashA.Finalize(), HashB.Finalize());

	FMassifStateHash HashC;
	HashC.AppendFloats(Values);
	HashC.AppendInt(43);
	TestNotEqual(TEXT("Different input produces different hash"), HashA.Finalize(), HashC.Finalize());

	FMassifStateHash Empty;
	TestNotEqual(TEXT("Empty hash differs from non-empty hash"), Empty.Finalize(), HashA.Finalize());

	return true;
}
