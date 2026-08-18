#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifWorldGenSmokeTest,
	"Massif.WorldGen.Smoke",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifWorldGenSmokeTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("MassifWorldGen module test harness runs"), true);
	return true;
}
