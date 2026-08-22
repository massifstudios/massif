#include "MassifDebugHud.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "MassifDebugChannelView.h"
#include "MassifEnvironmentInspector.h"
#include "MassifViewPicker.h"
#include "MassifEnvironmentProbe.h"
#include "MassifGameMode.h"

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
	constexpr float StatsRowHeight = 17.0f;
	constexpr float HistogramHeight = 30.0f;

	/** Slices the ramp is drawn in. Enough that a four-stop ramp reads as a gradient. */
	constexpr int32 RampSliceCount = 96;

	const FLinearColor PanelBackground(0.02f, 0.02f, 0.03f, 0.78f);
	const FLinearColor PanelText(0.94f, 0.95f, 0.96f, 1.0f);

	constexpr float InspectorWidth = 430.0f;
	constexpr float InspectorRowHeight = 15.0f;
	constexpr float ProbeWidth = 460.0f;

	/** Dirt is what has not been re-solved yet, so it is the one thing worth colouring. */
	const FLinearColor DirtyText(1.0f, 0.78f, 0.35f, 1.0f);
	const FLinearColor QuietText(0.62f, 0.65f, 0.7f, 1.0f);
	const FLinearColor HoverHighlight(0.22f, 0.26f, 0.32f, 0.85f);
	const FLinearColor ScrubberTick(0.30f, 0.36f, 0.44f, 1.0f);

	/** The off entry sits above the views, so index -1 is a name rather than a lookup. */
	FString PickerRowLabel(const AMassifDebugChannelView& View, int32 ViewIndex)
	{
		if (ViewIndex < 0)
		{
			return TEXT("off");
		}

		const FMassifDebugView& Entry = View.GetViews()[ViewIndex];
		return FString::Printf(TEXT("%-34s %s"), *Entry.Name.ToString(), *Entry.Unit);
	}

	const TCHAR* FeedbackMarker(bool bInFeedbackGroup)
	{
		if (bInFeedbackGroup)
		{
			return TEXT("  loop");
		}
		return TEXT("");
	}

	const TCHAR* DirtyMarker(bool bDirty)
	{
		if (bDirty)
		{
			return TEXT("  dirty");
		}
		return TEXT("");
	}

	FLinearColor RowColor(bool bHighlighted, const FLinearColor& Quiet)
	{
		if (bHighlighted)
		{
			return DirtyText;
		}
		return Quiet;
	}

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

	float ContinuousPanelHeight()
	{
		return TitleHeight + RampHeight + LabelHeight + StatsRowHeight + HistogramHeight + 2.0f * PanelPadding;
	}

	/** Ground, shape and climate: two lines, because one would run off the panel. */
	TArray<FString> HabitatRows(const FMassifHabitatSample& Sample)
	{
		return {
			FString::Printf(TEXT("habitat  %s, %.0f m, %.1f deg slope, aspect %.0f deg"),
				MassifDebugChannels::GetGroundTypeLabel(Sample.GroundType), Sample.AltitudeMeters,
				Sample.SlopeDegrees, Sample.AspectDegrees),
			FString::Printf(TEXT("         %.1f C, humidity %.2f, soil %.2f m, standing water %.2f m"),
				Sample.TemperatureCelsius, Sample.Humidity, Sample.SoilDepthMeters,
				Sample.StandingWaterDepthMeters) };
	}

	int32 ProbeHabitatRowCount(const FMassifProbeResult& Probe)
	{
		if (!Probe.Habitat.IsSet())
		{
			return 0;
		}
		return HabitatRows(*Probe.Habitat).Num();
	}
}

AMassifDebugChannelView* AMassifDebugHud::FindChannelView()
{
	if (ChannelView.IsValid())
	{
		return ChannelView.Get();
	}
	ChannelView = AMassifDebugChannelView::Find(this);
	return ChannelView.Get();
}

void AMassifDebugHud::DrawHUD()
{
	Super::DrawHUD();
	if (Canvas == nullptr)
	{
		return;
	}

	if (MassifEnvironmentInspector::IsPanelVisible())
	{
		DrawEnvironmentInspector();
	}
	if (MassifEnvironmentProbe::IsPanelVisible())
	{
		DrawProbePanel();
	}
	DrawChannelLegend();
}

void AMassifDebugHud::DrawChannelLegend()
{
	AMassifDebugChannelView* View = FindChannelView();
	if (View == nullptr || View->GetChannel() == EMassifDebugChannel::Off)
	{
		return;
	}

	const FMassifDebugChannelLegend& Legend = View->GetLegend();
	const int32 ViewIndex = static_cast<int32>(View->GetChannel());
	if (Legend.IsClassified())
	{
		const float Height = TitleHeight + Legend.ClassLabels.Num() * SwatchRowHeight + 2.0f * PanelPadding;
		DrawPanel(Legend, ViewIndex, Height);
		DrawClassifiedLegend(Legend, PanelLeftEdge(*Canvas), PanelMargin + PanelPadding + TitleHeight);
		return;
	}

	const float RowTop = PanelMargin + PanelPadding + TitleHeight;
	DrawPanel(Legend, ViewIndex, ContinuousPanelHeight());
	DrawContinuousLegend(Legend, PanelLeftEdge(*Canvas), RowTop);
	DrawStatistics(Legend, PanelLeftEdge(*Canvas), RowTop + RampHeight + LabelHeight);
}

void AMassifDebugHud::DrawPanel(const FMassifDebugChannelLegend& Legend, int32 ViewIndex, float PanelHeight)
{
	const float Left = PanelLeftEdge(*Canvas);
	DrawRect(PanelBackground, Left, PanelMargin, PanelWidth, PanelHeight);

	// The index comes first: the legend doubles as the list of what the number
	// keys do, so it has to say which one is currently down.
	const FString Title = FString::Printf(TEXT("%d  %s"), ViewIndex, *Legend.Name);
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

void AMassifDebugHud::DrawStatistics(
	const FMassifDebugChannelLegend& Legend, float PanelLeft, float RowTop)
{
	if (Legend.Statistics.SampleCount == 0)
	{
		return;
	}

	const FString Text = FString::Printf(TEXT("min %s   mean %s   max %s"),
		*FormatValue(Legend.Statistics.Minimum), *FormatValue(Legend.Statistics.Mean),
		*FormatValue(Legend.Statistics.Maximum));
	DrawText(Text, PanelText, PanelLeft + PanelPadding, RowTop, nullptr, 1.0f);
	DrawHistogram(Legend, PanelLeft, RowTop + StatsRowHeight);
}

void AMassifDebugHud::DrawHistogram(
	const FMassifDebugChannelLegend& Legend, float PanelLeft, float RowTop)
{
	const int32 LargestCount = Legend.Statistics.GetLargestBucketCount();
	if (LargestCount <= 0)
	{
		return;
	}

	const float BarWidth = (PanelWidth - 2.0f * PanelPadding) / MassifHistogramBucketCount;
	for (int32 BucketIndex = 0; BucketIndex < Legend.Statistics.BucketCounts.Num(); ++BucketIndex)
	{
		const float BarLeft = PanelLeft + PanelPadding + BucketIndex * BarWidth;
		DrawHistogramBar(Legend, BucketIndex, LargestCount, BarLeft, RowTop, BarWidth);
	}
}

void AMassifDebugHud::DrawHistogramBar(
	const FMassifDebugChannelLegend& Legend, int32 BucketIndex, int32 LargestCount, float BarLeft, float RowTop,
	float BarWidth)
{
	const float Fraction = Legend.Statistics.BucketCounts[BucketIndex] / static_cast<float>(LargestCount);
	// A bucket holding anything at all keeps a pixel, so an outlier tail stays visible.
	const float BarHeight = FMath::Max(HistogramHeight * Fraction, 1.0f);
	const FLinearColor Color =
		EvaluateRamp(Legend.RampStops, (BucketIndex + 0.5f) / MassifHistogramBucketCount);
	DrawRect(Color, BarLeft, RowTop + HistogramHeight - BarHeight, BarWidth + 1.0f, BarHeight);
}

void AMassifDebugHud::DrawEnvironmentInspector()
{
	const AMassifGameMode* GameMode = GetWorld()->GetAuthGameMode<AMassifGameMode>();
	if (GameMode == nullptr || !GameMode->GetEnvironmentScheduler().IsValid())
	{
		return;
	}

	const FMassifInspectorSnapshot Snapshot =
		MassifEnvironmentInspector::Snapshot(*GameMode->GetEnvironmentScheduler());
	if (Snapshot.IsEmpty())
	{
		return;
	}

	const float Height =
		TitleHeight + (Snapshot.Processes.Num() + Snapshot.Channels.Num() + 2) * InspectorRowHeight
		+ 2.0f * PanelPadding;
	DrawRect(PanelBackground, PanelMargin, PanelMargin, InspectorWidth, Height);

	const FString Title = FString::Printf(TEXT("Environment  %.0f ms  %.0f MB"),
		Snapshot.TotalSolveSeconds * 1000.0, Snapshot.ResidentByteCount / (1024.0 * 1024.0));
	DrawText(Title, PanelText, PanelMargin + PanelPadding, PanelMargin + PanelPadding, nullptr, 1.15f);

	const float RowTop = PanelMargin + PanelPadding + TitleHeight;
	const float AfterProcesses = DrawProcessRows(Snapshot, PanelMargin + PanelPadding, RowTop);
	DrawChannelRows(Snapshot, PanelMargin + PanelPadding, AfterProcesses + InspectorRowHeight);
}

float AMassifDebugHud::DrawProcessRows(const FMassifInspectorSnapshot& Snapshot, float Left, float RowTop)
{
	float Top = RowTop;
	for (const FMassifInspectorProcessRow& Row : Snapshot.Processes)
	{
		// The loop marker matters: a relaxed process ran several times, so its cost
		// is not comparable with a process that ran once.
		const FString Text = FString::Printf(TEXT("%-28s %-10s %5.0f ms%s%s"),
			*Row.Name.ToString(), MassifFidelityName(Row.Fidelity), Row.LastExecutionSeconds * 1000.0,
			FeedbackMarker(Row.bInFeedbackGroup), DirtyMarker(Row.bDirty));
		DrawText(Text, RowColor(Row.bDirty, PanelText), Left, Top, nullptr, 1.0f);
		Top += InspectorRowHeight;
	}
	return Top;
}

float AMassifDebugHud::DrawChannelRows(const FMassifInspectorSnapshot& Snapshot, float Left, float RowTop)
{
	float Top = RowTop;
	for (const FMassifInspectorChannelRow& Row : Snapshot.Channels)
	{
		const FString Text = FString::Printf(TEXT("%-34s %4.0f m  %s to %s %s"),
			*Row.Name.ToString(), Row.CellSizeMeters, *FormatValue(Row.Minimum), *FormatValue(Row.Maximum),
			*Row.Unit);
		DrawText(Text, RowColor(!Row.bWritten, QuietText), Left, Top, nullptr, 1.0f);
		Top += InspectorRowHeight;
	}
	return Top;
}

void AMassifDebugHud::DrawProbePanel()
{
	const FMassifProbeResult& Probe = MassifEnvironmentProbe::GetLastProbe();
	if (Probe.IsEmpty())
	{
		return;
	}

	const float Height =
		TitleHeight + (Probe.Readings.Num() + ProbeHabitatRowCount(Probe)) * InspectorRowHeight
		+ 2.0f * PanelPadding;
	const float PanelTop = Canvas->SizeY - Height - PanelMargin;
	DrawRect(PanelBackground, PanelMargin, PanelTop, ProbeWidth, Height);

	const FString Title = FString::Printf(
		TEXT("Probe  %.0f, %.0f m"), Probe.PositionMeters.X, Probe.PositionMeters.Y);
	DrawText(Title, PanelText, PanelMargin + PanelPadding, PanelTop + PanelPadding, nullptr, 1.15f);
	DrawProbeRows(Probe, PanelMargin + PanelPadding, PanelTop + PanelPadding + TitleHeight);
}

void AMassifDebugHud::DrawProbeRows(const FMassifProbeResult& Probe, float Left, float RowTop)
{
	float Top = RowTop;
	for (const FMassifProbeReading& Reading : Probe.Readings)
	{
		const FString Text = FString::Printf(
			TEXT("%-34s %s"), *Reading.Name.ToString(), *MassifEnvironmentProbe::FormatReading(Reading));
		DrawText(Text, PanelText, Left, Top, nullptr, 1.0f);
		Top += InspectorRowHeight;
	}

	if (!Probe.Habitat.IsSet())
	{
		return;
	}

	for (const FString& Row : HabitatRows(*Probe.Habitat))
	{
		DrawText(Row, QuietText, Left, Top, nullptr, 1.0f);
		Top += InspectorRowHeight;
	}
}

void AMassifDebugHud::DrawViewPicker(AMassifDebugChannelView& View)
{
	const FMassifViewPickerLayout Layout = MassifViewPicker::MakeLayout(
		View.GetViews().Num(), View.GetSelectedTimeSliceCount(), Canvas->SizeY);

	DrawRect(PanelBackground, Layout.Left, Layout.Top, Layout.PanelWidth(), Layout.TotalHeight());
	DrawText(TEXT("Environment views"), PanelText, Layout.Left + Layout.Padding,
		Layout.Top + Layout.Padding, nullptr, 1.15f);

	DrawPickerRows(View, Layout);
	DrawSliceScrubber(View, Layout);
	HandlePickerClick(View, Layout);
}

void AMassifDebugHud::DrawPickerRows(AMassifDebugChannelView& View, const FMassifViewPickerLayout& Layout)
{
	FVector2D MousePosition = FVector2D::ZeroVector;
	if (PlayerOwner != nullptr)
	{
		PlayerOwner->GetMousePosition(MousePosition.X, MousePosition.Y);
	}

	const int32 Hovered = MassifViewPicker::ViewIndexAt(Layout, MousePosition);
	const int32 Selected = static_cast<int32>(View.GetChannel()) - 1;

	for (int32 EntryIndex = 0; EntryIndex < Layout.EntryCount; ++EntryIndex)
	{
		const int32 ViewIndex = EntryIndex - 1;
		DrawPickerRow(View, Layout, ViewIndex, MassifViewPicker::EntryPosition(Layout, EntryIndex),
			ViewIndex == Hovered, ViewIndex == Selected);
	}
}

void AMassifDebugHud::DrawPickerRow(
	const AMassifDebugChannelView& View, const FMassifViewPickerLayout& Layout, int32 ViewIndex,
	FVector2D Position, bool bHovered, bool bSelected)
{
	if (bHovered)
	{
		DrawRect(HoverHighlight, Position.X, Position.Y, Layout.ColumnWidth, Layout.RowHeight);
	}

	const FString Label = PickerRowLabel(View, ViewIndex);
	DrawText(Label, RowColor(bSelected, QuietText), Position.X + Layout.Padding, Position.Y, nullptr, 1.0f);
}

void AMassifDebugHud::DrawSliceScrubber(AMassifDebugChannelView& View, const FMassifViewPickerLayout& Layout)
{
	if (Layout.TimeSliceCount <= 1)
	{
		return;
	}

	const float Top = Layout.ScrubberTop();
	const float TickWidth = Layout.PanelWidth() / Layout.TimeSliceCount;
	for (int32 Slice = 0; Slice < Layout.TimeSliceCount; ++Slice)
	{
		const bool bCurrent = Slice == View.GetTimeSlice();
		DrawRect(RowColor(bCurrent, ScrubberTick), Layout.Left + Slice * TickWidth + 1.0f, Top,
			TickWidth - 2.0f, Layout.ScrubberHeight);
	}

	DrawText(FString::Printf(TEXT("slice %d of %d"), View.GetTimeSlice() + 1, Layout.TimeSliceCount),
		PanelText, Layout.Left + Layout.Padding, Top + Layout.ScrubberHeight + 2.0f, nullptr, 1.0f);
}

bool AMassifDebugHud::HandlePickerClick(AMassifDebugChannelView& View, const FMassifViewPickerLayout& Layout)
{
	if (PlayerOwner == nullptr || !PlayerOwner->WasInputKeyJustPressed(EKeys::LeftMouseButton))
	{
		return false;
	}

	FVector2D MousePosition = FVector2D::ZeroVector;
	PlayerOwner->GetMousePosition(MousePosition.X, MousePosition.Y);

	const int32 Slice = MassifViewPicker::TimeSliceAt(Layout, MousePosition);
	if (Slice != INDEX_NONE)
	{
		MassifViewPicker::SetTimeSlice(Slice);
		return true;
	}

	const int32 ViewIndex = MassifViewPicker::ViewIndexAt(Layout, MousePosition);
	if (ViewIndex != INDEX_NONE)
	{
		AMassifDebugChannelView::RequestChannel(static_cast<EMassifDebugChannel>(ViewIndex + 1));
		return true;
	}
	return MassifViewPicker::ContainsPoint(Layout, MousePosition);
}
