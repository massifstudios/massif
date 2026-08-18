#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifSimSmokeTest,
	"Massif.Sim.Smoke",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifSimSmokeTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("MassifSim module test harness runs"), true);
	return true;
}
