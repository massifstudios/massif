#pragma once

#include "CoreMinimal.h"

/**
 * One object the environment solve placed, in absolute world metres.
 *
 * Wide like a channel's write scratch is wide: a process solves in double and
 * the descriptor decides what is kept, so precision is a property of the point
 * set rather than of every process that fills one.
 */
struct FMassifEnvironmentPoint
{
	FVector3d PositionMeters = FVector3d::ZeroVector;

	/** Half the object's extent. A point set holds no object without one, so zero drops the slot. */
	double RadiusMeters = 0.0;

	/** Resting orientation. Degrees, applied yaw then pitch then roll. */
	FVector3d RotationDegrees = FVector3d::ZeroVector;

	/** How far the object sits into the ground: 0 resting on it, 1 level with it. */
	double BuriedFraction = 0.0;

	/** Which of the descriptor's classes this is. Meaning belongs to the process that wrote it. */
	int32 ClassIndex = 0;
};

/**
 * A set of discrete objects over the world, and everything needed to store them.
 *
 * The second thing an environment process can produce. A channel answers what
 * the ground is like at a position; a point set answers what is standing on it,
 * which no grid can hold: two boulders in one cell are two boulders, and the
 * one that rolled further is not an average of the two.
 */
struct FMassifPointSetDescriptor
{
	FName Name;  // "Geology.RockPlacements"

	/**
	 * Side of one storage bucket. Readers ask for objects a bucket at a time, so
	 * this is what decides how much a consumer pulls in to draw one patch of
	 * ground; 512 m matches the resident tiles (ARCHITECTURE §5.4).
	 */
	double TileSizeMeters = 512.0;

	/** Radius band the stored quantisation covers. Anything outside it clamps. */
	FVector2d RadiusRangeMeters = FVector2d(0.1, 10.0);

	/** How many classes ClassIndex selects between, and what to call them. */
	TArray<FString> ClassNames;

	const TCHAR* Description = TEXT("");

	MASSIFWORLDGEN_API bool operator==(const FMassifPointSetDescriptor& Other) const;

	bool operator!=(const FMassifPointSetDescriptor& Other) const
	{
		return !(*this == Other);
	}
};

/** Resolved point-set index, so a consumer pulling tile after tile resolves the name once. */
struct FMassifPointSetHandle
{
	int32 Index = INDEX_NONE;

	bool IsValid() const
	{
		return Index != INDEX_NONE;
	}

	bool operator==(const FMassifPointSetHandle& Other) const
	{
		return Index == Other.Index;
	}
};
