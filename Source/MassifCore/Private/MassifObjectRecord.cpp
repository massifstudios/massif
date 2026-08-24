#include "MassifObjectRecord.h"

#include "MassifObjectTypeRegistry.h"

namespace
{
	constexpr double MillimetersPerMeter = 1000.0;
	constexpr double DegreesPerTurn = 360.0;

	/** Where in the band a radius falls, 0 at its floor and 1 at its ceiling. */
	double SizeFractionOf(double RadiusMeters, FVector2d SizeBandMeters)
	{
		const double Span = SizeBandMeters.Y - SizeBandMeters.X;
		if (Span <= 0.0)
		{
			return 0.0;
		}
		return (RadiusMeters - SizeBandMeters.X) / Span;
	}
}

int32 MassifObjectRecordCodec::EncodeMillimeters(double Meters)
{
	const int64 Millimeters = FMath::RoundToInt64(Meters * MillimetersPerMeter);
	return static_cast<int32>(FMath::Clamp<int64>(Millimeters, MIN_int32, MAX_int32));
}

double MassifObjectRecordCodec::DecodeMillimeters(int32 Millimeters)
{
	return static_cast<double>(Millimeters) / MillimetersPerMeter;
}

uint16 MassifObjectRecordCodec::EncodeTurn16(double Degrees)
{
	const double Turns = Degrees / DegreesPerTurn;
	const double Fraction = Turns - FMath::Floor(Turns);

	// Wrapping rather than clamping: a fraction that rounds up to a whole turn is
	// zero degrees, and clamping it would put it a step short of where it started.
	return static_cast<uint16>(FMath::RoundToInt64(Fraction * 65536.0) & 0xFFFF);
}

double MassifObjectRecordCodec::DecodeTurn16(uint16 Encoded)
{
	return static_cast<double>(Encoded) * DegreesPerTurn / 65536.0;
}

uint8 MassifObjectRecordCodec::EncodeTurn8(double Degrees)
{
	const double Turns = Degrees / DegreesPerTurn;
	const double Fraction = Turns - FMath::Floor(Turns);
	return static_cast<uint8>(FMath::RoundToInt64(Fraction * 256.0) & 0xFF);
}

double MassifObjectRecordCodec::DecodeTurn8(uint8 Encoded)
{
	return static_cast<double>(Encoded) * DegreesPerTurn / 256.0;
}

uint8 MassifObjectRecordCodec::EncodeFraction8(double Fraction)
{
	return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(Fraction * 255.0), 0, 255));
}

double MassifObjectRecordCodec::DecodeFraction8(uint8 Encoded)
{
	return static_cast<double>(Encoded) / 255.0;
}

bool MassifObjectRecordCodec::Encode(
	const FMassifObjectPlacement& Placement, const FMassifObjectTypeRegistry& Registry,
	FMassifObjectRecord& OutRecord)
{
	const FMassifObjectTypeInfo* TypeInfo = Registry.FindTypeInfo(Placement.TypeId);
	if (TypeInfo == nullptr)
	{
		return false;
	}

	FMassifObjectRecord Record;
	Record.XMillimeters = EncodeMillimeters(Placement.PositionMeters.X);
	Record.YMillimeters = EncodeMillimeters(Placement.PositionMeters.Y);
	Record.ZMillimeters = EncodeMillimeters(Placement.PositionMeters.Z);
	Record.TypeId = Placement.TypeId;
	Record.Size = EncodeFraction8(SizeFractionOf(Placement.RadiusMeters, TypeInfo->SizeBandMeters));
	Record.Flags = static_cast<uint8>(Placement.VariantIndex) & MassifObjectFlags::VariantMask;
	Record.Yaw = EncodeTurn16(Placement.Rotation.Yaw);
	Record.Pitch = EncodeTurn8(Placement.Rotation.Pitch);
	Record.Roll = EncodeTurn8(Placement.Rotation.Roll);
	Record.PlantedHalfMonths =
		static_cast<int16>(FMath::Clamp(Placement.PlantedHalfMonths, MIN_int16, MAX_int16));
	Record.Vigour = EncodeFraction8(Placement.Vigour);
	Record.Buried = EncodeFraction8(Placement.BuriedFraction);

	if (Placement.bFelled)
	{
		Record.Flags |= MassifObjectFlags::Felled;
	}
	if (Placement.bDeadStanding)
	{
		Record.Flags |= MassifObjectFlags::DeadStanding;
	}

	OutRecord = Record;
	return true;
}

bool MassifObjectRecordCodec::Decode(
	const FMassifObjectRecord& Record, const FMassifObjectTypeRegistry& Registry,
	FMassifObjectPlacement& OutPlacement)
{
	const FMassifObjectTypeInfo* TypeInfo = Registry.FindTypeInfo(Record.TypeId);
	if (TypeInfo == nullptr)
	{
		return false;
	}

	const FVector2d SizeBandMeters = TypeInfo->SizeBandMeters;

	FMassifObjectPlacement Placement;
	Placement.PositionMeters = FVector3d(
		DecodeMillimeters(Record.XMillimeters),
		DecodeMillimeters(Record.YMillimeters),
		DecodeMillimeters(Record.ZMillimeters));
	Placement.TypeId = Record.TypeId;
	Placement.RadiusMeters = FMath::Lerp(SizeBandMeters.X, SizeBandMeters.Y, DecodeFraction8(Record.Size));
	Placement.Rotation = FRotator(DecodeTurn8(Record.Pitch), DecodeTurn16(Record.Yaw), DecodeTurn8(Record.Roll));
	Placement.BuriedFraction = DecodeFraction8(Record.Buried);
	Placement.PlantedHalfMonths = Record.PlantedHalfMonths;
	Placement.Vigour = DecodeFraction8(Record.Vigour);
	Placement.VariantIndex = Record.Flags & MassifObjectFlags::VariantMask;
	Placement.bFelled = (Record.Flags & MassifObjectFlags::Felled) != 0;
	Placement.bDeadStanding = (Record.Flags & MassifObjectFlags::DeadStanding) != 0;

	OutPlacement = Placement;
	return true;
}
