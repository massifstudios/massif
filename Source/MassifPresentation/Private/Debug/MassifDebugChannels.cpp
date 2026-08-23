#include "MassifDebugChannels.h"

#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "MassifEnvironmentState.h"
#include "MassifHabitat.h"
#include "TextureResource.h"

namespace
{
	// Ramps run low to high. Values are what the colour looks like on screen: the
	// texture is uploaded as sRGB, so these are the bytes written, not linear.
	const FLinearColor ViridisRamp[] = {
		FLinearColor(0.267f, 0.005f, 0.329f), FLinearColor(0.254f, 0.265f, 0.530f),
		FLinearColor(0.164f, 0.471f, 0.558f), FLinearColor(0.135f, 0.659f, 0.518f),
		FLinearColor(0.478f, 0.821f, 0.318f), FLinearColor(0.993f, 0.906f, 0.144f) };
	const FLinearColor MagmaRamp[] = {
		FLinearColor(0.001f, 0.000f, 0.014f), FLinearColor(0.230f, 0.060f, 0.437f),
		FLinearColor(0.535f, 0.146f, 0.532f), FLinearColor(0.828f, 0.281f, 0.430f),
		FLinearColor(0.983f, 0.588f, 0.375f), FLinearColor(0.987f, 0.991f, 0.749f) };
	const FLinearColor BlueWhiteRamp[] = {
		FLinearColor(0.86f, 0.86f, 0.83f), FLinearColor(0.45f, 0.68f, 0.85f), FLinearColor(0.04f, 0.16f, 0.52f) };
	const FLinearColor TerrainRamp[] = {
		FLinearColor(0.17f, 0.42f, 0.23f), FLinearColor(0.76f, 0.72f, 0.39f),
		FLinearColor(0.55f, 0.40f, 0.27f), FLinearColor(0.96f, 0.97f, 0.99f) };

	/**
	 * One colour per class. The first entries are the ground types in the enum's
	 * own order, so the habitat view keeps its reading; the rest carry a
	 * registered classified channel, which names no colours of its own.
	 */
	const FLinearColor CategoricalRamp[] = {
		FLinearColor(0.46f, 0.45f, 0.44f), FLinearColor(0.73f, 0.63f, 0.47f), FLinearColor(0.29f, 0.51f, 0.21f),
		FLinearColor(0.19f, 0.56f, 0.56f), FLinearColor(0.96f, 0.97f, 0.99f), FLinearColor(0.09f, 0.28f, 0.72f),
		FLinearColor(0.85f, 0.37f, 0.28f), FLinearColor(0.85f, 0.72f, 0.24f), FLinearColor(0.62f, 0.35f, 0.71f),
		FLinearColor(0.35f, 0.71f, 0.68f), FLinearColor(0.90f, 0.55f, 0.75f), FLinearColor(0.50f, 0.50f, 0.50f) };

	static_assert(UE_ARRAY_COUNT(CategoricalRamp) >= static_cast<int32>(EMassifGroundType::Count),
		"Every ground type needs a debug colour");

	/** Classes a legend will list. Past this the view is not a classification any more. */
	constexpr int32 MaxLegendClassCount = static_cast<int32>(UE_ARRAY_COUNT(CategoricalRamp));

	TArray<FString> GroundTypeLabels()
	{
		TArray<FString> Labels;
		for (int32 TypeIndex = 0; TypeIndex < static_cast<int32>(EMassifGroundType::Count); ++TypeIndex)
		{
			Labels.Add(MassifDebugChannels::GetGroundTypeLabel(static_cast<EMassifGroundType>(TypeIndex)));
		}
		return Labels;
	}

	TArrayView<const FLinearColor> GetRamp(EMassifChannelColorMap ColorMap)
	{
		switch (ColorMap)
		{
			case EMassifChannelColorMap::Magma:
				return MagmaRamp;
			case EMassifChannelColorMap::BlueWhite:
				return BlueWhiteRamp;
			case EMassifChannelColorMap::Terrain:
				return TerrainRamp;
			case EMassifChannelColorMap::Categorical:
				return CategoricalRamp;
			default:
				return ViridisRamp;
		}
	}

	/** Classes are drawn from the categorical ramp whatever colour map a channel declared. */
	EMassifChannelColorMap ResolveColorMap(const FMassifDebugView& View)
	{
		if (View.IsClassified())
		{
			return EMassifChannelColorMap::Categorical;
		}
		return View.ColorMap;
	}

	/** The finest grid the graph solves on, which is what a world-wide view is sized against. */
	const FMassifChannelGrid& FinestGrid(const FMassifEnvironmentState& Environment)
	{
		const TArray<FMassifChannelHandle> Channels = Environment.GetChannelsByName();
		FMassifChannelHandle Finest = Channels[0];
		for (FMassifChannelHandle Channel : Channels)
		{
			const bool bFiner =
				Environment.GetGrid(Channel).CellSizeMeters < Environment.GetGrid(Finest).CellSizeMeters;
			if (bFiner)
			{
				Finest = Channel;
			}
		}
		return Environment.GetGrid(Finest);
	}

	/** A registered channel is drawn on its own grid; a habitat view on the finest one. */
	const FMassifChannelGrid& ResolveViewGrid(
		const FMassifEnvironmentState& Environment, const FMassifDebugView& View)
	{
		if (View.Source == EMassifDebugViewSource::Channel)
		{
			return Environment.GetGrid(View.Channel);
		}
		return FinestGrid(Environment);
	}

	/** A registered classified channel names no classes, so the legend lists its indices. */
	TArray<FString> MakeClassLabels(const FMassifEnvironmentState& Environment, FMassifChannelHandle Channel)
	{
		if (Environment.GetDescriptor(Channel).Type != EMassifChannelType::Classified)
		{
			return {};
		}

		const int32 HighestClass = FMath::Clamp(
			FMath::RoundToInt32(Environment.GetChannelMaximum(Channel)), 0, MaxLegendClassCount - 1);
		TArray<FString> Labels;
		for (int32 ClassIndex = 0; ClassIndex <= HighestClass; ++ClassIndex)
		{
			Labels.Add(FString::Printf(TEXT("Class %d"), ClassIndex));
		}
		return Labels;
	}

	FMassifDebugView MakeChannelView(const FMassifEnvironmentState& Environment, FMassifChannelHandle Channel)
	{
		const FMassifChannelDescriptor& Descriptor = Environment.GetDescriptor(Channel);

		FMassifDebugView View;
		View.Name = Descriptor.Name;
		View.Unit = Descriptor.Unit;
		View.Source = EMassifDebugViewSource::Channel;
		View.ColorMap = Descriptor.ColorMap;
		View.DisplayRange = Descriptor.DisplayRange;
		View.Channel = Channel;
		View.ChannelType = Descriptor.Type;
		View.TimeSliceCount = Descriptor.TimeSliceCount;
		View.ClassLabels = MakeClassLabels(Environment, Channel);
		return View;
	}

	FMassifDebugView MakeHabitatView(EMassifHabitatView HabitatView)
	{
		FMassifDebugView View;
		View.Source = EMassifDebugViewSource::Habitat;
		View.HabitatView = HabitatView;
		switch (HabitatView)
		{
			case EMassifHabitatView::GroundType:
				View.Name = TEXT("Habitat.GroundType");
				View.ColorMap = EMassifChannelColorMap::Categorical;
				View.ClassLabels = GroundTypeLabels();
				return View;
			case EMassifHabitatView::Humidity:
				View.Name = TEXT("Habitat.Humidity");
				View.ColorMap = EMassifChannelColorMap::BlueWhite;
				View.DisplayRange = FVector2d(0.0, 1.0);
				return View;
			case EMassifHabitatView::Temperature:
				View.Name = TEXT("Habitat.Temperature");
				View.Unit = TEXT("C");
				View.ColorMap = EMassifChannelColorMap::Magma;
				return View;
			case EMassifHabitatView::Altitude:
				View.Name = TEXT("Habitat.Altitude");
				View.Unit = TEXT("m");
				View.ColorMap = EMassifChannelColorMap::Terrain;
				return View;
			default:
				View.Name = TEXT("Habitat.Slope");
				View.Unit = TEXT("deg");
				View.ColorMap = EMassifChannelColorMap::Magma;
				View.DisplayRange = FVector2d(0.0, 70.0);
				return View;
		}
	}

	double SampleHabitatView(
		const FMassifHabitatField& Habitat, EMassifHabitatView HabitatView, FVector2d PositionMeters,
		double SampleSpacingMeters)
	{
		const FMassifHabitatSample Sample = Habitat.Sample(PositionMeters, SampleSpacingMeters);
		switch (HabitatView)
		{
			case EMassifHabitatView::GroundType:
				return static_cast<double>(Sample.GroundType);
			case EMassifHabitatView::Humidity:
				return Sample.Humidity;
			case EMassifHabitatView::Temperature:
				return Sample.TemperatureCelsius;
			case EMassifHabitatView::Altitude:
				return Sample.AltitudeMeters;
			default:
				return Sample.SlopeDegrees;
		}
	}

	/** A vector channel's scalar view is its magnitude; the arrows carry the direction. */
	double ReadChannelCell(
		const FMassifEnvironmentState& Environment, const FMassifDebugView& View, int32 Column, int32 Row,
		int32 TimeSlice)
	{
		if (View.ChannelType != EMassifChannelType::Vector2)
		{
			return Environment.ReadCell(View.Channel, Column, Row, 0, TimeSlice);
		}

		const double East = Environment.ReadCell(View.Channel, Column, Row, 0, TimeSlice);
		const double North = Environment.ReadCell(View.Channel, Column, Row, 1, TimeSlice);
		return FMath::Sqrt(East * East + North * North);
	}

	void SampleChannelRow(
		const FMassifEnvironmentState& Environment,
		const FMassifDebugView& View,
		const FMassifChannelGrid& Grid,
		int32 Row,
		int32 TimeSlice,
		TArray<double>& Values)
	{
		for (int32 Column = 0; Column < Grid.Width; ++Column)
		{
			Values[Row * Grid.Width + Column] = ReadChannelCell(Environment, View, Column, Row, TimeSlice);
		}
	}

	void SampleHabitatRow(
		const FMassifHabitatField& Habitat,
		EMassifHabitatView HabitatView,
		const FMassifChannelGrid& Grid,
		int32 Row,
		TArray<double>& Values)
	{
		for (int32 Column = 0; Column < Grid.Width; ++Column)
		{
			Values[Row * Grid.Width + Column] = SampleHabitatView(
				Habitat, HabitatView, Grid.CellCentreMeters(Column, Row), Grid.CellSizeMeters);
		}
	}

	/** Every cell of the view, from whichever of the two sources it reads. */
	void SampleWorld(
		const FMassifHabitatField& Habitat,
		const FMassifDebugView& View,
		const FMassifChannelGrid& Grid,
		int32 TimeSlice,
		TArray<double>& Values)
	{
		Values.SetNumUninitialized(Grid.CellCount());
		if (View.Source == EMassifDebugViewSource::Habitat)
		{
			const EMassifHabitatView HabitatView = View.HabitatView;
			ParallelFor(Grid.Height, [&Habitat, HabitatView, &Grid, &Values](int32 Row)
			{
				SampleHabitatRow(Habitat, HabitatView, Grid, Row, Values);
			});
			return;
		}

		const FMassifEnvironmentState& Environment = Habitat.GetEnvironment();
		ParallelFor(Grid.Height, [&Environment, &View, &Grid, TimeSlice, &Values](int32 Row)
		{
			SampleChannelRow(Environment, View, Grid, Row, TimeSlice, Values);
		});
	}

	/**
	 * Where the ramp's ends are put, as fractions of the sorted readings.
	 * Not the extremes: a single trunk river carries a thousand times the flow of
	 * the hillsides around it, and ramping to that maximum leaves the whole
	 * massif inside the first fifth of the ramp, reading as one flat colour. The
	 * few percent past each end clip rather than compress everything else.
	 */
	constexpr double RampLowPercentile = 0.02;
	constexpr double RampHighPercentile = 0.98;

	/** Readings the range is measured from. Sorting 6 M values to place two ends is waste. */
	constexpr int32 RangeSampleTarget = 65536;

	/**
	 * How far the top of a channel has to stand above its middle before the ramp
	 * runs in log space. Flow accumulation holds a hillside and a trunk river in
	 * one channel, and no linear ramp can draw both.
	 */
	constexpr double LogarithmicRampRatio = 1000.0;

	TArray<double> SortedSample(TArrayView<const double> Values)
	{
		const int32 Stride = FMath::Max(Values.Num() / RangeSampleTarget, 1);
		TArray<double> Sorted;
		Sorted.Reserve(Values.Num() / Stride + 1);
		for (int32 Index = 0; Index < Values.Num(); Index += Stride)
		{
			Sorted.Add(Values[Index]);
		}
		Sorted.Sort();
		return Sorted;
	}

	bool SpansOrdersOfMagnitude(TArrayView<const double> Sorted, double Maximum)
	{
		if (Sorted.IsEmpty())
		{
			return false;
		}

		const double Median = Sorted[Sorted.Num() / 2];
		if (Median <= 0.0)
		{
			return false;
		}
		return Maximum / Median >= LogarithmicRampRatio;
	}

	/**
	 * An empty percentile range means the view is flat almost everywhere and
	 * interesting in a handful of cells — standing water, which is a few tarns in
	 * a world of dry ground. Clipping there would draw nothing at all, so those
	 * views keep the extent they were measured over.
	 */
	void ApplyPercentileEnds(TArrayView<const double> Sorted, FMassifDebugRampSpace& Space)
	{
		if (Sorted.IsEmpty())
		{
			return;
		}

		const int32 LastIndex = Sorted.Num() - 1;
		const double Low = Sorted[FMath::RoundToInt32(RampLowPercentile * LastIndex)];
		const double High = Sorted[FMath::RoundToInt32(RampHighPercentile * LastIndex)];
		if (High - Low > UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			Space.Minimum = Low;
			Space.Maximum = High;
		}
	}

	/**
	 * Readings from across the whole year rather than from the slice being drawn.
	 *
	 * A scrubbed series has to be drawn against one ramp or the scrubber shows
	 * nothing. A channel whose slices differ only in scale — a year's rainfall
	 * shared out over its half-months is exactly that — renormalises to an
	 * identical picture in every slice, and the season becomes invisible in the one
	 * view that exists to show it.
	 *
	 * Strided across the slices as well as across the cells, so measuring a year
	 * costs what measuring one slice cost.
	 */
	TArray<double> SortedYearSample(
		const FMassifEnvironmentState& Environment, const FMassifDebugView& View, const FMassifChannelGrid& Grid)
	{
		const int32 Stride = FMath::Max(Grid.CellCount() * View.TimeSliceCount / RangeSampleTarget, 1);
		TArray<double> Sorted;
		Sorted.Reserve(Grid.CellCount() * View.TimeSliceCount / Stride + 1);
		for (int64 Reading = 0; Reading < int64(Grid.CellCount()) * View.TimeSliceCount; Reading += Stride)
		{
			const int32 CellIndex = static_cast<int32>(Reading % Grid.CellCount());
			const int32 TimeSlice = static_cast<int32>(Reading / Grid.CellCount());
			Sorted.Add(
				ReadChannelCell(Environment, View, CellIndex % Grid.Width, CellIndex / Grid.Width, TimeSlice));
		}
		Sorted.Sort();
		return Sorted;
	}

	/**
	 * What the ramp is measured over: the whole year where the view has one, and the
	 * drawn values where it does not. Habitat views carry no slices, and a channel
	 * of one slice has already been read.
	 */
	TArray<double> MeasureRampSample(
		const FMassifEnvironmentState& Environment, const FMassifDebugView& View, const FMassifChannelGrid& Grid,
		TArrayView<const double> SliceValues)
	{
		if (View.Source == EMassifDebugViewSource::Habitat || View.TimeSliceCount <= 1)
		{
			return SortedSample(SliceValues);
		}
		return SortedYearSample(Environment, View, Grid);
	}

	FMassifDebugRampSpace MeasureRampSpaceOver(
		const FMassifDebugView& View, TArrayView<const double> Sorted)
	{
		FMassifDebugRampSpace Space;
		if (View.DisplayRange.IsSet())
		{
			Space.Minimum = View.DisplayRange->X;
			Space.Maximum = View.DisplayRange->Y;
			return Space;
		}
		if (Sorted.IsEmpty())
		{
			return Space;
		}

		Space.Minimum = Sorted[0];
		Space.Maximum = Sorted[Sorted.Num() - 1];
		Space.bLogarithmic = SpansOrdersOfMagnitude(Sorted, Space.Maximum);
		ApplyPercentileEnds(Sorted, Space);
		return Space;
	}

	/** Where a value sits between the ends of the legend, in the space the ramp is drawn in. */
	double RampFraction(const FMassifDebugRampSpace& Space, double Value)
	{
		if (Space.Maximum - Space.Minimum <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return 0.0;
		}
		if (!Space.bLogarithmic)
		{
			return (Value - Space.Minimum) / (Space.Maximum - Space.Minimum);
		}

		// Against the observed floor as well as the ceiling. Measuring only from
		// zero would put a whole hillside, which spans two decades, inside the
		// first fifth of a ramp that has to reach a river eight decades up.
		const double CompressedMin = FMath::Loge(1.0 + FMath::Max(Space.Minimum, 0.0));
		const double CompressedSpan = FMath::Loge(1.0 + FMath::Max(Space.Maximum, 0.0)) - CompressedMin;
		if (CompressedSpan <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return 0.0;
		}
		return (FMath::Loge(1.0 + FMath::Max(Value, 0.0)) - CompressedMin) / CompressedSpan;
	}

	FColor ColorForValue(const FMassifDebugView& View, const FMassifDebugRampSpace& Space, double Value)
	{
		if (View.IsClassified())
		{
			const TArrayView<const FLinearColor> Ramp = GetRamp(ResolveColorMap(View));
			const int32 ClassIndex = FMath::Clamp(FMath::RoundToInt32(Value), 0, Ramp.Num() - 1);
			return Ramp[ClassIndex].ToFColor(true);
		}
		return MassifDebugChannels::EvaluateColorMap(ResolveColorMap(View), RampFraction(Space, Value))
			.ToFColor(true);
	}

	UTexture2D* EnsureTexture(TObjectPtr<UTexture2D>& Texture, int32 Width, int32 Height)
	{
		const bool bReusable = Texture != nullptr
			&& Texture->GetSizeX() == Width && Texture->GetSizeY() == Height;
		if (bReusable)
		{
			return Texture;
		}

		Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->SRGB = true;
		Texture->NeverStream = true;
		return Texture;
	}

	void FillLegend(
		const FMassifDebugView& View,
		const FMassifDebugRampSpace& Space,
		const FMassifChannelStatistics& Statistics,
		FMassifDebugChannelLegend& OutLegend)
	{
		const TArrayView<const FLinearColor> Ramp = GetRamp(ResolveColorMap(View));

		OutLegend.Name = View.Name.ToString();
		OutLegend.Unit = View.Unit;
		OutLegend.MinValue = Space.Minimum;
		OutLegend.MaxValue = Space.Maximum;
		OutLegend.Statistics = Statistics;
		OutLegend.RampStops.Reset();
		OutLegend.RampStops.Append(Ramp.GetData(), Ramp.Num());
		OutLegend.ClassLabels = View.ClassLabels;
	}

	void UploadPixels(
		const FMassifDebugView& View,
		const FMassifDebugRampSpace& Space,
		const FMassifChannelGrid& Grid,
		TArrayView<const double> Values,
		UTexture2D& Texture)
	{
		FTexture2DMipMap& Mip = Texture.GetPlatformData()->Mips[0];
		FColor* Pixels = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
		const int32 Width = Grid.Width;
		ParallelFor(Grid.Height, [&View, &Space, &Values, Pixels, Width](int32 Row)
		{
			for (int32 Column = 0; Column < Width; ++Column)
			{
				const int32 Index = Row * Width + Column;
				Pixels[Index] = ColorForValue(View, Space, Values[Index]);
			}
		});
		Mip.BulkData.Unlock();
		Texture.UpdateResource();
	}
}

TArray<FMassifDebugView> MassifDebugChannels::BuildViews(const FMassifEnvironmentState& Environment)
{
	TArray<FMassifDebugView> Views;
	for (FMassifChannelHandle Channel : Environment.GetChannelsByName())
	{
		Views.Add(MakeChannelView(Environment, Channel));
	}

	for (int32 ViewIndex = 0; ViewIndex < static_cast<int32>(EMassifHabitatView::Count); ++ViewIndex)
	{
		Views.Add(MakeHabitatView(static_cast<EMassifHabitatView>(ViewIndex)));
	}
	return Views;
}

const TCHAR* MassifDebugChannels::GetGroundTypeLabel(EMassifGroundType GroundType)
{
	switch (GroundType)
	{
		case EMassifGroundType::Rock: return TEXT("Rock");
		case EMassifGroundType::Scree: return TEXT("Scree");
		case EMassifGroundType::Soil: return TEXT("Soil");
		case EMassifGroundType::Wetland: return TEXT("Wetland");
		case EMassifGroundType::PermanentSnow: return TEXT("Snow");
		case EMassifGroundType::Water: return TEXT("Water");
		default: return TEXT("Unknown");
	}
}

int32 MassifDebugChannels::FindViewIndex(const TArray<FMassifDebugView>& Views, FName Name)
{
	return Views.IndexOfByPredicate([Name](const FMassifDebugView& View)
	{
		return View.Name == Name;
	});
}

FLinearColor MassifDebugChannels::EvaluateColorMap(EMassifChannelColorMap ColorMap, double Fraction)
{
	const TArrayView<const FLinearColor> Ramp = GetRamp(ColorMap);
	const double Scaled = FMath::Clamp(Fraction, 0.0, 1.0) * (Ramp.Num() - 1);
	const int32 Lower = FMath::Min(FMath::FloorToInt32(Scaled), Ramp.Num() - 1);
	const int32 Upper = FMath::Min(Lower + 1, Ramp.Num() - 1);
	return FMath::Lerp(Ramp[Lower], Ramp[Upper], static_cast<float>(Scaled - Lower));
}

TextureFilter MassifDebugChannels::GetTextureFilter(const FMassifDebugView& View)
{
	if (View.IsClassified())
	{
		return TF_Nearest;
	}
	return TF_Bilinear;
}

int32 MassifDebugChannels::ClampTimeSlice(const FMassifDebugView& View, int32 TimeSlice)
{
	return FMath::Clamp(TimeSlice, 0, FMath::Max(0, View.TimeSliceCount - 1));
}

FMassifDebugRampSpace MassifDebugChannels::MeasureRampSpace(
	const FMassifEnvironmentState& Environment,
	const FMassifDebugView& View,
	const FMassifChannelGrid& Grid,
	TArrayView<const double> SliceValues)
{
	return MeasureRampSpaceOver(View, MeasureRampSample(Environment, View, Grid, SliceValues));
}

void MassifDebugChannels::RenderChannel(
	const FMassifHabitatField& Habitat,
	const FMassifDebugView& View,
	int32 TimeSlice,
	TObjectPtr<UTexture2D>& Texture,
	FMassifDebugChannelLegend& OutLegend)
{
	const FMassifEnvironmentState& Environment = Habitat.GetEnvironment();
	const FMassifChannelGrid& Grid = ResolveViewGrid(Environment, View);

	TArray<double> Values;
	SampleWorld(Habitat, View, Grid, ClampTimeSlice(View, TimeSlice), Values);

	const FMassifChannelStatistics Statistics = MassifChannelStatistics::Measure(Values);
	const FMassifDebugRampSpace Space =
		MassifDebugChannels::MeasureRampSpace(Environment, View, Grid, Values);
	FillLegend(View, Space, Statistics, OutLegend);

	UTexture2D* ViewTexture = EnsureTexture(Texture, Grid.Width, Grid.Height);
	ViewTexture->Filter = GetTextureFilter(View);
	UploadPixels(View, Space, Grid, Values, *ViewTexture);
}
