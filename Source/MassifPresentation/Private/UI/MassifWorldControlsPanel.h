#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * Puts the corner controls on the viewport and takes them off again.
 *
 * Shown from the start rather than behind a key: the time of day and the fog are
 * both things you set while watching what they change, and a panel you have to
 * summon is one you forget is there.
 */
namespace MassifWorldControlsPanel
{
	void Show(UWorld* World);
	void Hide();
	void Toggle(UWorld* World);
	bool IsVisible();
}
