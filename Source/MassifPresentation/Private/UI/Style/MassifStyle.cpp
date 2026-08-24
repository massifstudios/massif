#include "MassifStyle.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "MassifDesignTokens.h"
#include "MassifWidgetStyles.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"

namespace
{
	TSharedPtr<FSlateStyleSet> GStyleSet;

	/** Two weights, per DESIGN.md §3. Regular carries text, Medium carries emphasis. */
	FSlateFontInfo RegularFont(int32 SizePoints)
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", SizePoints);
	}

	FSlateFontInfo MediumFont(int32 SizePoints)
	{
		return FCoreStyle::GetDefaultFontStyle("Medium", SizePoints);
	}

	FTextBlockStyle MakeText(FSlateFontInfo Font, const FLinearColor& Colour)
	{
		return FTextBlockStyle()
			.SetFont(MoveTemp(Font))
			.SetColorAndOpacity(FSlateColor(Colour))
			.SetSelectedBackgroundColor(FSlateColor(MassifDesignTokens::Accent));
	}

	/** Uppercase section headers are letterspaced; nothing else is. */
	FSlateFontInfo LetterspacedFont(int32 SizePoints)
	{
		FSlateFontInfo Font = MediumFont(SizePoints);
		Font.LetterSpacing = 120;
		return Font;
	}

	void RegisterTextStyles(FSlateStyleSet& Style)
	{
		Style.Set("Text.Display",
			MakeText(MediumFont(MassifDesignTokens::TypeSize::Display), MassifDesignTokens::Neutral::TextPrimary));
		Style.Set("Text.Title",
			MakeText(MediumFont(MassifDesignTokens::TypeSize::Title), MassifDesignTokens::Neutral::TextPrimary));
		Style.Set("Text.Body",
			MakeText(RegularFont(MassifDesignTokens::TypeSize::Body), MassifDesignTokens::Neutral::TextPrimary));
		Style.Set("Text.Muted",
			MakeText(RegularFont(MassifDesignTokens::TypeSize::Small), MassifDesignTokens::Neutral::TextMuted));
		Style.Set("Text.Label",
			MakeText(LetterspacedFont(MassifDesignTokens::TypeSize::Micro), MassifDesignTokens::Neutral::TextMuted));
		Style.Set("Text.Readout",
			MakeText(RegularFont(MassifDesignTokens::TypeSize::Body), MassifDesignTokens::Neutral::TextPrimary));
		Style.Set("Text.Error",
			MakeText(RegularFont(MassifDesignTokens::TypeSize::Small), MassifDesignTokens::Error));
	}

	FSlateRoundedBoxBrush SurfaceBrush(const FLinearColor& Fill, const FLinearColor& Outline)
	{
		return FSlateRoundedBoxBrush(
			Fill, MassifDesignTokens::CornerRadius, Outline, MassifDesignTokens::HairlineThickness);
	}

	void RegisterBrushes(FSlateStyleSet& Style)
	{
		Style.Set("Brush.Panel", new FSlateRoundedBoxBrush(
			MassifDesignTokens::PanelBackground, MassifDesignTokens::CornerRadius,
			MassifDesignTokens::Neutral::Line, MassifDesignTokens::HairlineThickness));
		Style.Set("Brush.Control", new FSlateRoundedBoxBrush(
			MassifDesignTokens::Neutral::Control, MassifDesignTokens::CornerRadius,
			MassifDesignTokens::Neutral::Line, MassifDesignTokens::HairlineThickness));
		Style.Set("Brush.Separator", new FSlateColorBrush(MassifDesignTokens::Neutral::Line));
		Style.Set("Brush.Backdrop", new FSlateColorBrush(MassifDesignTokens::Neutral::Surface));
		Style.Set("Brush.Scrim", new FSlateColorBrush(MassifDesignTokens::Scrim));
	}

	/**
	 * A menu entry is a line of text until it is pointed at, and the accent is spent
	 * on the pointer rather than on a permanent fill. Four items down a dark screen do
	 * not need four filled rectangles to read as a list.
	 */
	FButtonStyle MakeMenuEntryStyle()
	{
		return FButtonStyle()
			.SetNormal(FSlateNoResource())
			.SetHovered(SurfaceBrush(MassifDesignTokens::Neutral::Raised, MassifDesignTokens::Accent))
			.SetPressed(SurfaceBrush(MassifDesignTokens::Neutral::Control, MassifDesignTokens::Accent))
			.SetDisabled(FSlateNoResource())
			.SetNormalForeground(FSlateColor(MassifDesignTokens::Neutral::TextPrimary))
			.SetHoveredForeground(FSlateColor(MassifDesignTokens::Accent))
			.SetPressedForeground(FSlateColor(MassifDesignTokens::Accent))
			.SetDisabledForeground(FSlateColor(MassifDesignTokens::TextDisabled))
			.SetNormalPadding(FMargin(MassifDesignTokens::Spacing::Base, MassifDesignTokens::Spacing::Snug))
			.SetPressedPadding(FMargin(MassifDesignTokens::Spacing::Base, MassifDesignTokens::Spacing::Snug));
	}

	FButtonStyle MakeButtonStyle()
	{
		return FButtonStyle()
			.SetNormal(SurfaceBrush(MassifDesignTokens::Neutral::Control, MassifDesignTokens::Neutral::Line))
			.SetHovered(SurfaceBrush(MassifDesignTokens::Neutral::Raised, MassifDesignTokens::Accent))
			.SetPressed(SurfaceBrush(MassifDesignTokens::Neutral::Surface, MassifDesignTokens::Accent))
			.SetDisabled(SurfaceBrush(MassifDesignTokens::Neutral::Surface, MassifDesignTokens::Neutral::Line))
			.SetNormalForeground(FSlateColor(MassifDesignTokens::Neutral::TextPrimary))
			.SetHoveredForeground(FSlateColor(MassifDesignTokens::Accent))
			.SetPressedForeground(FSlateColor(MassifDesignTokens::Accent))
			.SetDisabledForeground(FSlateColor(MassifDesignTokens::TextDisabled))
			.SetNormalPadding(FMargin(MassifDesignTokens::Spacing::Snug, MassifDesignTokens::Spacing::Tight))
			.SetPressedPadding(FMargin(MassifDesignTokens::Spacing::Snug, MassifDesignTokens::Spacing::Tight));
	}

	FEditableTextBoxStyle MakeTextEntryStyle()
	{
		return FEditableTextBoxStyle()
			.SetTextStyle(MakeText(
				RegularFont(MassifDesignTokens::TypeSize::Body), MassifDesignTokens::Neutral::TextPrimary))
			.SetBackgroundImageNormal(
				SurfaceBrush(MassifDesignTokens::Neutral::Surface, MassifDesignTokens::Neutral::Line))
			.SetBackgroundImageHovered(
				SurfaceBrush(MassifDesignTokens::Neutral::Surface, MassifDesignTokens::Accent))
			.SetBackgroundImageFocused(
				SurfaceBrush(MassifDesignTokens::Neutral::Surface, MassifDesignTokens::Accent))
			.SetBackgroundImageReadOnly(
				SurfaceBrush(MassifDesignTokens::Neutral::Surface, MassifDesignTokens::Neutral::Line))
			.SetForegroundColor(FSlateColor(MassifDesignTokens::Neutral::TextPrimary))
			.SetPadding(FMargin(MassifDesignTokens::Spacing::Tight, MassifDesignTokens::Spacing::Hair));
	}

	/** Square ends and a hairline track: a measurement, not a capsule. */
	FProgressBarStyle MakeProgressBarStyle()
	{
		return FProgressBarStyle()
			.SetBackgroundImage(FSlateColorBrush(MassifDesignTokens::Neutral::Control))
			.SetFillImage(FSlateColorBrush(MassifDesignTokens::Accent))
			.SetMarqueeImage(FSlateColorBrush(MassifDesignTokens::Neutral::Line));
	}

	void RegisterWidgetStyles(FSlateStyleSet& Style)
	{
		Style.Set("Button.MenuEntry", MakeMenuEntryStyle());
		Style.Set("Button.Default", MakeButtonStyle());
		Style.Set("TextEntry.Default", MakeTextEntryStyle());
		Style.Set("ProgressBar.Default", MakeProgressBarStyle());
	}

	TSharedRef<FSlateStyleSet> MakeStyleSet()
	{
		TSharedRef<FSlateStyleSet> Style = MakeShared<FSlateStyleSet>(MassifStyle::GetStyleSetName());
		RegisterTextStyles(Style.Get());
		RegisterBrushes(Style.Get());
		RegisterWidgetStyles(Style.Get());
		return Style;
	}
}

FName MassifStyle::GetStyleSetName()
{
	static const FName StyleSetName(TEXT("MassifStyle"));
	return StyleSetName;
}

void MassifStyle::Register()
{
	if (GStyleSet.IsValid())
	{
		return;
	}
	GStyleSet = MakeStyleSet();
	FSlateStyleRegistry::RegisterSlateStyle(*GStyleSet);
}

void MassifStyle::Unregister()
{
	if (!GStyleSet.IsValid())
	{
		return;
	}
	FSlateStyleRegistry::UnRegisterSlateStyle(*GStyleSet);
	GStyleSet.Reset();
}

const ISlateStyle& MassifStyle::Get()
{
	Register();
	return *GStyleSet;
}

const FTextBlockStyle& MassifWidgetStyles::Display()
{
	return MassifStyle::Get().GetWidgetStyle<FTextBlockStyle>("Text.Display");
}

const FTextBlockStyle& MassifWidgetStyles::Title()
{
	return MassifStyle::Get().GetWidgetStyle<FTextBlockStyle>("Text.Title");
}

const FTextBlockStyle& MassifWidgetStyles::Body()
{
	return MassifStyle::Get().GetWidgetStyle<FTextBlockStyle>("Text.Body");
}

const FTextBlockStyle& MassifWidgetStyles::Muted()
{
	return MassifStyle::Get().GetWidgetStyle<FTextBlockStyle>("Text.Muted");
}

const FTextBlockStyle& MassifWidgetStyles::Label()
{
	return MassifStyle::Get().GetWidgetStyle<FTextBlockStyle>("Text.Label");
}

const FTextBlockStyle& MassifWidgetStyles::Readout()
{
	return MassifStyle::Get().GetWidgetStyle<FTextBlockStyle>("Text.Readout");
}

const FTextBlockStyle& MassifWidgetStyles::Error()
{
	return MassifStyle::Get().GetWidgetStyle<FTextBlockStyle>("Text.Error");
}

const FButtonStyle& MassifWidgetStyles::MenuEntry()
{
	return MassifStyle::Get().GetWidgetStyle<FButtonStyle>("Button.MenuEntry");
}

const FButtonStyle& MassifWidgetStyles::Button()
{
	return MassifStyle::Get().GetWidgetStyle<FButtonStyle>("Button.Default");
}

const FEditableTextBoxStyle& MassifWidgetStyles::TextEntry()
{
	return MassifStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("TextEntry.Default");
}

const FProgressBarStyle& MassifWidgetStyles::ProgressBar()
{
	return MassifStyle::Get().GetWidgetStyle<FProgressBarStyle>("ProgressBar.Default");
}

const FSlateBrush* MassifWidgetStyles::PanelBrush()
{
	return MassifStyle::Get().GetBrush("Brush.Panel");
}

const FSlateBrush* MassifWidgetStyles::ControlBrush()
{
	return MassifStyle::Get().GetBrush("Brush.Control");
}

const FSlateBrush* MassifWidgetStyles::SeparatorBrush()
{
	return MassifStyle::Get().GetBrush("Brush.Separator");
}

const FSlateBrush* MassifWidgetStyles::BackdropBrush()
{
	return MassifStyle::Get().GetBrush("Brush.Backdrop");
}

const FSlateBrush* MassifWidgetStyles::ScrimBrush()
{
	return MassifStyle::Get().GetBrush("Brush.Scrim");
}
