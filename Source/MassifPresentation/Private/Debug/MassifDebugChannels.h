#pragma once

#include "CoreMinimal.h"

class FMassifHabitatField;
class UTexture2D;

/**
 * One view of the generated world, selected by its number key (ARCHITECTURE
 * §5.7). Off is key 0 and returns the terrain to its own material.
 *
 * The order is the key order, so inserting a channel renumbers the ones after
 * it. That is deliberate: the keys are the interface, and grouping water
 * together is worth more than keeping a key stable across a commit.
 */
enum class EMassifDebugChannel : uint8
{
	Off,
	StandingWater,
	FlowAccumulation,
	Wetness,
	SoilDepth,
	GroundType,
	Humidity,
	Temperature,
	Altitude,
	Slope,
	Count
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

	bool IsClassified() const
	{
		return !ClassLabels.IsEmpty();
	}
};

namespace MassifDebugChannels
{
	const TCHAR* GetName(EMassifDebugChannel Channel);

	/**
	 * Renders one channel over the whole world into Texture, at the bake's own
	 * resolution, and fills the legend that reads it. Reuses the texture when it
	 * already has the right size, so switching views does not allocate 25 MB a
	 * keypress.
	 *
	 * Sampling runs across the thread pool: habitat is a pure function of
	 * position, so a row is independent of every other row.
	 */
	void RenderChannel(
		const FMassifHabitatField& Habitat,
		EMassifDebugChannel Channel,
		TObjectPtr<UTexture2D>& Texture,
		FMassifDebugChannelLegend& OutLegend);
}
