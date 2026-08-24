#include "MassifElevationSource.h"
#include "MassifEnvironmentScheduler.h"
#include "MassifObjectRecord.h"
#include "MassifObjectStore.h"
#include "MassifObjectStoreBuilder.h"
#include "MassifObjectTypeRegistry.h"
#include "MassifRockfallProcesses.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 BuilderTestSeed = 20260824;
	constexpr double BuilderTestWorldMeters = 2048.0;

	/** A cone steep enough to shed, with flat corners for the blocks to run out onto. */
	TUniquePtr<FMassifEnvironmentScheduler> SolveRockfallForStore()
	{
		FMassifAnalyticElevationSource::FParams TerrainParams;
		TerrainParams.Shape = EMassifAnalyticTerrain::Cone;
		TerrainParams.WorldSizeMeters = BuilderTestWorldMeters;
		TerrainParams.BaseAltitudeMeters = 1500.0;
		TerrainParams.ReliefMeters = 1600.0;

		FMassifRockfallParams RockfallParams;
		RockfallParams.CandidatesPerSquareKilometre = 200000.0;
		RockfallParams.TileSizeMeters = 256.0;

		TUniquePtr<FMassifEnvironmentScheduler> Scheduler = MakeUnique<FMassifEnvironmentScheduler>(
			MakeShared<FMassifAnalyticElevationSource>(TerrainParams), FMassifGeoreference(), BuilderTestSeed);
		MassifRegisterRockfallProcesses(*Scheduler, RockfallParams);

		FString Error;
		checkf(Scheduler->BuildSchedule(Error), TEXT("%s"), *Error);
		Scheduler->Execute();
		return Scheduler;
	}

	FMassifObjectTypeRegistry MakeBuilderTestRegistry()
	{
		FMassifObjectTypeRegistry Registry;
		FMassifObjectTypeRegistry::RegisterBuiltinTypes(Registry);
		return Registry;
	}

	/** Decodes one chunk, widening the radius range. Returns how many records decoded. */
	int32 MeasureChunk(
		const FMassifObjectStore& Store, const FMassifObjectTypeRegistry& Registry, int32 Column, int32 Row,
		double& InOutSmallestRadiusMeters, double& InOutLargestRadiusMeters)
	{
		int32 DecodedCount = 0;
		for (const FMassifObjectRecord& Record : Store.GetChunkRecords(Column, Row))
		{
			FMassifObjectPlacement Placement;
			if (MassifObjectRecordCodec::Decode(Record, Registry, Placement))
			{
				InOutSmallestRadiusMeters = FMath::Min(InOutSmallestRadiusMeters, Placement.RadiusMeters);
				InOutLargestRadiusMeters = FMath::Max(InOutLargestRadiusMeters, Placement.RadiusMeters);
				++DecodedCount;
			}
		}
		return DecodedCount;
	}

	int32 MeasureRow(
		const FMassifObjectStore& Store, const FMassifObjectTypeRegistry& Registry, int32 Row,
		double& InOutSmallestRadiusMeters, double& InOutLargestRadiusMeters)
	{
		int32 DecodedCount = 0;
		for (int32 Column = 0; Column < Store.GetChunkGrid().Width; ++Column)
		{
			DecodedCount += MeasureChunk(
				Store, Registry, Column, Row, InOutSmallestRadiusMeters, InOutLargestRadiusMeters);
		}
		return DecodedCount;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectStoreBuilderTest,
	"Massif.WorldGen.ObjectStore.DrainsRockfall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectStoreBuilderTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = SolveRockfallForStore();
	const FMassifEnvironmentState& Environment = Scheduler->GetState();
	const FMassifObjectTypeRegistry Registry = MakeBuilderTestRegistry();

	const FMassifPointSetHandle Placements =
		Environment.FindPointSet(MassifEnvironmentPointSets::RockPlacements());
	const int32 SolvedCount = Environment.GetPointCount(Placements);
	TestTrue(TEXT("The fall placed blocks to drain"), SolvedCount > 0);

	const FMassifObjectStore Store =
		MassifObjectStoreBuilder::BuildFromEnvironment(Environment, Registry);

	// Losing blocks between the solve and the store is the failure that would
	// otherwise show up only as a thinner scree slope than the solve reported.
	TestEqual(TEXT("Every solved block reached the store"), Store.GetRecordCount(), SolvedCount);
	TestTrue(TEXT("The store is finalized"), Store.IsFinalized());

	// The store's chunk grid covers the same world the solve ran over, or the
	// blocks near an edge clamp into the chunk next to the one they belong in.
	const FMassifObjectChunkGrid& ChunkGrid = Store.GetChunkGrid();
	TestEqual(TEXT("The chunk grid spans the world"),
		ChunkGrid.Width * ChunkGrid.ChunkSizeMeters, BuilderTestWorldMeters, 0.001);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassifObjectStoreBuilderFidelityTest,
	"Massif.WorldGen.ObjectStore.CarriesBlockShape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMassifObjectStoreBuilderFidelityTest::RunTest(const FString& Parameters)
{
	const TUniquePtr<FMassifEnvironmentScheduler> Scheduler = SolveRockfallForStore();
	const FMassifEnvironmentState& Environment = Scheduler->GetState();
	const FMassifObjectTypeRegistry Registry = MakeBuilderTestRegistry();
	const FMassifObjectStore Store =
		MassifObjectStoreBuilder::BuildFromEnvironment(Environment, Registry);

	const FMassifPointSetHandle Placements =
		Environment.FindPointSet(MassifEnvironmentPointSets::RockPlacements());
	const FVector2d SolvedRadiusRange = Environment.GetPointSetDescriptor(Placements).RadiusRangeMeters;

	double SmallestRadiusMeters = TNumericLimits<double>::Max();
	double LargestRadiusMeters = 0.0;
	int32 DecodedCount = 0;
	for (int32 Row = 0; Row < Store.GetChunkGrid().Height; ++Row)
	{
		DecodedCount += MeasureRow(Store, Registry, Row, SmallestRadiusMeters, LargestRadiusMeters);
	}

	TestEqual(TEXT("Every record decodes against the builtin registry"), DecodedCount, Store.GetRecordCount());

	// The fall sorts blocks by size down the cone, so a store that flattened the
	// size byte would still hold the right count and draw a slope of clones.
	TestTrue(FString::Printf(TEXT("Stored radii span the solve's range (%.2f-%.2f m of %.2f-%.2f m)"),
		SmallestRadiusMeters, LargestRadiusMeters, SolvedRadiusRange.X, SolvedRadiusRange.Y),
		LargestRadiusMeters - SmallestRadiusMeters > 0.5 * (SolvedRadiusRange.Y - SolvedRadiusRange.X));

	TestTrue(TEXT("No stored radius escapes the type's band"),
		SmallestRadiusMeters >= SolvedRadiusRange.X - 0.05 && LargestRadiusMeters <= SolvedRadiusRange.Y + 0.05);

	return true;
}
