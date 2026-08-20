#include "MassifDebugHud.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Kismet/GameplayStatics.h"
#include "MassifDebugChannelView.h"

namespace
{
	constexpr float PanelWidth = 250.0f;
	constexpr float PanelMargin = 24.0f;
	constexpr float PanelPadding = 12.0f;
	constexpr float TitleHeight = 22.0f;
	constexpr float RampHeight = 14.0f;
	constexpr float LabelHeight = 18.0f;
	constexpr float SwatchSize = 13.0f;
	constexpr float SwatchRowHeight = 18.0f;

	/** Slices the ramp is drawn in. Enough that a four-stop ramp reads as a gradient. */
	constexpr int32 RampSliceCount = 96;

	const FLinearColor PanelBackground(0.02f, 0.02f, 0.03f, 0.78f);
	const FLinearColor PanelText(0.94f, 0.95f, 0.96f, 1.0f);

	float PanelLeftEdge(const UCanvas& Canvas)
	{
		return Canvas.SizeX - PanelWidth - PanelMargin;
	}

	FLinearColor EvaluateRamp(const TArray<FLinearColor>& Ramp, float Fraction)
	{
		const float Scaled = FMath::Clamp(Fraction, 0.0f, 1.0f) * (Ramp.Num() - 1);
		const int32 Lower = FMath::Min(FMath::FloorToInt32(Scaled), Ramp.Num() - 1);
		const int32 Upper = FMath::Min(Lower + 1, Ramp.Num() - 1);
		return FMath::Lerp(Ramp[Lower], Ramp[Upper], Scaled - Lower);
	}

	/** Enough decimals to tell two ends of a narrow range apart, and no more. */
	FString FormatValue(double Value)
	{
		if (FMath::Abs(Value) >= 100.0)
		{
			return FString::Printf(TEXT("%.0f"), Value);
		}
		if (FMath::Abs(Value) >= 1.0)
		{
			return FString::Printf(TEXT("%.1f"), Value);
		}
		return FString::Printf(TEXT("%.2f"), Value);
	}
}

AMassifDebugChannelView* AMassifDebugHud::FindChannelView()
{
	if (ChannelView.IsValid())
	{
		return ChannelView.Get();
	}
	ChannelView = Cast<AMassifDebugChannelView>(
		UGameplayStatics::GetActorOfClass(this, AMassifDebugChannelView::StaticClass()));
	return ChannelView.Get();
}

void AMassifDebugHud::DrawHUD()
{
	Super::DrawHUD();

	AMassifDebugChannelView* View = FindChannelView();
	if (View == nullptr || View->GetChannel() == EMassifDebugChannel::Off || Canvas == nullptr)
	{
		return;
	}

	const FMassifDebugChannelLegend& Legend = View->GetLegend();
	const int32 ChannelKey = static_cast<int32>(View->GetChannel());
	if (Legend.IsClassified())
	{
		const float Height = TitleHeight + Legend.ClassLabels.Num() * SwatchRowHeight + 2.0f * PanelPadding;
		DrawPanel(Legend, ChannelKey, Height);
		DrawClassifiedLegend(Legend, PanelLeftEdge(*Canvas), PanelMargin + PanelPadding + TitleHeight);
		return;
	}

	const float Height = TitleHeight + RampHeight + LabelHeight + 2.0f * PanelPadding;
	DrawPanel(Legend, ChannelKey, Height);
	DrawContinuousLegend(Legend, PanelLeftEdge(*Canvas), PanelMargin + PanelPadding + TitleHeight);
}

void AMassifDebugHud::DrawPanel(const FMassifDebugChannelLegend& Legend, int32 ChannelKey, float PanelHeight)
{
	const float Left = PanelLeftEdge(*Canvas);
	DrawRect(PanelBackground, Left, PanelMargin, PanelWidth, PanelHeight);

	// The key comes first: the legend doubles as the list of what the number
	// keys do, so it has to say which one is currently down.
	const FString Title = FString::Printf(TEXT("%d  %s"), ChannelKey, *Legend.Name);
	DrawText(Title, PanelText, Left + PanelPadding, PanelMargin + PanelPadding, nullptr, 1.15f);
}

void AMassifDebugHud::DrawContinuousLegend(
	const FMassifDebugChannelLegend& Legend, float PanelLeft, float RowTop)
{
	const float BarLeft = PanelLeft + PanelPadding;
	const float BarWidth = PanelWidth - 2.0f * PanelPadding;
	const float SliceWidth = BarWidth / RampSliceCount;
	for (int32 Slice = 0; Slice < RampSliceCount; ++Slice)
	{
		const FLinearColor Color = EvaluateRamp(Legend.RampStops, Slice / static_cast<float>(RampSliceCount - 1));
		// Slices overlap by a pixel: exact widths leave seams once they round.
		DrawRect(Color, BarLeft + Slice * SliceWidth, RowTop, SliceWidth + 1.0f, RampHeight);
	}

	const FString Low = FormatValue(Legend.MinValue);
	const FString High = FString::Printf(TEXT("%s %s"), *FormatValue(Legend.MaxValue), *Legend.Unit);
	const float LabelTop = RowTop + RampHeight + 3.0f;
	DrawText(Low, PanelText, BarLeft, LabelTop, nullptr, 1.0f);

	float HighWidth = 0.0f;
	float HighHeight = 0.0f;
	GetTextSize(High, HighWidth, HighHeight, nullptr, 1.0f);
	DrawText(High, PanelText, BarLeft + BarWidth - HighWidth, LabelTop, nullptr, 1.0f);
}

void AMassifDebugHud::DrawClassifiedLegend(
	const FMassifDebugChannelLegend& Legend, float PanelLeft, float RowTop)
{
	const float SwatchLeft = PanelLeft + PanelPadding;
	for (int32 ClassIndex = 0; ClassIndex < Legend.ClassLabels.Num(); ++ClassIndex)
	{
		const float Top = RowTop + ClassIndex * SwatchRowHeight;
		DrawRect(Legend.RampStops[ClassIndex], SwatchLeft, Top, SwatchSize, SwatchSize);
		DrawText(Legend.ClassLabels[ClassIndex], PanelText, SwatchLeft + SwatchSize + 8.0f, Top - 2.0f, nullptr, 1.0f);
	}
}
