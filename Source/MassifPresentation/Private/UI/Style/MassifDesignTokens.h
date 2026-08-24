#pragma once

#include "CoreMinimal.h"

/**
 * The foundation layer of `DESIGN.md`: every colour, measure and duration the UI
 * is allowed to use.
 *
 * Widgets never construct a colour or pick a pixel value of their own — they name
 * a token here, or a widget style built from one (`MassifWidgetStyles.h`). That
 * rule is what an automation test checks, so this header is the only place in the
 * presentation UI where a literal appears.
 *
 * Nothing here depends on Slate. Tokens are values; turning them into brushes and
 * widget styles is `MassifStyle`'s job.
 */
namespace MassifDesignTokens
{
	/**
	 * Six cool neutrals, near-black to bone white (`DESIGN.md` §2.1).
	 *
	 * Cool rather than pure grey because the whole game is drawn over snow and rock
	 * under an alpine sky, and a neutral grey panel reads warm against that.
	 */
	namespace Neutral
	{
		inline constexpr FLinearColor Surface(0.020f, 0.022f, 0.027f);
		inline constexpr FLinearColor Raised(0.055f, 0.060f, 0.070f);
		inline constexpr FLinearColor Control(0.100f, 0.108f, 0.124f);
		inline constexpr FLinearColor Line(0.180f, 0.192f, 0.215f);
		inline constexpr FLinearColor TextMuted(0.520f, 0.545f, 0.590f);
		inline constexpr FLinearColor TextPrimary(0.880f, 0.895f, 0.910f);
	}

	/** Panels sit over terrain the player is meant to keep reading, so they are translucent. */
	inline constexpr FLinearColor PanelBackground(0.020f, 0.020f, 0.030f, 0.940f);

	/** Behind a screen that takes over, so what it covers reads as inactive rather than as clutter. */
	inline constexpr FLinearColor Scrim(0.010f, 0.011f, 0.014f, 0.780f);

	/**
	 * The single accent: cold glacial cyan-blue.
	 *
	 * Deliberately off the blue the water and wetness ramps occupy — chrome and data
	 * are separate systems (`DESIGN.md` §2), and a selection colour that lands inside
	 * a legend's ramp makes the legend lie.
	 */
	inline constexpr FLinearColor Accent(0.243f, 0.706f, 0.788f);

	/** Staged but not applied. The settings screen's amber, promoted out of the widget. */
	inline constexpr FLinearColor Staged(0.720f, 0.520f, 0.140f);

	inline constexpr FLinearColor Error(0.780f, 0.240f, 0.200f);

	inline constexpr FLinearColor TextDisabled(0.320f, 0.340f, 0.380f);

	/** 4 px base. These seven steps and nothing between them (`DESIGN.md` §3). */
	namespace Spacing
	{
		inline constexpr float Hair = 4.0f;
		inline constexpr float Tight = 8.0f;
		inline constexpr float Snug = 12.0f;
		inline constexpr float Base = 16.0f;
		inline constexpr float Loose = 24.0f;
		inline constexpr float Section = 32.0f;
		inline constexpr float Page = 48.0f;
	}

	/**
	 * Five sizes, two weights.
	 *
	 * `DESIGN.md` §3 names Inter or IBM Plex Sans; the engine ships neither, so Roboto
	 * stands in until a face is licensed. It is the same class of neutral grotesque and
	 * its digits are already tabular, which is the property this UI actually needs.
	 */
	namespace TypeSize
	{
		inline constexpr int32 Micro = 10;
		inline constexpr int32 Small = 12;
		inline constexpr int32 Body = 14;
		inline constexpr int32 Title = 20;
		inline constexpr int32 Display = 32;
	}

	/** Sharp reads cartographic; rounded reads mobile app. */
	inline constexpr float CornerRadius = 2.0f;

	inline constexpr float HairlineThickness = 1.0f;

	/** Nothing eases in on a panel the user opened with a hotkey. */
	inline constexpr float MotionSeconds = 0.12f;
}
