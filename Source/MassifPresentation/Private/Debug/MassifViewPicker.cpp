#include "MassifViewPicker.h"

#include "HAL/IConsoleManager.h"

namespace
{
	TAutoConsoleVariable<int32> CVarViewPicker(
		TEXT("massif.Env.Picker"), 0,
		TEXT("Show the debug view picker, which the key bound to it toggles."),
		ECVF_Cheat);

	TAutoConsoleVariable<int32> CVarViewSlice(
		TEXT("massif.Env.Slice"), 0,
		TEXT("Slice of the year the painted view shows, for channels that hold more than one."),
		ECVF_Cheat);

	/** Kept clear of the screen edges, so the panel never runs off the bottom. */
	constexpr float ScreenMargin = 24.0f;
}

float FMassifViewPickerLayout::PanelWidth() const
{
	return ColumnCount * ColumnWidth;
}

float FMassifViewPickerLayout::TotalHeight() const
{
	// One row for the title, then as deep as the longest column.
	const float RowsHeight = (1 + RowsPerColumn) * RowHeight;
	if (TimeSliceCount <= 1)
	{
		return RowsHeight + 2.0f * Padding;
	}
	return RowsHeight + ScrubberHeight + 3.0f * Padding;
}

float FMassifViewPickerLayout::FirstRowTop() const
{
	return Top + Padding + RowHeight;
}

float FMassifViewPickerLayout::ScrubberTop() const
{
	return FirstRowTop() + RowsPerColumn * RowHeight + Padding;
}

bool MassifViewPicker::IsVisible()
{
	return CVarViewPicker.GetValueOnGameThread() != 0;
}

void MassifViewPicker::SetVisible(bool bVisible)
{
	CVarViewPicker->Set(static_cast<int32>(bVisible), ECVF_SetByConsole);
}

void MassifViewPicker::Toggle()
{
	SetVisible(!IsVisible());
}

int32 MassifViewPicker::GetTimeSlice()
{
	return FMath::Max(0, CVarViewSlice.GetValueOnGameThread());
}

void MassifViewPicker::SetTimeSlice(int32 TimeSlice)
{
	CVarViewSlice->Set(FMath::Max(0, TimeSlice), ECVF_SetByConsole);
}

FMassifViewPickerLayout MassifViewPicker::MakeLayout(
	int32 ViewCount, int32 TimeSliceCount, float ScreenHeight)
{
	FMassifViewPickerLayout Layout;
	Layout.Left = ScreenMargin;
	Layout.Top = ScreenMargin;
	Layout.TimeSliceCount = TimeSliceCount;
	Layout.EntryCount = FMath::Max(1, ViewCount + 1);

	float Available = ScreenHeight - 2.0f * ScreenMargin - 2.0f * Layout.Padding - Layout.RowHeight;
	if (TimeSliceCount > 1)
	{
		Available -= Layout.ScrubberHeight + Layout.Padding;
	}

	const int32 RowsThatFit = FMath::Max(1, FMath::FloorToInt32(Available / Layout.RowHeight));
	Layout.RowsPerColumn = FMath::Min(RowsThatFit, Layout.EntryCount);
	Layout.ColumnCount = FMath::DivideAndRoundUp(Layout.EntryCount, Layout.RowsPerColumn);
	return Layout;
}

FVector2D MassifViewPicker::EntryPosition(const FMassifViewPickerLayout& Layout, int32 EntryIndex)
{
	const int32 Column = EntryIndex / Layout.RowsPerColumn;
	const int32 Row = EntryIndex % Layout.RowsPerColumn;
	return FVector2D(
		Layout.Left + Column * Layout.ColumnWidth, Layout.FirstRowTop() + Row * Layout.RowHeight);
}

int32 MassifViewPicker::ViewIndexAt(const FMassifViewPickerLayout& Layout, FVector2D Point)
{
	const int32 Column = FMath::FloorToInt32((Point.X - Layout.Left) / Layout.ColumnWidth);
	const int32 Row = FMath::FloorToInt32((Point.Y - Layout.FirstRowTop()) / Layout.RowHeight);
	const bool bOnGrid = Column >= 0 && Column < Layout.ColumnCount && Row >= 0 && Row < Layout.RowsPerColumn;
	if (!bOnGrid)
	{
		return INDEX_NONE;
	}

	const int32 EntryIndex = Column * Layout.RowsPerColumn + Row;
	if (EntryIndex >= Layout.EntryCount)
	{
		return INDEX_NONE;
	}

	// Entry 0 is the one that turns the view off, so the first real view is entry 1.
	return EntryIndex - 1;
}

int32 MassifViewPicker::TimeSliceAt(const FMassifViewPickerLayout& Layout, FVector2D Point)
{
	if (Layout.TimeSliceCount <= 1)
	{
		return INDEX_NONE;
	}

	const float Top = Layout.ScrubberTop();
	const bool bOnStrip = Point.X >= Layout.Left && Point.X <= Layout.Left + Layout.PanelWidth()
		&& Point.Y >= Top && Point.Y <= Top + Layout.ScrubberHeight;
	if (!bOnStrip)
	{
		return INDEX_NONE;
	}

	const double Fraction = (Point.X - Layout.Left) / FMath::Max(Layout.PanelWidth(), 1.0f);
	return FMath::Clamp(
		FMath::FloorToInt32(Fraction * Layout.TimeSliceCount), 0, Layout.TimeSliceCount - 1);
}

bool MassifViewPicker::ContainsPoint(const FMassifViewPickerLayout& Layout, FVector2D Point)
{
	return Point.X >= Layout.Left && Point.X <= Layout.Left + Layout.PanelWidth()
		&& Point.Y >= Layout.Top && Point.Y <= Layout.Top + Layout.TotalHeight();
}
