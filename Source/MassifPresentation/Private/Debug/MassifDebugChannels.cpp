#include "MassifDebugChannels.h"

#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "MassifEnvironmentBake.h"
#include "MassifHabitat.h"
#include "MassifTerrainField.h"
#include "TextureResource.h"

namespace
{
	/** Spacing habitat is sampled at for a whole-world view: the bake's own cell size. */
	constexpr double DebugViewSampleSpacingMeters = 8.0;

	// Ramps run low to high. Values are what the colour looks like on screen: the
	// texture is uploaded as sRGB, so these are the bytes written, not linear.
	const FLinearColor StandingWaterRamp[] = {
		FLinearColor(0.86f, 0.86f, 0.83f), FLinearColor(0.45f, 0.68f, 0.85f), FLinearColor(0.04f, 0.16f, 0.52f) };
	const FLinearColor FlowRamp[] = {
		FLinearColor(0.11f, 0.12f, 0.14f), FLinearColor(0.16f, 0.45f, 0.70f), FLinearColor(0.62f, 0.92f, 1.00f) };
	const FLinearColor WetnessRamp[] = {
		FLinearColor(0.74f, 0.61f, 0.37f), FLinearColor(0.57f, 0.70f, 0.35f),
		FLinearColor(0.20f, 0.56f, 0.56f), FLinearColor(0.09f, 0.28f, 0.71f) };
	const FLinearColor SoilRamp[] = {
		FLinearColor(0.58f, 0.56f, 0.53f), FLinearColor(0.53f, 0.40f, 0.25f), FLinearColor(0.22f, 0.14f, 0.08f) };
	const FLinearColor HumidityRamp[] = {
		FLinearColor(0.79f, 0.66f, 0.41f), FLinearColor(0.36f, 0.62f, 0.55f), FLinearColor(0.10f, 0.29f, 0.73f) };
	const FLinearColor TemperatureRamp[] = {
		FLinearColor(0.18f, 0.34f, 0.86f), FLinearColor(0.92f, 0.93f, 0.95f), FLinearColor(0.86f, 0.33f, 0.13f) };
	const FLinearColor AltitudeRamp[] = {
		FLinearColor(0.17f, 0.42f, 0.23f), FLinearColor(0.76f, 0.72f, 0.39f),
		FLinearColor(0.55f, 0.40f, 0.27f), FLinearColor(0.96f, 0.97f, 0.99f) };
	const FLinearColor SlopeRamp[] = {
		FLinearColor(0.20f, 0.50f, 0.25f), FLinearColor(0.86f, 0.80f, 0.28f), FLinearColor(0.81f, 0.24f, 0.19f) };

	/** One colour per EMassifGroundType, in the enum's own order. */
	const FLinearColor GroundTypeColors[] = {
		FLinearColor(0.46f, 0.45f, 0.44f), FLinearColor(0.73f, 0.63f, 0.47f), FLinearColor(0.29f, 0.51f, 0.21f),
		FLinearColor(0.19f, 0.56f, 0.56f), FLinearColor(0.96f, 0.97f, 0.99f),
		FLinearColor(0.09f, 0.28f, 0.72f) };

	static_assert(UE_ARRAY_COUNT(GroundTypeColors) == static_cast<int32>(EMassifGroundType::Count),
		"Every ground type needs a debug colour");

	/** How a channel is read off the world, and how its numbers become colour. */
	struct FChannelDescription
	{
		const TCHAR* Name = TEXT("");
		const TCHAR* Unit = TEXT("");
		TArrayView<const FLinearColor> Ramp;

		/** Set where the numbers are class indices rather than a measurement. */
		TArrayView<const FString> ClassLabels;

		/**
		 * Fixed display range. Left at zero for channels whose interesting range
		 * depends on the world, which are normalised against what the solve
		 * actually produced instead.
		 */
		double FixedMinimum = 0.0;
		double FixedMaximum = 0.0;

		/** Flow accumulation covers six orders of magnitude, so it is ramped in log space. */
		bool bLogarithmic = false;

		bool HasFixedRange() const
		{
			return FixedMaximum > FixedMinimum;
		}

		bool IsClassified() const
		{
			return !ClassLabels.IsEmpty();
		}
	};

	const TArray<FString>& GroundTypeLabels()
	{
		static const TArray<FString> Labels = {
			TEXT("Rock"), TEXT("Scree"), TEXT("Soil"), TEXT("Wetland"), TEXT("Snow") };
		return Labels;
	}

	FChannelDescription DescribeChannel(EMassifDebugChannel Channel)
	{
		switch (Channel)
		{
			case EMassifDebugChannel::StandingWater:
				return { TEXT("Standing water"), TEXT("m"), StandingWaterRamp };
			case EMassifDebugChannel::FlowAccumulation:
				return { TEXT("Flow accumulation"), TEXT("m2"), FlowRamp, {}, 0.0, 0.0, true };
			case EMassifDebugChannel::Wetness:
				return { TEXT("Wetness"), TEXT(""), WetnessRamp, {}, 0.0, 1.0 };
			case EMassifDebugChannel::SoilDepth:
				return { TEXT("Soil depth"), TEXT("m"), SoilRamp };
			case EMassifDebugChannel::GroundType:
				return { TEXT("Ground type"), TEXT(""), GroundTypeColors, GroundTypeLabels() };
			case EMassifDebugChannel::Humidity:
				return { TEXT("Humidity"), TEXT(""), HumidityRamp, {}, 0.0, 1.0 };
			case EMassifDebugChannel::Temperature:
				return { TEXT("Temperature"), TEXT("C"), TemperatureRamp };
			case EMassifDebugChannel::Altitude:
				return { TEXT("Altitude"), TEXT("m"), AltitudeRamp };
			case EMassifDebugChannel::Slope:
				return { TEXT("Slope"), TEXT("deg"), SlopeRamp, {}, 0.0, 70.0 };
			default:
				return {};
		}
	}

	/** Channels that come straight out of the bake, without the habitat layer over them. */
	bool TryGetBakeChannel(EMassifDebugChannel Channel, EMassifBakeChannel& OutBakeChannel)
	{
		switch (Channel)
		{
			case EMassifDebugChannel::StandingWater:
				OutBakeChannel = EMassifBakeChannel::StandingWaterDepth;
				return true;
			case EMassifDebugChannel::FlowAccumulation:
				OutBakeChannel = EMassifBakeChannel::FlowAccumulation;
				return true;
			case EMassifDebugChannel::Wetness:
				OutBakeChannel = EMassifBakeChannel::Wetness;
				return true;
			case EMassifDebugChannel::SoilDepth:
				OutBakeChannel = EMassifBakeChannel::SoilDepth;
				return true;
			default:
				return false;
		}
	}

	double SampleHabitatChannel(
		const FMassifHabitatField& Habitat, EMassifDebugChannel Channel, FVector2d PositionMeters)
	{
		const FMassifHabitatSample Sample = Habitat.Sample(PositionMeters, DebugViewSampleSpacingMeters);
		switch (Channel)
		{
			case EMassifDebugChannel::GroundType:
				return static_cast<double>(Sample.GroundType);
			case EMassifDebugChannel::Humidity:
				return Sample.Humidity;
			case EMassifDebugChannel::Temperature:
				return Sample.TemperatureCelsius;
			case EMassifDebugChannel::Altitude:
				return Sample.AltitudeMeters;
			case EMassifDebugChannel::Slope:
				return Sample.SlopeDegrees;
			default:
				return 0.0;
		}
	}

	void SampleRow(
		const FMassifHabitatField& Habitat, EMassifDebugChannel Channel, int32 Row, TArray<float>& Values)
	{
		const FMassifEnvironmentBake& Bake = Habitat.GetEnvironmentBake();
		EMassifBakeChannel BakeChannel = EMassifBakeChannel::Wetness;
		const bool bFromBake = TryGetBakeChannel(Channel, BakeChannel);

		for (int32 Column = 0; Column < Bake.GetWidth(); ++Column)
		{
			const int32 Index = Row * Bake.GetWidth() + Column;
			if (bFromBake)
			{
				Values[Index] = static_cast<float>(Bake.ReadCell(BakeChannel, Column, Row));
				continue;
			}
			Values[Index] = static_cast<float>(
				SampleHabitatChannel(Habitat, Channel, Bake.GetCellCentreMeters(Column, Row)));
		}
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

	TArray<float> SortedSample(TArrayView<const float> Values)
	{
		const int32 Stride = FMath::Max(Values.Num() / RangeSampleTarget, 1);
		TArray<float> Sorted;
		Sorted.Reserve(Values.Num() / Stride + 1);
		for (int32 Index = 0; Index < Values.Num(); Index += Stride)
		{
			Sorted.Add(Values[Index]);
		}
		Sorted.Sort();
		return Sorted;
	}

	void MeasureExtent(TArrayView<const float> Values, double& OutMinimum, double& OutMaximum)
	{
		OutMinimum = TNumericLimits<double>::Max();
		OutMaximum = TNumericLimits<double>::Lowest();
		for (float Value : Values)
		{
			OutMinimum = FMath::Min(OutMinimum, static_cast<double>(Value));
			OutMaximum = FMath::Max(OutMaximum, static_cast<double>(Value));
		}
	}

	void MeasureRange(TArrayView<const float> Values, double& OutMinimum, double& OutMaximum)
	{
		MeasureExtent(Values, OutMinimum, OutMaximum);

		const TArray<float> Sorted = SortedSample(Values);
		if (Sorted.IsEmpty())
		{
			return;
		}

		const int32 LastIndex = Sorted.Num() - 1;
		const double Low = Sorted[FMath::RoundToInt32(RampLowPercentile * LastIndex)];
		const double High = Sorted[FMath::RoundToInt32(RampHighPercentile * LastIndex)];

		// An empty percentile range means the channel is flat almost everywhere and
		// interesting in a handful of cells — standing water, which is a few tarns
		// in a world of dry ground. Clipping there would draw nothing at all, so
		// those channels keep the full extent measured above.
		if (High - Low > UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			OutMinimum = Low;
			OutMaximum = High;
		}
	}

	FLinearColor EvaluateRamp(TArrayView<const FLinearColor> Ramp, double Fraction)
	{
		if (Ramp.IsEmpty())
		{
			return FLinearColor::Black;
		}

		const double Scaled = FMath::Clamp(Fraction, 0.0, 1.0) * (Ramp.Num() - 1);
		const int32 Lower = FMath::Min(FMath::FloorToInt32(Scaled), Ramp.Num() - 1);
		const int32 Upper = FMath::Min(Lower + 1, Ramp.Num() - 1);
		return FMath::Lerp(Ramp[Lower], Ramp[Upper], static_cast<float>(Scaled - Lower));
	}

	/** Where a value sits between the ends of the legend, in the space the ramp is drawn in. */
	double RampFraction(const FChannelDescription& Description, double Value, double Minimum, double Maximum)
	{
		if (Maximum - Minimum <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return 0.0;
		}
		if (!Description.bLogarithmic)
		{
			return (Value - Minimum) / (Maximum - Minimum);
		}

		// Against the observed floor as well as the ceiling. Measuring only from
		// zero would put a whole hillside, which spans two decades, inside the
		// first fifth of a ramp that has to reach a river eight decades up.
		const double CompressedMin = FMath::Loge(1.0 + FMath::Max(Minimum, 0.0));
		const double CompressedSpan = FMath::Loge(1.0 + FMath::Max(Maximum, 0.0)) - CompressedMin;
		if (CompressedSpan <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return 0.0;
		}
		return (FMath::Loge(1.0 + FMath::Max(Value, 0.0)) - CompressedMin) / CompressedSpan;
	}

	FColor ColorForValue(
		const FChannelDescription& Description, double Value, double Minimum, double Maximum)
	{
		if (Description.IsClassified())
		{
			const int32 ClassIndex = FMath::Clamp(FMath::RoundToInt32(Value), 0, Description.Ramp.Num() - 1);
			return Description.Ramp[ClassIndex].ToFColor(true);
		}
		return EvaluateRamp(Description.Ramp, RampFraction(Description, Value, Minimum, Maximum)).ToFColor(true);
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
}

const TCHAR* MassifDebugChannels::GetName(EMassifDebugChannel Channel)
{
	if (Channel == EMassifDebugChannel::Off)
	{
		return TEXT("Off");
	}
	return DescribeChannel(Channel).Name;
}

void MassifDebugChannels::RenderChannel(
	const FMassifHabitatField& Habitat,
	EMassifDebugChannel Channel,
	TObjectPtr<UTexture2D>& Texture,
	FMassifDebugChannelLegend& OutLegend)
{
	const FMassifEnvironmentBake& Bake = Habitat.GetEnvironmentBake();
	const FChannelDescription Description = DescribeChannel(Channel);

	TArray<float> Values;
	Values.SetNumUninitialized(Bake.GetWidth() * Bake.GetHeight());
	ParallelFor(Bake.GetHeight(), [&Habitat, Channel, &Values](int32 Row)
	{
		SampleRow(Habitat, Channel, Row, Values);
	});

	double Minimum = Description.FixedMinimum;
	double Maximum = Description.FixedMaximum;
	if (!Description.HasFixedRange())
	{
		MeasureRange(Values, Minimum, Maximum);
	}

	OutLegend.Name = Description.Name;
	OutLegend.Unit = Description.Unit;
	OutLegend.MinValue = Minimum;
	OutLegend.MaxValue = Maximum;
	OutLegend.RampStops.Reset();
	OutLegend.RampStops.Append(Description.Ramp.GetData(), Description.Ramp.Num());
	OutLegend.ClassLabels.Reset();
	OutLegend.ClassLabels.Append(Description.ClassLabels.GetData(), Description.ClassLabels.Num());

	UTexture2D* ChannelTexture = EnsureTexture(Texture, Bake.GetWidth(), Bake.GetHeight());
	// Classes must not be interpolated: a pixel halfway between rock and soil
	// would be drawn as the scree that sits between them in the enum.
	ChannelTexture->Filter = TF_Bilinear;
	if (Description.IsClassified())
	{
		ChannelTexture->Filter = TF_Nearest;
	}

	FTexture2DMipMap& Mip = ChannelTexture->GetPlatformData()->Mips[0];
	FColor* Pixels = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
	const int32 Width = Bake.GetWidth();
	ParallelFor(Bake.GetHeight(), [&Description, &Values, Pixels, Width, Minimum, Maximum](int32 Row)
	{
		for (int32 Column = 0; Column < Width; ++Column)
		{
			const int32 Index = Row * Width + Column;
			Pixels[Index] = ColorForValue(Description, Values[Index], Minimum, Maximum);
		}
	});
	Mip.BulkData.Unlock();
	ChannelTexture->UpdateResource();
}
