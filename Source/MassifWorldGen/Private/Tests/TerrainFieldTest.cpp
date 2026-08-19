#include "MassifHeightmap.h"
#include "MassifTerrainField.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr int32 FieldTestSide = 256;
	constexpr double FieldTestSpacingMeters = 2.0;
	constexpr double FieldTestFallPerSampleMeters = 1.5;

	TSharedRef<const FMassifTerrainField> MakeTestField()
	{
		TArray<double> Altitudes;
		Altitudes.Reserve(FieldTestSide * FieldTestSide);
		for (int32 Row = 0; Row < FieldTestSide; ++Row)
		{
			for (int32 Column = 0; Column < FieldTestSide; ++Column)
			{
				// A flank falling east, with a ridge running across it, so
				// normals point somewhere specific and slope varies.
				Altitudes.Add(1500.0
					- Column * FieldTestFallPerSampleMeters
					+ 180.0 * FMath::Sin(Row * FieldTestSpacingMeters / 150.0));
			}
		}
		return MakeShared<const FMassifTerrainField>(
			FMassifHeightmap::FromAltitudes(Altitudes, FieldTestSide, FieldTestSide, FieldTestSpacingMeters));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainFieldExtentTest,
	"Massif.WorldGen.TerrainField.Extent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTerrainFieldExtentTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifTerrainField> Field = MakeTestField();

	TestEqual(TEXT("World size comes from the imported window"),
		Field->GetWorldSizeMeters(), FieldTestSide * FieldTestSpacingMeters);
	TestTrue(TEXT("Altitude band is non-empty"),
		Field->GetMaxAltitudeMeters() > Field->GetMinAltitudeMeters());

	// The quadtree measures its root tile against the world size, so ground has
	// to exist over the whole of it, corners included.
	const double HalfSizeMeters = Field->GetWorldSizeMeters() * 0.5;
	for (int32 Step = 0; Step <= 32; ++Step)
	{
		const double Fraction = Step / 32.0;
		const FVector2d Position(
			-HalfSizeMeters + Fraction * 2.0 * HalfSizeMeters,
			HalfSizeMeters - Fraction * 2.0 * HalfSizeMeters);
		if (!FMath::IsFinite(Field->SampleHeightMeters(Position, 4.0)))
		{
			AddError(FString::Printf(TEXT("No ground at (%.1f, %.1f)"), Position.X, Position.Y));
			return false;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainFieldNormalTest,
	"Massif.WorldGen.TerrainField.Normal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTerrainFieldNormalTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifTerrainField> Field = MakeTestField();

	// The surface falls 1.5 m per 2 m eastward, so the normal leans west and
	// the slope is the arctangent of that gradient. A sign error here is what
	// makes foliage and placement validation read the ground backwards.
	const double ExpectedSlopeDegrees = FMath::RadiansToDegrees(
		FMath::Atan(FieldTestFallPerSampleMeters / FieldTestSpacingMeters));
	double WorstSlopeErrorDegrees = 0.0;

	for (int32 Step = 0; Step < 24; ++Step)
	{
		const FVector2d Position(-90.0 + Step * 3.0, 0.0);
		const FVector3d Normal = Field->SampleNormal(Position, FieldTestSpacingMeters);

		TestTrue(TEXT("Normal is unit length"), FMath::IsNearlyEqual(Normal.Length(), 1.0, 1.0e-6));
		TestTrue(TEXT("Normal points upward"), Normal.Z > 0.0);
		TestTrue(TEXT("Normal leans west, away from the falling side"), Normal.X > 0.0);

		const double SlopeDegrees = Field->SampleSlopeDegrees(Position, FieldTestSpacingMeters);
		WorstSlopeErrorDegrees = FMath::Max(WorstSlopeErrorDegrees, FMath::Abs(SlopeDegrees - ExpectedSlopeDegrees));
	}

	TestTrue(FString::Printf(TEXT("Slope matches the surface gradient (%.3f deg off %.3f deg)"),
		WorstSlopeErrorDegrees, ExpectedSlopeDegrees), WorstSlopeErrorDegrees < 1.0);
	return true;
}
