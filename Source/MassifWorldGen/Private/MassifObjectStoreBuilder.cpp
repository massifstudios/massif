#include "MassifObjectStoreBuilder.h"

#include "MassifEnvironmentChannel.h"
#include "MassifEnvironmentPointSet.h"
#include "MassifEnvironmentState.h"
#include "MassifObjectStore.h"
#include "MassifObjectTypeRegistry.h"
#include "MassifRockfallProcesses.h"
#include "MassifWorldGenLog.h"

namespace
{
	using MassifObjectStoreBuilder::FPointSetMapping;

	/** How the point sets the world passes write become object types. */
	TArray<FPointSetMapping> MakeBuiltinMappings()
	{
		FPointSetMapping Rocks;
		Rocks.PointSetName = MassifEnvironmentPointSets::RockPlacements();
		Rocks.TypeIdsByClass = { MassifObjectTypes::GraniteBoulder };

		return { Rocks };
	}

	/**
	 * A point set whose radii reach outside the type's band would be silently
	 * clamped into it, which shows up as a scree slope of identically sized
	 * blocks rather than as an error.
	 */
	void WarnOnMappingMismatch(
		const FMassifPointSetDescriptor& Descriptor, int32 ClassIndex, uint16 TypeId,
		const FMassifObjectTypeRegistry& Registry)
	{
		const FMassifObjectTypeInfo* TypeInfo = Registry.FindTypeInfo(TypeId);
		if (TypeInfo == nullptr)
		{
			UE_LOG(LogMassifWorldGen, Warning,
				TEXT("Point set '%s' class %d maps to unregistered object type %u; its objects are dropped."),
				*Descriptor.Name.ToString(), ClassIndex, TypeId);
			return;
		}

		const bool bFits = Descriptor.RadiusRangeMeters.X >= TypeInfo->SizeBandMeters.X - UE_DOUBLE_KINDA_SMALL_NUMBER
			&& Descriptor.RadiusRangeMeters.Y <= TypeInfo->SizeBandMeters.Y + UE_DOUBLE_KINDA_SMALL_NUMBER;
		if (bFits)
		{
			return;
		}

		UE_LOG(LogMassifWorldGen, Warning,
			TEXT("Point set '%s' class %d produces radii %.2f-%.2f m, outside type '%s' band %.2f-%.2f m; ")
			TEXT("sizes outside the band clamp."),
			*Descriptor.Name.ToString(), ClassIndex,
			Descriptor.RadiusRangeMeters.X, Descriptor.RadiusRangeMeters.Y,
			*TypeInfo->Name.ToString(), TypeInfo->SizeBandMeters.X, TypeInfo->SizeBandMeters.Y);
	}

	FMassifObjectPlacement MakePlacement(const FMassifEnvironmentPoint& Point, uint16 TypeId)
	{
		FMassifObjectPlacement Placement;
		Placement.PositionMeters = Point.PositionMeters;
		Placement.TypeId = TypeId;
		Placement.RadiusMeters = Point.RadiusMeters;
		Placement.Rotation = FRotator(Point.RotationDegrees.Y, Point.RotationDegrees.X, Point.RotationDegrees.Z);
		Placement.BuriedFraction = Point.BuriedFraction;
		return Placement;
	}

	int32 DrainTile(
		TConstArrayView<FMassifEnvironmentPoint> Points, TConstArrayView<uint16> TypeIdsByClass,
		const FMassifObjectTypeRegistry& Registry, FMassifObjectStore& Store)
	{
		int32 AddedCount = 0;
		for (const FMassifEnvironmentPoint& Point : Points)
		{
			const bool bMapped = TypeIdsByClass.IsValidIndex(Point.ClassIndex);
			if (bMapped && Store.AddPlacement(MakePlacement(Point, TypeIdsByClass[Point.ClassIndex]), Registry))
			{
				++AddedCount;
			}
		}
		return AddedCount;
	}
}

int32 MassifObjectStoreBuilder::DrainPointSet(
	const FMassifEnvironmentState& Environment, const FPointSetMapping& Mapping,
	const FMassifObjectTypeRegistry& Registry, FMassifObjectStore& Store)
{
	const FMassifPointSetHandle Handle = Environment.FindPointSet(Mapping.PointSetName);
	if (!Handle.IsValid() || !Environment.IsPointSetWritten(Handle))
	{
		return 0;
	}

	const FMassifPointSetDescriptor& Descriptor = Environment.GetPointSetDescriptor(Handle);
	for (int32 ClassIndex = 0; ClassIndex < Mapping.TypeIdsByClass.Num(); ++ClassIndex)
	{
		WarnOnMappingMismatch(Descriptor, ClassIndex, Mapping.TypeIdsByClass[ClassIndex], Registry);
	}

	// Row-major over the buckets, which is what makes the drain reproducible
	// before Finalize sorts it, and keeps a chunk's records arriving together.
	const FMassifChannelGrid& TileGrid = Environment.GetPointSetTileGrid(Handle);
	TArray<FMassifEnvironmentPoint> TilePoints;
	int32 AddedCount = 0;
	for (int32 TileIndex = 0; TileIndex < TileGrid.CellCount(); ++TileIndex)
	{
		Environment.ReadTilePoints(Handle, TileIndex % TileGrid.Width, TileIndex / TileGrid.Width, TilePoints);
		AddedCount += DrainTile(TilePoints, Mapping.TypeIdsByClass, Registry, Store);
	}
	return AddedCount;
}

FMassifObjectStore MassifObjectStoreBuilder::MakeEmptyStore(const FMassifEnvironmentState& Environment)
{
	return FMassifObjectStore(FMassifObjectChunkGrid::FromWorldSize(Environment.GetWorldSizeMeters()));
}

void MassifObjectStoreBuilder::FillFromEnvironment(
	const FMassifEnvironmentState& Environment, const FMassifObjectTypeRegistry& Registry,
	FMassifObjectStore& Store)
{
	for (const FPointSetMapping& Mapping : MakeBuiltinMappings())
	{
		const int32 AddedCount = DrainPointSet(Environment, Mapping, Registry, Store);
		UE_LOG(LogMassifWorldGen, Log, TEXT("Object store: %d objects from '%s'."),
			AddedCount, *Mapping.PointSetName.ToString());
	}
}

FMassifObjectStore MassifObjectStoreBuilder::BuildFromEnvironment(
	const FMassifEnvironmentState& Environment, const FMassifObjectTypeRegistry& Registry)
{
	FMassifObjectStore Store = MakeEmptyStore(Environment);
	FillFromEnvironment(Environment, Registry, Store);
	Store.Finalize();
	return Store;
}
