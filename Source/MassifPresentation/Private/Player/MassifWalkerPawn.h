#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "MassifWalkerPawn.generated.h"

class FMassifTerrainField;
class UCameraComponent;
struct FInputActionValue;

/**
 * A person on the mountain: capsule, eye-level camera, earth gravity. Walks on
 * the collision the terrain quadtree cooks for the tiles around the player, so
 * it stands on the same ground that is drawn.
 *
 * Purely presentation. Nothing here is sim state: the walker is a camera with
 * legs until a guest or a player avatar exists in the sim.
 */
UCLASS()
class AMassifWalkerPawn : public ACharacter
{
	GENERATED_BODY()

public:
	AMassifWalkerPawn();

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Height of the eye above the capsule's feet, for placing the pawn on the ground. */
	double GetEyeHeightMeters() const;

	/** Ground to fall back to while the terrain tile under the pawn has no collision yet. */
	void SetTerrainField(TSharedPtr<const FMassifTerrainField> InField);

private:
	/** Walking and running speed in centimetres per second, and the jump that goes with them. */
	static constexpr float WalkSpeed = 300.0f;
	static constexpr float RunSpeed = 650.0f;
	static constexpr float JumpSpeed = 420.0f;

	/** Mountain footing: steeper than the engine default, which gives up at 44 degrees. */
	static constexpr float WalkableFloorAngleDegrees = 55.0f;

	/** How far below the field the pawn may sink before it is put back on the surface. */
	static constexpr double MaxSinkMeters = 3.0;

	UPROPERTY()
	TObjectPtr<UCameraComponent> Camera;

	TSharedPtr<const FMassifTerrainField> TerrainField;

	void ApplyMove(const FInputActionValue& Value);
	void ApplyLook(const FInputActionValue& Value);
	void StartJump();
	void StopJump();
	void StartRun();
	void StopRun();

	/** Puts the pawn back on the field when it has fallen through terrain that has no collision. */
	void CatchFallThroughGround();
};
