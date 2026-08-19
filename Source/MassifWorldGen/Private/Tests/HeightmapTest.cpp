#include "MassifHeightmap.h"
#include "Misc/AutomationTest.h"

namespace MassifHeightmapTest
{
	constexpr int32 TestSide = 256;
	constexpr double TestSpacingMeters = 2.0;

	/** A ridge and a valley crossing at an angle: curvature in both axes, no symmetry to hide a swapped index. */
	double TestAltitudeMeters(int32 Column, int32 Row)
	{
		const double East = Column * TestSpacingMeters;
		const double North = Row * TestSpacingMeters;
		return 1400.0
			+ 300.0 * FMath::Sin(East / 190.0)
			+ 220.0 * FMath::Cos(North / 130.0)
			+ 60.0 * FMath::Sin((East + 2.0 * North) / 47.0);
	}

	TArray<double> MakeTestAltitudes()
	{
		TArray<double> Altitudes;
		Altitudes.Reserve(TestSide * TestSide);
		for (int32 Row = 0; Row < TestSide; ++Row)
		{
			for (int32 Column = 0; Column < TestSide; ++Column)
			{
				Altitudes.Add(TestAltitudeMeters(Column, Row));
			}
		}
		return Altitudes;
	}

	TSharedRef<const FMassifHeightmap> MakeTestHeightmap()
	{
		const TArray<double> Altitudes = MakeTestAltitudes();
		return FMassifHeightmap::FromAltitudes(Altitudes, TestSide, TestSide, TestSpacingMeters);
	}

	/** World position of a sample centre, given the origin sits at the centre of the box. */
	FVector2d PositionOfSample(int32 Column, int32 Row)
	{
		const double HalfSizeMeters = TestSide * TestSpacingMeters * 0.5;
		return FVector2d(
			-HalfSizeMeters + (Column + 0.5) * TestSpacingMeters,
			HalfSizeMeters - (Row + 0.5) * TestSpacingMeters);
	}
}

using namespace MassifHeightmapTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeightmapSampleFidelityTest,
	"Massif.WorldGen.Heightmap.SampleFidelity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHeightmapSampleFidelityTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHeightmap> Heightmap = MakeTestHeightmap();

	// At a sample centre, at the finest spacing, the interpolation weights
	// collapse onto the stored sample: what comes back has to be the imported
	// altitude, give or take the uint16 quantisation step.
	const double AllowedMeters = (Heightmap->GetMaxAltitudeMeters() - Heightmap->GetMinAltitudeMeters()) / 65535.0 + 1.0e-9;
	double WorstDifference = 0.0;
	for (int32 Row = 4; Row < TestSide - 4; Row += 7)
	{
		for (int32 Column = 4; Column < TestSide - 4; Column += 7)
		{
			const double Sampled = Heightmap->SampleHeightMeters(PositionOfSample(Column, Row), TestSpacingMeters);
			WorstDifference = FMath::Max(WorstDifference, FMath::Abs(Sampled - TestAltitudeMeters(Column, Row)));
		}
	}

	TestTrue(FString::Printf(TEXT("Sample centres reproduce the source altitudes (%.4f m off, %.4f m allowed)"),
		WorstDifference, AllowedMeters), WorstDifference <= AllowedMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeightmapChunkIndependenceTest,
	"Massif.WorldGen.Heightmap.ChunkIndependence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHeightmapChunkIndependenceTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHeightmap> Heightmap = MakeTestHeightmap();
	const double Spacing = 4.0;
	const FVector2d TileOrigin(-120.0, 60.0);

	// The property the whole streaming scheme rests on: a tile evaluated on its
	// own has to come out bit-identical to the same tile evaluated as part of
	// something larger, and in any order.
	TArray<double> Alone;
	Alone.Reserve(16 * 16);
	for (int32 StepY = 0; StepY < 16; ++StepY)
	{
		for (int32 StepX = 0; StepX < 16; ++StepX)
		{
			Alone.Add(Heightmap->SampleHeightMeters(TileOrigin + FVector2d(StepX * Spacing, StepY * Spacing), Spacing));
		}
	}

	for (int32 Index = Alone.Num() - 1; Index >= 0; --Index)
	{
		const int32 StepX = Index % 16;
		const int32 StepY = Index / 16;
		Heightmap->SampleHeightMeters(FVector2d(200.0 - Index * 13.0, -180.0 + Index * 7.0), 32.0);

		const double Interleaved = Heightmap->SampleHeightMeters(TileOrigin + FVector2d(StepX * Spacing, StepY * Spacing), Spacing);
		if (Interleaved != Alone[Index])
		{
			AddError(FString::Printf(TEXT("Sample at tile cell (%d, %d) changed with evaluation order: %.9f then %.9f"),
				StepX, StepY, Alone[Index], Interleaved));
			return false;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeightmapLodAgreementTest,
	"Massif.WorldGen.Heightmap.LodAgreement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHeightmapLodAgreementTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHeightmap> Heightmap = MakeTestHeightmap();

	// Coarse levels filter detail away, so they may differ from the finest one,
	// but only by the relief that detail carried. A larger gap means the levels
	// describe different ground and rings would step where they meet.
	const double AllowedMeters = 0.25 * (Heightmap->GetMaxAltitudeMeters() - Heightmap->GetMinAltitudeMeters());
	double WorstDifference = 0.0;
	for (int32 Step = 0; Step < 400; ++Step)
	{
		const FVector2d Position(-180.0 + Step * 0.7, 140.0 - Step * 0.5);
		const double Fine = Heightmap->SampleHeightMeters(Position, TestSpacingMeters);
		const double Coarse = Heightmap->SampleHeightMeters(Position, 64.0);
		WorstDifference = FMath::Max(WorstDifference, FMath::Abs(Fine - Coarse));
	}
	TestTrue(FString::Printf(TEXT("Coarse and fine samplings agree within the filtered detail (%.1f m of %.1f m)"),
		WorstDifference, AllowedMeters), WorstDifference <= AllowedMeters);

	// Height must be continuous in spacing: a jump at a level boundary is
	// exactly the seam the mip blend exists to remove.
	double WorstStep = 0.0;
	for (double Spacing = TestSpacingMeters; Spacing < 400.0; Spacing *= 1.02)
	{
		const FVector2d Position(-33.0, 71.0);
		const double Lower = Heightmap->SampleHeightMeters(Position, Spacing);
		const double Higher = Heightmap->SampleHeightMeters(Position, Spacing * 1.02);
		WorstStep = FMath::Max(WorstStep, FMath::Abs(Higher - Lower));
	}
	TestTrue(FString::Printf(TEXT("Height is continuous across level boundaries (worst step %.3f m)"), WorstStep),
		WorstStep < 2.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeightmapBorderTest,
	"Massif.WorldGen.Heightmap.Border",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHeightmapBorderTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHeightmap> Heightmap = MakeTestHeightmap();
	const double HalfSizeMeters = Heightmap->GetWorldSizeXMeters() * 0.5;

	// Outside the imported window there is no data, so the border extends
	// outward. It must stay finite and in range however far out it is asked.
	const double Outside = Heightmap->SampleHeightMeters(FVector2d(HalfSizeMeters * 40.0, -HalfSizeMeters * 25.0), 8.0);
	TestTrue(TEXT("Far outside the window the border height stays finite"), FMath::IsFinite(Outside));

	// Catmull-Rom overshoots at a curvature reversal, so the interpolated
	// surface may sit slightly outside the sample range; it must not run away.
	const double RangeMeters = Heightmap->GetMaxAltitudeMeters() - Heightmap->GetMinAltitudeMeters();
	const double AllowedOvershoot = 0.1 * RangeMeters;
	for (int32 Step = 0; Step < 500; ++Step)
	{
		const FVector2d Position(-HalfSizeMeters + Step * 1.13, HalfSizeMeters - Step * 0.97);
		const double Height = Heightmap->SampleHeightMeters(Position, 1.0);
		const bool bInRange = Height > Heightmap->GetMinAltitudeMeters() - AllowedOvershoot
			&& Height < Heightmap->GetMaxAltitudeMeters() + AllowedOvershoot;
		if (!bInRange)
		{
			AddError(FString::Printf(TEXT("Height %.1f m at (%.0f, %.0f) is outside the imported altitude band"),
				Height, Position.X, Position.Y));
			return false;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeightmapFileFormatTest,
	"Massif.WorldGen.Heightmap.FileFormat",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHeightmapFileFormatTest::RunTest(const FString& Parameters)
{
	// The byte layout is a contract with Tools/fetch_tirol_dem.py, which writes
	// it from Python. Building a file here is what pins the two together.
	const int32 Side = 8;
	const double SpacingMeters = 5.0;
	const double BaseMeters = 900.0;
	const double StepMeters = 0.25;

	TArray<uint8> Bytes;
	Bytes.SetNumZeroed(192 + Side * Side * sizeof(uint16));
	FMemory::Memcpy(Bytes.GetData(), "MASSIFDM", 8);
	const uint32 Header[4] = { 1, static_cast<uint32>(Side), static_cast<uint32>(Side), 0 };
	FMemory::Memcpy(Bytes.GetData() + 8, Header, sizeof(Header));
	const double Doubles[5] = { SpacingMeters, BaseMeters, StepMeters, 39076.0, 214292.0 };
	FMemory::Memcpy(Bytes.GetData() + 24, Doubles, sizeof(Doubles));
	FMemory::Memcpy(Bytes.GetData() + 64, "EPSG:31254", 10);
	FMemory::Memcpy(Bytes.GetData() + 96, "test window", 11);

	uint16* Samples = reinterpret_cast<uint16*>(Bytes.GetData() + 192);
	for (int32 Index = 0; Index < Side * Side; ++Index)
	{
		Samples[Index] = static_cast<uint16>(Index * 3);
	}

	FString LoadError;
	TSharedPtr<const FMassifHeightmap> Heightmap = FMassifHeightmap::LoadFromBytes(Bytes, LoadError);
	if (!Heightmap.IsValid())
	{
		AddError(FString::Printf(TEXT("Rejected a well-formed file: %s"), *LoadError));
		return false;
	}

	TestEqual(TEXT("World size follows from sample count and spacing"),
		Heightmap->GetWorldSizeXMeters(), Side * SpacingMeters);
	TestEqual(TEXT("Lowest sample decodes to its altitude"), Heightmap->GetMinAltitudeMeters(), BaseMeters);
	TestEqual(TEXT("Highest sample decodes to its altitude"),
		Heightmap->GetMaxAltitudeMeters(), BaseMeters + (Side * Side - 1) * 3 * StepMeters);
	TestEqual(TEXT("Source georeference is carried through"), Heightmap->GetSourceInfo().CoordinateSystem, FString(TEXT("EPSG:31254")));
	TestEqual(TEXT("Source label is carried through"), Heightmap->GetSourceInfo().Label, FString(TEXT("test window")));

	TArray<uint8> Truncated(Bytes.GetData(), Bytes.Num() - 2);
	TestFalse(TEXT("A truncated file is rejected"),
		FMassifHeightmap::LoadFromBytes(Truncated, LoadError).IsValid());

	TArray<uint8> WrongMagic = Bytes;
	WrongMagic[0] = 'X';
	TestFalse(TEXT("A file with the wrong magic is rejected"),
		FMassifHeightmap::LoadFromBytes(WrongMagic, LoadError).IsValid());

	return true;
}
