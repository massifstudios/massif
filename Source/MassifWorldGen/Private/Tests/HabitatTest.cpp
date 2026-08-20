#include "MassifEnvironmentBake.h"
#include "MassifHabitat.h"
#include "MassifHeightmap.h"
#include "MassifTerrainField.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr uint32 HabitatTestSeed = 90210;
	constexpr int32 HabitatTestSide = 128;
	constexpr double HabitatTestSpacingMeters = 4.0;
	constexpr double HabitatSampleSpacingMeters = 1.0;

	/** Habitat over a field, with the water solve it reads for wetness and soil. */
	TSharedRef<const FMassifHabitatField> MakeHabitat(
		TSharedRef<const FMassifTerrainField> Field, uint32 MasterSeed)
	{
		FMassifEnvironmentBakeParams Params;
		Params.CellSizeMeters = HabitatTestSpacingMeters;
		return MakeShared<const FMassifHabitatField>(
			Field,
			FMassifEnvironmentBake::BuildOnCpu(Field->GetHeightmap(), MasterSeed, Params),
			MasterSeed);
	}

	/** Ground of one altitude everywhere: aspect drops out, leaving the lapse rate. */
	TSharedRef<const FMassifTerrainField> MakeFlatField(double AltitudeMeters)
	{
		TArray<double> Altitudes;
		Altitudes.Init(AltitudeMeters, HabitatTestSide * HabitatTestSide);
		return MakeShared<const FMassifTerrainField>(FMassifHeightmap::FromAltitudes(
			Altitudes, HabitatTestSide, HabitatTestSide, HabitatTestSpacingMeters));
	}

	/** A ridge running east-west: one flank faces south, the other north, at matching altitudes. */
	TSharedRef<const FMassifTerrainField> MakeRidgeField(double CrestAltitudeMeters, double FallPerMeter)
	{
		// Row 0 is north and sample centres sit half a cell in, so the crest falls
		// between the two middle rows and the flanks mirror each other exactly.
		const double HalfWorldMeters = 0.5 * HabitatTestSide * HabitatTestSpacingMeters;
		TArray<double> Altitudes;
		Altitudes.Reserve(HabitatTestSide * HabitatTestSide);
		for (int32 Row = 0; Row < HabitatTestSide; ++Row)
		{
			const double NorthMeters = HalfWorldMeters - (Row + 0.5) * HabitatTestSpacingMeters;
			const double AltitudeMeters = CrestAltitudeMeters - FMath::Abs(NorthMeters) * FallPerMeter;
			for (int32 Column = 0; Column < HabitatTestSide; ++Column)
			{
				Altitudes.Add(AltitudeMeters);
			}
		}
		return MakeShared<const FMassifTerrainField>(FMassifHeightmap::FromAltitudes(
			Altitudes, HabitatTestSide, HabitatTestSide, HabitatTestSpacingMeters));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHabitatTemperatureTest,
	"Massif.WorldGen.Habitat.Temperature",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHabitatTemperatureTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHabitatField> LowHabitat = MakeHabitat(MakeFlatField(800.0), HabitatTestSeed);
	const TSharedRef<const FMassifHabitatField> HighHabitat = MakeHabitat(MakeFlatField(2000.0), HabitatTestSeed);

	const FMassifHabitatSample Low = LowHabitat->Sample(FVector2d::ZeroVector, HabitatSampleSpacingMeters);
	const FMassifHabitatSample High = HighHabitat->Sample(FVector2d::ZeroVector, HabitatSampleSpacingMeters);

	TestTrue(TEXT("Flat ground reads as flat"), Low.SlopeDegrees < 0.5);
	TestTrue(FString::Printf(TEXT("Altitude cools the ground (%.1f C at 800 m, %.1f C at 2000 m)"),
		Low.TemperatureCelsius, High.TemperatureCelsius),
		Low.TemperatureCelsius > High.TemperatureCelsius);

	// 1200 m of lift at the modelled lapse rate, with no aspect to warm either end.
	const double DropCelsius = Low.TemperatureCelsius - High.TemperatureCelsius;
	TestTrue(FString::Printf(TEXT("The drop follows the lapse rate (%.2f C over 1200 m)"), DropCelsius),
		FMath::IsNearlyEqual(DropCelsius, 7.8, 0.3));

	// The treeline the species tables are tuned around: cool but not freezing high up.
	TestTrue(FString::Printf(TEXT("2000 m sits near the treeline (%.1f C)"), High.TemperatureCelsius),
		High.TemperatureCelsius > 4.0 && High.TemperatureCelsius < 8.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHabitatAspectTest,
	"Massif.WorldGen.Habitat.Aspect",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHabitatAspectTest::RunTest(const FString& Parameters)
{
	// Flanks of one ridge, equally far from the crest: same altitude, opposite aspect.
	const TSharedRef<const FMassifHabitatField> Habitat = MakeHabitat(MakeRidgeField(1600.0, 0.4), HabitatTestSeed);
	const FMassifHabitatSample SouthFlank = Habitat->Sample(FVector2d(0.0, -100.0), HabitatSampleSpacingMeters);
	const FMassifHabitatSample NorthFlank = Habitat->Sample(FVector2d(0.0, 100.0), HabitatSampleSpacingMeters);

	TestTrue(FString::Printf(TEXT("The flanks match in altitude (%.1f m and %.1f m)"),
		SouthFlank.AltitudeMeters, NorthFlank.AltitudeMeters),
		FMath::IsNearlyEqual(SouthFlank.AltitudeMeters, NorthFlank.AltitudeMeters, 1.0));
	TestTrue(FString::Printf(TEXT("The flanks match in steepness (%.1f deg and %.1f deg)"),
		SouthFlank.SlopeDegrees, NorthFlank.SlopeDegrees),
		FMath::IsNearlyEqual(SouthFlank.SlopeDegrees, NorthFlank.SlopeDegrees, 1.0));

	// Aspect is the bearing the ground faces: the flank south of the crest falls
	// south. Getting this backwards would put the treeline on the wrong side.
	TestTrue(FString::Printf(TEXT("The southern flank faces south (%.0f deg)"), SouthFlank.AspectDegrees),
		FMath::IsNearlyEqual(SouthFlank.AspectDegrees, 180.0, 5.0));
	TestTrue(FString::Printf(TEXT("The northern flank faces north (%.0f deg)"), NorthFlank.AspectDegrees),
		FMath::Abs(FRotator3d::NormalizeAxis(NorthFlank.AspectDegrees)) < 5.0);
	TestTrue(FString::Printf(TEXT("Sun warms the southern flank (%.1f C against %.1f C)"),
		SouthFlank.TemperatureCelsius, NorthFlank.TemperatureCelsius),
		SouthFlank.TemperatureCelsius > NorthFlank.TemperatureCelsius + 0.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHabitatGroundTest,
	"Massif.WorldGen.Habitat.Ground",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHabitatGroundTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHabitatField> Gentle = MakeHabitat(MakeFlatField(1100.0), HabitatTestSeed);
	TestEqual(TEXT("Temperate ground at a walkable angle is soil"),
		Gentle->Sample(FVector2d::ZeroVector, HabitatSampleSpacingMeters).GroundType, EMassifGroundType::Soil);

	const TSharedRef<const FMassifHabitatField> Frozen = MakeHabitat(MakeFlatField(3200.0), HabitatTestSeed);
	TestEqual(TEXT("Ground above the freezing line keeps its snow"),
		Frozen->Sample(FVector2d::ZeroVector, HabitatSampleSpacingMeters).GroundType,
		EMassifGroundType::PermanentSnow);

	// A 60 degree face sheds soil, snow and everything else.
	const TSharedRef<const FMassifHabitatField> Cliff = MakeHabitat(MakeRidgeField(2400.0, FMath::Tan(FMath::DegreesToRadians(60.0))), HabitatTestSeed);
	const FMassifHabitatSample CliffSample = Cliff->Sample(FVector2d(0.0, -100.0), HabitatSampleSpacingMeters);
	TestTrue(FString::Printf(TEXT("The face reads as steep (%.1f deg)"), CliffSample.SlopeDegrees),
		CliffSample.SlopeDegrees > 55.0);
	TestEqual(TEXT("A face too steep to hold anything is rock"), CliffSample.GroundType, EMassifGroundType::Rock);

	// Steep and cold, but not sheer: loose debris rather than bare rock.
	const TSharedRef<const FMassifHabitatField> Debris =
		MakeHabitat(MakeRidgeField(2300.0, FMath::Tan(FMath::DegreesToRadians(38.0))), HabitatTestSeed);
	const FMassifHabitatSample DebrisSample = Debris->Sample(FVector2d(0.0, -100.0), HabitatSampleSpacingMeters);
	AddInfo(FString::Printf(TEXT("Debris flank: %.1f deg, %.1f C, %.3f m soil, %.2f humidity"),
		DebrisSample.SlopeDegrees, DebrisSample.TemperatureCelsius,
		DebrisSample.SoilDepthMeters, DebrisSample.Humidity));
	// Thinned by the water that ran down it, but not swept to bedrock: that gap
	// between the two is the whole difference between scree and a rock face.
	const double UndisturbedSoilMeters = FMassifEnvironmentBakeParams().BaseSoilDepthMeters;
	TestTrue(FString::Printf(TEXT("Water has stripped the flank without clearing it (%.3f of %.2f m)"),
		DebrisSample.SoilDepthMeters, UndisturbedSoilMeters),
		DebrisSample.SoilDepthMeters > 0.0 && DebrisSample.SoilDepthMeters < UndisturbedSoilMeters);
	TestEqual(TEXT("Steep ground above the forest is scree"), DebrisSample.GroundType, EMassifGroundType::Scree);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHabitatHumidityTest,
	"Massif.WorldGen.Habitat.Humidity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHabitatHumidityTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifHabitatField> Habitat = MakeHabitat(MakeRidgeField(1600.0, 0.4), HabitatTestSeed);

	double MinHumidity = 1.0;
	double MaxHumidity = 0.0;
	for (int32 Step = 0; Step < 64; ++Step)
	{
		const FVector2d Position(-240.0 + Step * 7.5, 180.0 - Step * 5.0);
		const double Humidity = Habitat->Sample(Position, HabitatSampleSpacingMeters).Humidity;
		if (Humidity < 0.0 || Humidity > 1.0)
		{
			AddError(FString::Printf(TEXT("Humidity %.3f at (%.0f, %.0f) is outside 0-1"),
				Humidity, Position.X, Position.Y));
			return false;
		}
		MinHumidity = FMath::Min(MinHumidity, Humidity);
		MaxHumidity = FMath::Max(MaxHumidity, Humidity);
	}

	TestTrue(FString::Printf(TEXT("Humidity varies across the massif (%.2f to %.2f)"), MinHumidity, MaxHumidity),
		MaxHumidity - MinHumidity > 0.02);

	// Windward and lee are opposite sides of the same ridge, so one of the two
	// flanks must come out wetter whichever way the seeded wind blows.
	const FVector2d WindDirection = Habitat->GetPrevailingWindDirection();
	TestTrue(TEXT("The prevailing wind has a direction"),
		FMath::IsNearlyEqual(WindDirection.Length(), 1.0, 1.0e-6));

	const double SouthFlank = Habitat->Sample(FVector2d(0.0, -100.0), HabitatSampleSpacingMeters).Humidity;
	const double NorthFlank = Habitat->Sample(FVector2d(0.0, 100.0), HabitatSampleSpacingMeters).Humidity;
	TestTrue(FString::Printf(TEXT("The two flanks of a ridge are not equally wet (%.3f and %.3f)"),
		SouthFlank, NorthFlank), !FMath::IsNearlyEqual(SouthFlank, NorthFlank, 0.005));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHabitatDeterminismTest,
	"Massif.WorldGen.Habitat.Determinism",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHabitatDeterminismTest::RunTest(const FString& Parameters)
{
	const TSharedRef<const FMassifTerrainField> Field = MakeRidgeField(1600.0, 0.4);
	const TSharedRef<const FMassifHabitatField> Habitat = MakeHabitat(Field, HabitatTestSeed);
	const TSharedRef<const FMassifHabitatField> SameSeed = MakeHabitat(Field, HabitatTestSeed);
	const TSharedRef<const FMassifHabitatField> OtherSeed = MakeHabitat(Field, HabitatTestSeed + 1);

	double WorstDifference = 0.0;
	bool bOtherSeedDiffers = false;
	for (int32 Step = 0; Step < 32; ++Step)
	{
		const FVector2d Position(-200.0 + Step * 12.0, 60.0 - Step * 4.0);
		const FMassifHabitatSample First = Habitat->Sample(Position, HabitatSampleSpacingMeters);
		const FMassifHabitatSample Second = SameSeed->Sample(Position, HabitatSampleSpacingMeters);
		const FMassifHabitatSample Other = OtherSeed->Sample(Position, HabitatSampleSpacingMeters);

		WorstDifference = FMath::Max(WorstDifference, FMath::Abs(First.Humidity - Second.Humidity));
		bOtherSeedDiffers |= !FMath::IsNearlyEqual(First.Humidity, Other.Humidity, 1.0e-6);
	}

	TestEqual(TEXT("The same seed and position always answer the same"), WorstDifference, 0.0);
	TestTrue(TEXT("A different master seed moves the weather"), bOtherSeedDiffers);
	return true;
}
