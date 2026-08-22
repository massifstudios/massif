#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassifDebugChannels.h"
#include "MassifDebugChannelView.generated.h"

class AMassifTerrainManager;
class FMassifHabitatField;
class UTexture2D;

/**
 * Paints one generated channel onto the terrain in place of its material, so a
 * simulation whose output is otherwise only visible as a second-order effect on
 * where trees ended up can be looked at directly (ARCHITECTURE §5.7).
 *
 * The channel is uploaded as one world-sized texture and sampled by world XY in
 * the terrain material, so nothing about the mesh, the quadtree or the field
 * changes when a view is switched on. Purely presentation: it reads the bake
 * and the habitat, and writes nothing back.
 *
 * One texture is kept and re-uploaded on every switch rather than one per
 * channel, since a 20 km world at bake resolution is 25 MB apiece.
 */
UCLASS()
class AMassifDebugChannelView : public AActor
{
	GENERATED_BODY()

public:
	AMassifDebugChannelView();

	void Initialize(TSharedPtr<const FMassifHabitatField> InHabitat, AMassifTerrainManager* InTerrainManager);

	/** The one in the world, for the console tools that need what it holds. Null before world gen. */
	static AMassifDebugChannelView* Find(const UObject* WorldContext);

	/**
	 * The view the world is currently painted with, held in the massif.DebugChannel
	 * console variable so the number keys and the console cannot disagree about
	 * which view is on.
	 */
	static void RequestChannel(EMassifDebugChannel Channel);

	virtual void Tick(float DeltaSeconds) override;

	EMassifDebugChannel GetChannel() const
	{
		return CurrentChannel;
	}

	/** Slice of the year the painted view is showing. Always 0 for a view holding one. */
	int32 GetTimeSlice() const
	{
		return CurrentTimeSlice;
	}

	/** Slices the selected view holds, so the picker knows whether to offer a scrubber. */
	int32 GetSelectedTimeSliceCount() const;

	const FMassifDebugChannelLegend& GetLegend() const
	{
		return Legend;
	}

	const TSharedPtr<const FMassifHabitatField>& GetHabitat() const
	{
		return Habitat;
	}

	/** Every selectable view, built from the channel registry once the environment is up. */
	const TArray<FMassifDebugView>& GetViews() const
	{
		return Views;
	}

private:
	/** Renders the view and hands it to the terrain, or restores the terrain material for Off. */
	void ApplyChannel(EMassifDebugChannel Channel, int32 TimeSlice);

	void EnsureViews();

	int32 ClampRequestedSlice(int32 RequestedChannel) const;

	TSharedPtr<const FMassifHabitatField> Habitat;

	UPROPERTY()
	TObjectPtr<AMassifTerrainManager> TerrainManager;

	/** Holds the uploaded channel against garbage collection; transient, never saved. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> ChannelTexture;

	TArray<FMassifDebugView> Views;
	EMassifDebugChannel CurrentChannel = EMassifDebugChannel::Off;
	int32 CurrentTimeSlice = 0;
	FMassifDebugChannelLegend Legend;
};
