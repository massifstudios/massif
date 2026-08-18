#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifPresentationSmokeTest,
	"Massif.Presentation.Smoke",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifPresentationSmokeTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("MassifPresentation module test harness runs"), true);
	return true;
}
