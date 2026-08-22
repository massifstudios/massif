#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureDefines.h"
#include "MassifChannelStatistics.h"
#include "MassifEnvironmentChannel.h"

class FMassifEnvironmentState;
class FMassifHabitatField;
class UTexture2D;
enum class EMassifGroundType : uint8;

/**
 * Which view the terrain is painted with, as an index into the list
 * MassifDebugChannels::BuildViews returns, offset by one so that 0 is off and
 * returns the terrain to its own material.
 *
 * Only the ends are named. The list comes from the channel registry, so what a
 * given index shows depends on what the graph registered, and a channel nothing
 * here knows the name of is selectable the moment it exists (ARCHITECTURE §5.7).
 * Count is how many views the number keys reach; massif.Env.View reaches the
 * rest by name.
 */
enum class EMassifDebugChannel : uint8
{
	Off = 0,
};

/** Where a view's numbers come from. */
enum class EMassifDebugViewSource : uint8
{
	/** A channel the environment graph registered. */
	Channel,

	/** Derived by FMassifHabitatField out of several channels and the seed; no channel holds it. */
	Habitat,
};

/** The habitat-derived views. A fixed set, because habitat is one fixed function. */
enum class EMassifHabitatView : uint8
{
	GroundType,
	Humidity,
	Temperature,
	Altitude,
	Slope,
	Count,
};

/** One selectable view of the world, and everything needed to draw and label it. */
struct FMassifDebugView
{
	FName Name;
	FString Unit;
	EMassifDebugViewSource Source = EMassifDebugViewSource::Channel;
	EMassifChannelColorMap ColorMap = EMassifChannelColorMap::Viridis;

	/** Unset normalises the ramp to what the solve actually produced. */
	TOptional<FVector2d> DisplayRange;

	/** Valid where Source is Channel. */
	FMassifChannelHandle Channel;
	EMassifChannelType ChannelType = EMassifChannelType::Scalar;

	/** Slices of a year the view holds. Above 1 the picker offers a scrubber over them. */
	int32 TimeSliceCount = 1;

	/** Meaningful where Source is Habitat. */
	EMassifHabitatView HabitatView = EMassifHabitatView::GroundType;

	/** One per class where the numbers are class indices. Empty for a measurement. */
	TArray<FString> ClassLabels;

	bool IsClassified() const
	{
		return !ClassLabels.IsEmpty();
	}

	/** A direction field, which a heatmap of its magnitude cannot show going the wrong way. */
	bool IsVectorField() const
	{
		return Source == EMassifDebugViewSource::Channel && ChannelType == EMassifChannelType::Vector2;
	}
};

/** What the legend needs to explain a view: what it shows, in what unit, over what range. */
struct FMassifDebugChannelLegend
{
	FString Name;
	FString Unit;
	double MinValue = 0.0;
	double MaxValue = 0.0;

	/** Colours from MinValue to MaxValue, or one per class where ClassLabels is filled. */
	TArray<FLinearColor> RampStops;

	/** Non-empty for a classified view, which has classes rather than a range. */
	TArray<FString> ClassLabels;

	/** Over every cell the view was rendered from, not over the clipped ramp range. */
	FMassifChannelStatistics Statistics;

	bool IsClassified() const
	{
		return !ClassLabels.IsEmpty();
	}
};

namespace MassifDebugChannels
{
	/**
	 * Every selectable view: the registered channels in name order, then the
	 * habitat-derived ones. A registered channel supplies its own label, unit,
	 * colour map and display range, so nothing here names a channel.
	 */
	TArray<FMassifDebugView> BuildViews(const FMassifEnvironmentState& Environment);

	/** INDEX_NONE where no view carries the name. Names are matched case-insensitively. */
	int32 FindViewIndex(const TArray<FMassifDebugView>& Views, FName Name);

	/** The ground type as the legend and the probe both name it. */
	const TCHAR* GetGroundTypeLabel(EMassifGroundType GroundType);

	/** Colour at a fraction of a ramp, which the vector overlay draws its arrows with too. */
	FLinearColor EvaluateColorMap(EMassifChannelColorMap ColorMap, double Fraction);

	/**
	 * Nearest for a classified view: a pixel halfway between rock and soil would
	 * otherwise be drawn as the scree that sits between them in the enum.
	 */
	TextureFilter GetTextureFilter(const FMassifDebugView& View);

	/**
	 * Renders one view over the whole world into Texture, on the grid the view is
	 * solved at, and fills the legend that reads it. Reuses the texture when it
	 * already has the right size, so switching views does not allocate 25 MB a
	 * keypress.
	 *
	 * Sampling runs across the thread pool: both the solved channels and habitat
	 * are pure functions of position, so a row is independent of every other row.
	 */
	void RenderChannel(
		const FMassifHabitatField& Habitat,
		const FMassifDebugView& View,
		int32 TimeSlice,
		TObjectPtr<UTexture2D>& Texture,
		FMassifDebugChannelLegend& OutLegend);

	/** The slice a view can actually show, which is 0 for everything that holds one. */
	int32 ClampTimeSlice(const FMassifDebugView& View, int32 TimeSlice);
}
