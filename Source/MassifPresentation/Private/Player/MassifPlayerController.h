#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "MassifPlayerController.generated.h"

class AMassifWalkerPawn;
class UInputMappingContext;
class UMassifInputConfig;

UENUM()
enum class EMassifViewMode : uint8
{
	Fly,
	Walk
};

/**
 * Owns the two ways of looking at the mountain and the switch between them:
 * the free-flying management camera the game starts in, and a walker on the
 * ground. V toggles.
 *
 * Each view keeps its own place. Switching to the walker drops it where the
 * camera was looking from, onto the ground under that point; switching back
 * lifts the camera to where the walker's eyes were.
 */
UCLASS()
class AMassifPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AMassifPlayerController();

	/**
	 * The input config of whichever controller owns this pawn, or null if it has
	 * none. Pawns bind their input when they are possessed, which happens before
	 * the controller's BeginPlay, so the config builds on first use rather than
	 * at a point in the startup order that callers would have to know about.
	 */
	static UMassifInputConfig* GetInputConfig(AController* Controller);

	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;

	EMassifViewMode GetViewMode() const
	{
		return ViewMode;
	}

private:
	UPROPERTY()
	TObjectPtr<UMassifInputConfig> InputConfig;

	/** The flying pawn the game mode spawned, kept alive while the walker is possessed. */
	UPROPERTY()
	TObjectPtr<APawn> FlyPawn;

	UPROPERTY()
	TObjectPtr<AMassifWalkerPawn> WalkerPawn;

	EMassifViewMode ViewMode = EMassifViewMode::Fly;

	UMassifInputConfig* EnsureInputConfig();

	/** Adds the context for the current view, and the always-on one, to the local player. */
	void ApplyMappingContexts();

	void ToggleViewMode();
	void EnterWalkMode();
	void EnterFlyMode();

	/** Spawns the walker on first use, standing on the ground below the camera. */
	AMassifWalkerPawn* AcquireWalkerPawn();
};
