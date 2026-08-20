#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SpectatorPawn.h"
#include "MassifFlyPawn.generated.h"

struct FInputActionValue;

/**
 * The management camera: free flight over the massif, no collision, no gravity.
 * Fast enough to cross the twenty kilometre world in about half a minute, with
 * a boost for when that is still too slow.
 */
UCLASS()
class AMassifFlyPawn : public ASpectatorPawn
{
	GENERATED_BODY()

public:
	AMassifFlyPawn();

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

private:
	/** Cruise and boost speeds in centimetres per second. */
	static constexpr float CruiseSpeed = 60000.0f;
	static constexpr float BoostSpeed = 240000.0f;

	void ApplyMove(const FInputActionValue& Value);
	void ApplyLook(const FInputActionValue& Value);
	void ApplyVertical(const FInputActionValue& Value);
	void StartBoost();
	void StopBoost();

	void SetMaxSpeed(float MaxSpeed);
};
