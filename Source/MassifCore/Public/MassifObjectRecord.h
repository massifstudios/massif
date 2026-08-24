#pragma once

#include "CoreMinimal.h"

class FMassifObjectTypeRegistry;

/** Nothing is ever this type. A zeroed record is invalid rather than a boulder. */
inline constexpr uint16 MassifInvalidObjectTypeId = 0;

namespace MassifObjectFlags
{
	/** Which mesh variant of the type, 0-15. */
	inline constexpr uint8 VariantMask = 0x0F;

	/** Placed lying down: windthrow the generator put there, not a tree the player cut. */
	inline constexpr uint8 Felled = 1 << 4;

	/** Placed as a standing snag. */
	inline constexpr uint8 DeadStanding = 1 << 5;
}

/**
 * One stored object, exactly as it sits on disk and in memory.
 *
 * Four-byte aligned with no padding, so a chunk is a flat array a reader can
 * copy in one go and a hash can walk as bytes. Keep it that way: a hole in the
 * middle is uninitialised memory reaching the determinism hash.
 *
 * The record is written once, when generation produces it, and never again.
 * Everything that changes during play — a felled tree, a blasted boulder —
 * lives in the sim's sparse overlay instead (OBJECT_STORE.md §7), so that the
 * store stays world data that presentation may read directly.
 */
struct FMassifObjectRecord
{
	/** Absolute world position. Millimetres are uniform everywhere; float metres are not. */
	int32 XMillimeters = 0;
	int32 YMillimeters = 0;
	int32 ZMillimeters = 0;

	/** Index into the type registry, which is where the mesh and the size band come from. */
	uint16 TypeId = MassifInvalidObjectTypeId;

	/** Size at planting, over the type's own band. Meaningless without the registry. */
	uint8 Size = 0;

	uint8 Flags = 0;

	/** Resting orientation, as fractions of a turn. */
	uint16 Yaw = 0;
	uint8 Pitch = 0;
	uint8 Roll = 0;

	/**
	 * When the object came into being, relative to the start of the world.
	 * Signed because every generated tree was planted before then, and every
	 * boulder leaves it zero.
	 */
	int16 PlantedHalfMonths = 0;

	/** 255 healthy, 0 dead. What the generator produced, not what play has done to it. */
	uint8 Vigour = 0;

	/** 0 resting on the ground, 255 level with it. */
	uint8 Buried = 0;
};

static_assert(sizeof(FMassifObjectRecord) == 24, "The record is a save format; its size is part of it.");
static_assert(alignof(FMassifObjectRecord) == 4, "");
static_assert(offsetof(FMassifObjectRecord, TypeId) == 12, "");
static_assert(offsetof(FMassifObjectRecord, Yaw) == 16, "");
static_assert(offsetof(FMassifObjectRecord, PlantedHalfMonths) == 20, "");

/**
 * One object in the units the rest of the code thinks in.
 *
 * The wide side of the record, the way FMassifEnvironmentPoint is the wide side
 * of a point set's storage: producers and consumers work in metres and degrees,
 * and the quantisation is the store's business rather than theirs.
 */
struct FMassifObjectPlacement
{
	FVector3d PositionMeters = FVector3d::ZeroVector;

	uint16 TypeId = MassifInvalidObjectTypeId;

	/** Half the object's extent. Clamps into the type's band on encode. */
	double RadiusMeters = 0.0;

	FRotator Rotation = FRotator::ZeroRotator;

	/** 0 resting on the ground, 1 level with it. */
	double BuriedFraction = 0.0;

	int32 PlantedHalfMonths = 0;

	/** 1 healthy, 0 dead. */
	double Vigour = 1.0;

	int32 VariantIndex = 0;

	bool bFelled = false;
	bool bDeadStanding = false;
};

namespace MassifObjectRecordCodec
{
	/** Millimetres each way, clamped to what an int32 spans — about 2147 km from the origin. */
	MASSIFCORE_API int32 EncodeMillimeters(double Meters);
	MASSIFCORE_API double DecodeMillimeters(int32 Millimeters);

	/** Angles wrap rather than clamp, so 360 degrees and 0 encode to the same value. */
	MASSIFCORE_API uint16 EncodeTurn16(double Degrees);
	MASSIFCORE_API double DecodeTurn16(uint16 Encoded);
	MASSIFCORE_API uint8 EncodeTurn8(double Degrees);
	MASSIFCORE_API double DecodeTurn8(uint8 Encoded);

	MASSIFCORE_API uint8 EncodeFraction8(double Fraction);
	MASSIFCORE_API double DecodeFraction8(uint8 Encoded);

	/**
	 * Packs a placement. False leaves OutRecord untouched and means the type is
	 * not registered, which a caller must not paper over: the size band belongs
	 * to the type, so there is no sensible size to store without one.
	 */
	MASSIFCORE_API bool Encode(
		const FMassifObjectPlacement& Placement, const FMassifObjectTypeRegistry& Registry,
		FMassifObjectRecord& OutRecord);

	/** False for a record whose type has since been retired from the registry. */
	MASSIFCORE_API bool Decode(
		const FMassifObjectRecord& Record, const FMassifObjectTypeRegistry& Registry,
		FMassifObjectPlacement& OutPlacement);
}
