#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "MassifDebugHud.generated.h"

class AMassifDebugChannelView;
struct FMassifDebugChannelLegend;
struct FMassifInspectorSnapshot;
struct FMassifProbeResult;

/**
 * Draws the legend for the active debug channel view in the top-right corner,
 * the environment process graph down the left when massif.Env.Inspector is on,
 * and the last massif.Env.Probe reading at the bottom: which view is on, what it
 * shows, how its numbers are distributed, and what the world holds at one point.
 *
 * The legend reads its colours from the same ramp the texture was built from,
 * so it cannot drift out of step with what is painted on the terrain.
 */
UCLASS()
class AMassifDebugHud : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	AMassifDebugChannelView* FindChannelView();

	void DrawChannelLegend();
	void DrawPanel(const FMassifDebugChannelLegend& Legend, int32 ViewIndex, float PanelHeight);
	void DrawContinuousLegend(const FMassifDebugChannelLegend& Legend, float PanelLeft, float RowTop);
	void DrawClassifiedLegend(const FMassifDebugChannelLegend& Legend, float PanelLeft, float RowTop);

	/** Min, mean and max, and the histogram that catches the everything-is-0.03 bug. */
	void DrawStatistics(const FMassifDebugChannelLegend& Legend, float PanelLeft, float RowTop);
	void DrawHistogram(const FMassifDebugChannelLegend& Legend, float PanelLeft, float RowTop);
	void DrawHistogramBar(
		const FMassifDebugChannelLegend& Legend, int32 BucketIndex, int32 LargestCount, float BarLeft,
		float RowTop, float BarWidth);

	void DrawEnvironmentInspector();

	/**
	 * The picker overlay: every view the registry offers, chosen with the mouse,
	 * and a scrubber over the year for the views that hold one.
	 */
	void DrawViewPicker(AMassifDebugChannelView& View);
	void DrawPickerRows(AMassifDebugChannelView& View, const struct FMassifViewPickerLayout& Layout);
	void DrawPickerRow(
		const AMassifDebugChannelView& View, const FMassifViewPickerLayout& Layout, int32 ViewIndex,
		FVector2D Position, bool bHovered, bool bSelected);
	void DrawSliceScrubber(AMassifDebugChannelView& View, const FMassifViewPickerLayout& Layout);

	/** Applies a click on the picker. True where the click landed on it. */
	bool HandlePickerClick(AMassifDebugChannelView& View, const FMassifViewPickerLayout& Layout);
	float DrawProcessRows(const FMassifInspectorSnapshot& Snapshot, float Left, float RowTop);
	float DrawChannelRows(const FMassifInspectorSnapshot& Snapshot, float Left, float RowTop);

	void DrawProbePanel();
	void DrawProbeRows(const FMassifProbeResult& Probe, float Left, float RowTop);

	TWeakObjectPtr<AMassifDebugChannelView> ChannelView;
};
