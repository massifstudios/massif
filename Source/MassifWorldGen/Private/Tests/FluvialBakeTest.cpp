#include "MassifFluvialBake.h"
#include "MassifSeed.h"
#include "MassifUpliftField.h"
#include "Misc/AutomationTest.h"
#include "WorldGenConfig.h"

namespace
{
	/** A world small enough to solve in a test, keeping the scale relationships of the real one. */
	FWorldGenConfig MakeBakeTestConfig(int32 MasterSeed)
	{
		FWorldGenConfig Config;
		Config.MasterSeed = MasterSeed;
		Config.WorldSizeMeters = 24000.0f;
		Config.RangeSpacingMeters = 9000.0f;
		Config.RangeWarpWavelengthMeters = 12000.0f;
		Config.RangeWarpMeters = 2500.0f;
		Config.BakeCellSizeMeters = 64.0f;
		Config.BakeMaxIterationCount = 30;
		Config.RiverDrainageAreaThresholdSquareMeters = 1.0e6f;
		// Probes below compare grid cells against samples at their unwarped
		// positions, so the sampling warp stays off here. It has no effect on
		// the solve itself.
		Config.BakeSampleWarpMeters = 0.0f;
		return Config;
	}

	FMassifFluvialBake BuildBake(const FWorldGenConfig& Config)
	{
		const uint32 UpliftSeed = FMassifSeed(static_cast<uint32>(Config.MasterSeed)).MakeChannelSeed(TEXT("WorldGen.Uplift"));
		const FMassifUpliftField Uplift(Config, UpliftSeed);
		return FMassifFluvialBake::Build(Config, Uplift);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFluvialBakeDeterminismTest,
	"Massif.WorldGen.FluvialBake.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFluvialBakeDeterminismTest::RunTest(const FString& Parameters)
{
	const FWorldGenConfig Config = MakeBakeTestConfig(1337);
	const uint64 FirstHash = BuildBake(Config).ComputeGridHash();
	const uint64 SecondHash = BuildBake(Config).ComputeGridHash();
	const uint64 OtherSeedHash = BuildBake(MakeBakeTestConfig(99)).ComputeGridHash();

	TestEqual(TEXT("Same seed solves to the same surface"), SecondHash, FirstHash);
	TestNotEqual(TEXT("A different seed solves to different ground"), OtherSeedHash, FirstHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFluvialBakeTalusTest,
	"Massif.WorldGen.FluvialBake.Talus",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFluvialBakeTalusTest::RunTest(const FString& Parameters)
{
	const FWorldGenConfig Config = MakeBakeTestConfig(1337);
	const FMassifFluvialBake Bake = BuildBake(Config);
	const TArray<double>& Heights = Bake.GetHeightGridMeters();
	const int32 CellsPerSide = Bake.GetCellsPerSide();
	const double CellSizeMeters = Config.WorldSizeMeters / (CellsPerSide - 1);

	// The landslide limit is what keeps the fine grid from growing a knob field:
	// no interior cell may tower over an adjacent one much beyond the talus
	// slope. The clamp acts on the slope-weighted blend of the lower neighbours
	// over up-to-diagonal distances, and smoothing shifts heights after the last
	// clamp, so single-neighbour drops legitimately reach ~1.5x the talus slope;
	// 2x headroom still fails the unclamped regime, which stood past 4x. Border
	// cells are pinned at base level and get a pass.
	const double AllowedDropPerMeter = static_cast<double>(Config.BakeTalusSlope) * 2.0;
	double SteepestDropPerMeter = 0.0;
	for (int32 CellY = 1; CellY < CellsPerSide - 2; ++CellY)
	{
		for (int32 CellX = 1; CellX < CellsPerSide - 2; ++CellX)
		{
			const int32 CellIndex = CellY * CellsPerSide + CellX;
			const double EastDrop = FMath::Abs(Heights[CellIndex] - Heights[CellIndex + 1]);
			const double NorthDrop = FMath::Abs(Heights[CellIndex] - Heights[CellIndex + CellsPerSide]);
			SteepestDropPerMeter = FMath::Max(SteepestDropPerMeter, FMath::Max(EastDrop, NorthDrop) / CellSizeMeters);
		}
	}
	TestTrue(FString::Printf(TEXT("No cell stands past the talus limit over its neighbour (%.2f of %.2f rise per meter)"),
		SteepestDropPerMeter, AllowedDropPerMeter), SteepestDropPerMeter <= AllowedDropPerMeter);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFluvialBakeDrainageTest,
	"Massif.WorldGen.FluvialBake.Drainage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFluvialBakeDrainageTest::RunTest(const FString& Parameters)
{
	const FWorldGenConfig Config = MakeBakeTestConfig(1337);
	const FMassifFluvialBake Bake = BuildBake(Config);
	const TArray<int32>& Receivers = Bake.GetReceiverIndices();
	const TArray<double>& DrainageAreas = Bake.GetDrainageAreasSquareMeters();
	const int32 CellCount = Bake.GetCellsPerSide() * Bake.GetCellsPerSide();

	// The property everything downstream of the solve rests on: water leaves
	// the world from every cell. A cycle or an interior sink here means the
	// pit removal failed, and no later stage can repair that.
	for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
	{
		int32 Cursor = CellIndex;
		int32 StepsLeft = CellCount;
		while (Receivers[Cursor] != Cursor && StepsLeft > 0)
		{
			Cursor = Receivers[Cursor];
			--StepsLeft;
		}
		if (!Bake.IsBorderCell(Cursor))
		{
			AddError(FString::Printf(TEXT("Cell %d does not drain to the world border"), CellIndex));
			return false;
		}
	}

	// Flow spreads over several neighbours, so area is not monotone along the
	// steepest-descent chain; what must hold instead is conservation — every
	// cell's area ends up at the border, nowhere else.
	double TotalAreaSum = 0.0;
	double BorderAreaSum = 0.0;
	const double CellAreaSquareMeters = static_cast<double>(Config.BakeCellSizeMeters) * Config.BakeCellSizeMeters;
	for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
	{
		TotalAreaSum += CellAreaSquareMeters;
		if (Bake.IsBorderCell(CellIndex))
		{
			BorderAreaSum += DrainageAreas[CellIndex];
		}
	}
	const double ConservationError = FMath::Abs(BorderAreaSum - TotalAreaSum) / TotalAreaSum;
	TestTrue(FString::Printf(TEXT("All drained area reaches the border (error %.4f%%)"), 100.0 * ConservationError),
		ConservationError < 0.001);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFluvialBakeSurfaceTest,
	"Massif.WorldGen.FluvialBake.Surface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFluvialBakeSurfaceTest::RunTest(const FString& Parameters)
{
	const FWorldGenConfig Config = MakeBakeTestConfig(1337);
	const FMassifFluvialBake Bake = BuildBake(Config);
	const TArray<double>& Heights = Bake.GetHeightGridMeters();
	const int32 CellCount = Bake.GetCellsPerSide() * Bake.GetCellsPerSide();

	const double AltitudeFloor = Config.MinAltitudeMeters - 1.0;
	const double AltitudeCeiling = Config.MaxAltitudeMeters + 1.0;
	for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
	{
		const double Height = Heights[CellIndex];
		if (!FMath::IsFinite(Height) || Height < AltitudeFloor || Height > AltitudeCeiling)
		{
			AddError(FString::Printf(TEXT("Cell %d solved to %f, outside the altitude range"), CellIndex, Height));
			return false;
		}
		if (Bake.IsBorderCell(CellIndex) && Height != static_cast<double>(Config.MinAltitudeMeters))
		{
			AddError(FString::Printf(TEXT("Border cell %d left base level: %f"), CellIndex, Height));
			return false;
		}
	}

	// Drain water above the ground it drains would let a channel carve upwards.
	// Residual bumps on a receiver chain can leave rare exceptions; they must
	// stay rare and shallow enough for the carve's own guard to absorb.
	int32 SubmergedCellCount = 0;
	for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
	{
		const double CellSize = Config.BakeCellSizeMeters;
		const FVector2d Position(
			-Config.WorldSizeMeters * 0.5 + (CellIndex % Bake.GetCellsPerSide()) * CellSize,
			-Config.WorldSizeMeters * 0.5 + (CellIndex / Bake.GetCellsPerSide()) * CellSize);
		SubmergedCellCount += Bake.SampleDrainWaterElevationMeters(Position) > Heights[CellIndex] + 1.0;
	}
	const double SubmergedFraction = static_cast<double>(SubmergedCellCount) / static_cast<double>(CellCount);
	TestTrue(FString::Printf(TEXT("Drain water sits at or under the ground almost everywhere (%.3f%%)"), 100.0 * SubmergedFraction),
		SubmergedFraction < 0.02);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFluvialBakeRiversTest,
	"Massif.WorldGen.FluvialBake.Rivers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFluvialBakeRiversTest::RunTest(const FString& Parameters)
{
	const FWorldGenConfig Config = MakeBakeTestConfig(1337);
	const FMassifFluvialBake Bake = BuildBake(Config);
	const TArray<FMassifFluvialRiverNode>& Nodes = Bake.GetRiverNodes();

	TestTrue(TEXT("The solve produced river polylines"), Nodes.Num() > 0);

	int32 OutletCount = 0;
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		const FMassifFluvialRiverNode& Node = Nodes[NodeIndex];
		OutletCount += Node.ParentIndex == INDEX_NONE;
		if (Node.DrainageAreaSquareMeters < Config.RiverDrainageAreaThresholdSquareMeters)
		{
			AddError(FString::Printf(TEXT("Node %d sits below the channel threshold"), NodeIndex));
			return false;
		}
		if (Node.ParentIndex == INDEX_NONE)
		{
			continue;
		}
		const FMassifFluvialRiverNode& Parent = Nodes[Node.ParentIndex];
		if (Node.WaterElevationMeters < Parent.WaterElevationMeters)
		{
			AddError(FString::Printf(TEXT("Water rises downstream at node %d"), NodeIndex));
			return false;
		}
	}
	TestTrue(TEXT("At least one polyline reaches an outlet"), OutletCount > 0);

	// A channel must cut ground standing above its own water.
	const FMassifFluvialRiverNode& ProbeNode = Nodes[Nodes.Num() / 2];
	const double CarveMeters = Bake.SampleChannelCarveMeters(
		ProbeNode.PositionMeters, ProbeNode.WaterElevationMeters + 50.0, Config);
	TestTrue(FString::Printf(TEXT("A channel carves at its own polyline (%.2f m)"), CarveMeters), CarveMeters > 0.0);

	return true;
}
