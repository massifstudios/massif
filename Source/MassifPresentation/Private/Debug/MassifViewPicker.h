#pragma once

#include "CoreMinimal.h"

struct FMassifDebugView;

/**
 * Where the picker's entries and its slice scrubber sit on the screen.
 *
 * Kept apart from the drawing so that hit testing is a function of numbers
 * rather than of a canvas: which entry a click lands on is exactly the sort of
 * thing that is wrong by one and impossible to see, and this way it can be
 * tested without rendering anything.
 *
 * Entries run down a column and then into the next, because the list is as long
 * as the channel registry and a channel registered later must not push the last
 * one off the bottom of the screen.
 */
struct FMassifViewPickerLayout
{
	float Left = 0.0f;
	float Top = 0.0f;

	/** Width of one column. The panel is this times the column count. */
	float ColumnWidth = 300.0f;

	float RowHeight = 17.0f;

	/** Entries selectable in all: the one that turns the view off, then every view. */
	int32 EntryCount = 1;

	int32 RowsPerColumn = 1;
	int32 ColumnCount = 1;

	/** Slices the selected view holds. 1 draws no scrubber. */
	int32 TimeSliceCount = 1;

	float ScrubberHeight = 18.0f;
	float Padding = 10.0f;

	float PanelWidth() const;
	float TotalHeight() const;

	/** Top of the first entry, below the title. */
	float FirstRowTop() const;

	/** Top of the scrubber strip. Meaningless where the view holds one slice. */
	float ScrubberTop() const;
};

namespace MassifViewPicker
{
	/** Whether the overlay is up. Toggled by the key bound to it, and by massif.Env.Picker. */
	bool IsVisible();
	void SetVisible(bool bVisible);
	void Toggle();

	/** The slice the painted view is showing, shared with massif.Env.Slice. */
	int32 GetTimeSlice();
	void SetTimeSlice(int32 TimeSlice);

	FMassifViewPickerLayout MakeLayout(int32 ViewCount, int32 TimeSliceCount, float ScreenHeight);

	/** Where one entry is drawn, so the drawing and the hit testing cannot disagree. */
	FVector2D EntryPosition(const FMassifViewPickerLayout& Layout, int32 EntryIndex);

	/**
	 * View index under a point, or INDEX_NONE where the point is on no entry.
	 * Index -1 is the entry that turns the view off, so the result is offset the
	 * same way EMassifDebugChannel is: -1 is Off, 0 is the first view.
	 */
	int32 ViewIndexAt(const FMassifViewPickerLayout& Layout, FVector2D Point);

	/** Slice under a point on the scrubber, or INDEX_NONE where the point is off it. */
	int32 TimeSliceAt(const FMassifViewPickerLayout& Layout, FVector2D Point);

	/** Whether a point is anywhere on the panel, so a click on it does not fall through. */
	bool ContainsPoint(const FMassifViewPickerLayout& Layout, FVector2D Point);
}
