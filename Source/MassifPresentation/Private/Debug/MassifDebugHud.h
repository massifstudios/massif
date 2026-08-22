#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "MassifDebugHud.generated.h"

class AMassifDebugChannelView;
struct FMassifDebugChannelLegend;

/**
 * Draws the legend for the active debug channel view in the top-right corner,
 * and the environment process graph down the left when massif.Env.Inspector is
 * on:
 * which key is held down, what it shows, and what the colours on the ground
 * mean. Nothing at all while no view is on.
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

	void DrawPanel(const FMassifDebugChannelLegend& Legend, int32 ChannelKey, float PanelHeight);
	void DrawContinuousLegend(const FMassifDebugChannelLegend& Legend, float PanelLeft, float RowTop);
	void DrawClassifiedLegend(const FMassifDebugChannelLegend& Legend, float PanelLeft, float RowTop);

	void DrawEnvironmentInspector();
	float DrawProcessRows(const struct FMassifInspectorSnapshot& Snapshot, float Left, float RowTop);
	float DrawChannelRows(const FMassifInspectorSnapshot& Snapshot, float Left, float RowTop);

	TWeakObjectPtr<AMassifDebugChannelView> ChannelView;
};
