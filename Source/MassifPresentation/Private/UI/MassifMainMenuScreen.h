#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * Puts the main menu on the viewport and takes it off again.
 *
 * The menu is what a run opens on, so nothing of the world is built until an
 * entry asks for it. Reopened with massif.Menu, which is also how a screen is
 * looked at again without restarting.
 */
namespace MassifMainMenuScreen
{
	void Open(UWorld* World);

	void Close();

	bool IsOpen();
}
