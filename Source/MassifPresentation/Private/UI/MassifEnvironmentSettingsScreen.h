#pragma once

#include "CoreMinimal.h"

class FMassifEnvironmentScheduler;
class UWorld;

/**
 * Puts the environment settings screen on the viewport and takes it off again.
 *
 * Opened with massif.Env.Settings. The screen takes the mouse while it is up,
 * because a fly camera that keeps turning under a menu makes the menu unusable.
 */
namespace MassifEnvironmentSettingsScreen
{
	/** Opens over the world's scheduler, or closes it if it is already up. */
	void Toggle(UWorld* World);

	void Close();

	bool IsOpen();
}
