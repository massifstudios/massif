#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class UTexture2D;
struct FSlateBrush;

/**
 * What the menu and the loading screen are drawn over.
 *
 * Drop a texture at the path below and both screens draw it; leave it out and both
 * fall back to the flat backdrop token. Art arrives without a code change.
 *
 * Owned by the screen that draws it, and holds the texture against collection for
 * exactly as long as the brush pointing at it is on screen.
 */
class FMassifMenuBackdrop
{
public:
	static const TCHAR* GetTexturePath();

	/** The art if there is any, the flat token if there is not. Loads on first call. */
	const FSlateBrush* GetBrush();

private:
	TStrongObjectPtr<UTexture2D> Texture;
	TSharedPtr<FSlateBrush> Brush;
};
