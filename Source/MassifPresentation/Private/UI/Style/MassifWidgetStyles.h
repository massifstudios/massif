#pragma once

#include "CoreMinimal.h"

struct FButtonStyle;
struct FEditableTextBoxStyle;
struct FProgressBarStyle;
struct FSlateBrush;
struct FTextBlockStyle;

/**
 * What a widget is allowed to name.
 *
 * One accessor per role, not per appearance: a widget says it is drawing a section
 * label or a menu entry, and the look of that role is decided once in
 * `MassifStyle.cpp`. A widget that reaches past this for a colour of its own is the
 * thing the colour-literal test fails on.
 */
namespace MassifWidgetStyles
{
	/** The game's name and a screen's title. */
	const FTextBlockStyle& Display();
	const FTextBlockStyle& Title();

	const FTextBlockStyle& Body();

	/** Secondary lines: descriptions, hints, resolved values that are not the point. */
	const FTextBlockStyle& Muted();

	/** Uppercase letterspaced section headers. Small by role, not by choice. */
	const FTextBlockStyle& Label();

	/** Live numbers. Tabular figures, so a readout does not jitter as it updates. */
	const FTextBlockStyle& Readout();

	const FTextBlockStyle& Error();

	/** A main-menu entry: full width, quiet until hovered. */
	const FButtonStyle& MenuEntry();

	/** Everything else: settings buttons, dialog actions. */
	const FButtonStyle& Button();

	const FEditableTextBoxStyle& TextEntry();

	/** A thin filled rule, not a rounded capsule. */
	const FProgressBarStyle& ProgressBar();

	/** Translucent panel surface with a hairline border. */
	const FSlateBrush* PanelBrush();

	/** A control sitting on a panel, one step lighter than it. */
	const FSlateBrush* ControlBrush();

	/** A 1 px rule, for under a section header. */
	const FSlateBrush* SeparatorBrush();

	/** Opaque fill behind a whole screen, for when there is no world to show through. */
	const FSlateBrush* BackdropBrush();

	/** Translucent fill over whatever a takeover screen is covering. */
	const FSlateBrush* ScrimBrush();
}
