#include "Debug/MassifViewPicker.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr float PickerTestScreenHeight = 1080.0f;

	FMassifViewPickerLayout MakePickerLayout(int32 ViewCount, int32 TimeSliceCount)
	{
		return MassifViewPicker::MakeLayout(ViewCount, TimeSliceCount, PickerTestScreenHeight);
	}

	/** Middle of the entry a view is drawn on, which is what a click on it would land at. */
	FVector2D PointOnViewRow(const FMassifViewPickerLayout& Layout, int32 ViewIndex)
	{
		const FVector2D Position = MassifViewPicker::EntryPosition(Layout, ViewIndex + 1);
		return Position + FVector2D(0.5f * Layout.ColumnWidth, 0.5f * Layout.RowHeight);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewPickerRowsTest,
	"Massif.Presentation.ViewPicker.Rows",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FViewPickerRowsTest::RunTest(const FString& Parameters)
{
	// Every row has to answer with the view drawn on it. A picker off by one row
	// selects the neighbour of whatever was clicked, which reads as the views being
	// mislabelled rather than as the picker being wrong.
	const FMassifViewPickerLayout Layout = MakePickerLayout(26, 1);

	int32 Mismatches = 0;
	for (int32 ViewIndex = 0; ViewIndex < 26; ++ViewIndex)
	{
		Mismatches += static_cast<int32>(
			MassifViewPicker::ViewIndexAt(Layout, PointOnViewRow(Layout, ViewIndex)) != ViewIndex);
	}
	TestEqual(TEXT("Every row answers with the view on it"), Mismatches, 0);

	TestEqual(TEXT("The row above the first view turns the view off"),
		MassifViewPicker::ViewIndexAt(Layout, PointOnViewRow(Layout, -1)), -1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewPickerBoundsTest,
	"Massif.Presentation.ViewPicker.Bounds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FViewPickerBoundsTest::RunTest(const FString& Parameters)
{
	const FMassifViewPickerLayout Layout = MakePickerLayout(26, 1);

	TestEqual(TEXT("A point left of the panel hits nothing"),
		MassifViewPicker::ViewIndexAt(Layout, FVector2D(Layout.Left - 5.0f, PointOnViewRow(Layout, 0).Y)),
		static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("A point right of the panel hits nothing"),
		MassifViewPicker::ViewIndexAt(
			Layout, FVector2D(Layout.Left + Layout.PanelWidth() + 5.0f, PointOnViewRow(Layout, 0).Y)),
		static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("A point above the first row hits nothing"),
		MassifViewPicker::ViewIndexAt(Layout, FVector2D(Layout.Left + 10.0f, Layout.Top + 1.0f)),
		static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("A point past the last row hits nothing"),
		MassifViewPicker::ViewIndexAt(Layout, PointOnViewRow(Layout, 26)),
		static_cast<int32>(INDEX_NONE));

	// The panel has to swallow clicks that land on it but on no row, or a click in
	// its margin would fall through to whatever is behind it.
	TestTrue(TEXT("The panel contains its own top-left corner"),
		MassifViewPicker::ContainsPoint(Layout, FVector2D(Layout.Left + 1.0f, Layout.Top + 1.0f)));
	TestFalse(TEXT("And nothing outside it"),
		MassifViewPicker::ContainsPoint(Layout, FVector2D(Layout.Left - 1.0f, Layout.Top + 1.0f)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewPickerScrubberTest,
	"Massif.Presentation.ViewPicker.Scrubber",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FViewPickerScrubberTest::RunTest(const FString& Parameters)
{
	const FMassifViewPickerLayout Layout = MakePickerLayout(26, 24);
	const float ScrubberY = Layout.ScrubberTop() + 0.5f * Layout.ScrubberHeight;
	const float TickWidth = Layout.PanelWidth() / 24.0f;

	int32 Mismatches = 0;
	for (int32 Slice = 0; Slice < 24; ++Slice)
	{
		const FVector2D Point(Layout.Left + (Slice + 0.5f) * TickWidth, ScrubberY);
		Mismatches += static_cast<int32>(MassifViewPicker::TimeSliceAt(Layout, Point) != Slice);
	}
	TestEqual(TEXT("Every tick of the strip answers with its own slice"), Mismatches, 0);

	TestEqual(TEXT("The far right edge stays inside the last slice"),
		MassifViewPicker::TimeSliceAt(Layout, FVector2D(Layout.Left + Layout.PanelWidth(), ScrubberY)), 23);
	TestEqual(TEXT("A point off the strip hits no slice"),
		MassifViewPicker::TimeSliceAt(Layout, FVector2D(Layout.Left + 10.0f, Layout.ScrubberTop() - 5.0f)),
		static_cast<int32>(INDEX_NONE));

	// A view holding one slice has no strip at all, so nothing on the panel may
	// answer as though it had been scrubbed.
	const FMassifViewPickerLayout Single = MakePickerLayout(26, 1);
	TestEqual(TEXT("A single-slice view offers no scrubber"),
		MassifViewPicker::TimeSliceAt(
			Single, FVector2D(Single.Left + 10.0f, Single.ScrubberTop() + 5.0f)),
		static_cast<int32>(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewPickerFitTest,
	"Massif.Presentation.ViewPicker.Fit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FViewPickerFitTest::RunTest(const FString& Parameters)
{
	// The list is as long as the channel registry, so a channel registered later
	// must not push the last entry off the bottom of the screen.
	const int32 ViewCounts[] = {5, 26, 60, 120};
	for (int32 ViewCount : ViewCounts)
	{
		const FMassifViewPickerLayout Layout = MakePickerLayout(ViewCount, 24);
		TestTrue(
			FString::Printf(TEXT("%d views fit the screen (%.0f of %.0f)"),
				ViewCount, Layout.Top + Layout.TotalHeight(), PickerTestScreenHeight),
			Layout.Top + Layout.TotalHeight() <= PickerTestScreenHeight);

		TestEqual(
			FString::Printf(TEXT("The last of %d views is still reachable"), ViewCount),
			MassifViewPicker::ViewIndexAt(Layout, PointOnViewRow(Layout, ViewCount - 1)), ViewCount - 1);
	}
	return true;
}
