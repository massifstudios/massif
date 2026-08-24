#pragma once

#include "CoreMinimal.h"

class ISlateStyle;

/**
 * The one style set the game's UI draws from, built out of `MassifDesignTokens`.
 *
 * Registered from `FMassifPresentationModule::StartupModule` and unregistered on
 * shutdown, so the brushes it owns do not outlive a hot reload.
 *
 * Widgets should reach for `MassifWidgetStyles.h` rather than this: the named
 * roles there are what a widget means, and the style set is only where they live.
 */
namespace MassifStyle
{
	void Register();

	void Unregister();

	/** Registered on first use, so a headless test can ask for a style without a module. */
	const ISlateStyle& Get();

	FName GetStyleSetName();
}
