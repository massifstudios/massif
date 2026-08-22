#pragma once

#include "CoreMinimal.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "MassifUnits.h"

/** Where the debug tools mean by "here" when they are not told a position. */
namespace MassifDebugCamera
{
	inline FVector2d GetPositionMeters(const UWorld* World)
	{
		if (World == nullptr)
		{
			return FVector2d::ZeroVector;
		}

		const APlayerController* PlayerController = World->GetFirstPlayerController();
		if (PlayerController == nullptr || PlayerController->PlayerCameraManager == nullptr)
		{
			return FVector2d::ZeroVector;
		}

		const FVector LocationCentimeters = PlayerController->PlayerCameraManager->GetCameraLocation();
		return FVector2d(LocationCentimeters.X, LocationCentimeters.Y) * MassifUnits::CentimetersToMeters;
	}
}
