#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifCoreSmokeTest,
	"Massif.Core.Smoke",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifCoreSmokeTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("MassifCore module test harness runs"), true);
	return true;
}
