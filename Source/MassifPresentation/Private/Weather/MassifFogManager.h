#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Weather/MassifFogField.h"
#include "MassifFogManager.generated.h"

class FMassifEnvironmentState;
class FMassifTerrainField;
class ULocalFogVolumeComponent;

/**
 * Draws the fog the field describes (`MassifFogField.h`).
 *
 * Fog is a lake with a flat top, which is a shape a single global height fog
 * cannot make: the lid is the same altitude everywhere, but whether there is
 * anything under it is not. So the world near the camera is walked on a coarse
 * grid, the field is asked what each cell holds, and the cells that hold
 * anything get a local fog volume sized to the column between the ground and the
 * lid.
 *
 * Only near the camera. Fog a kilometre off reads as the distance haze the
 * exponential height fog already draws, and the volumes are worth spending where
 * the player can see into them.
 *
 * Purely presentation: owns no gameplay state, and drives nothing but its own
 * components.
 */
UCLASS()
class AMassifFogManager : public AActor
{
	GENERATED_BODY()

public:
	AMassifFogManager();

	void Initialize(
		TSharedRef<const FMassifEnvironmentState> InEnvironment,
		TSharedPtr<const FMassifTerrainField> InTerrainField, double InLatitudeDegrees);

	virtual void Tick(float DeltaSeconds) override;

private:
	/** How far out the volumes are placed. Past it the distance haze carries the reading. */
	static constexpr double VisibleRadiusMeters = 2400.0;

	/** Below this the cell is not worth a volume, and the haze covers what is left. */
	static constexpr double MinimumDrawnDensity = 0.01;

	/** Shallowest column worth drawing, so a cell just under the lid does not flicker. */
	static constexpr double MinimumColumnMeters = 15.0;

	/**
	 * Volumes kept alive. Tile culling admits 32 to a screen tile, so heavy overlap
	 * is affordable, but the total still wants a backstop.
	 */
	static constexpr int32 MaxVolumeCount = 400;

	/** Times a second the fog is re-solved. It moves on the hour, not on the frame. */
	static constexpr double UpdatesPerSecond = 4.0;

	void SolveFog(FVector2d CameraPositionMeters);
	void LogSolve(int32 DrawnVolumeCount);
	int32 PlaceRow(
		FIntPoint CentreCell, int32 OffsetY, int32 CellRadius, const FMassifSunPosition& Sun,
		int32 VolumeIndex);
	int32 PlaceCell(FVector2d CellCentreMeters, const FMassifSunPosition& Sun, int32 VolumeIndex);
	ULocalFogVolumeComponent* GetOrCreateVolume(int32 VolumeIndex);
	void HideVolumesFrom(int32 FirstUnusedIndex);

	/** Where the camera is, in metres, or false where there is no view yet. */
	bool TryGetCameraPositionMeters(FVector2d& OutPositionMeters) const;

	TSharedPtr<const FMassifEnvironmentState> Environment;
	TSharedPtr<const FMassifTerrainField> TerrainField;
	TUniquePtr<FMassifFogField> Field;

	FMassifFogParams Params;
	FMassifFogConditions Conditions;

	double LatitudeDegrees = 47.2;
	double FloorAltitudeMeters = 0.0;
	double SecondsSinceSolve = 0.0;
	int32 LastLoggedVolumeCount = -1;

	UPROPERTY()
	TArray<TObjectPtr<ULocalFogVolumeComponent>> Volumes;
};
