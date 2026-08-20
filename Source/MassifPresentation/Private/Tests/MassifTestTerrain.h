#pragma once

#include "CoreMinimal.h"
#include "MassifEnvironmentBake.h"
#include "MassifHabitat.h"
#include "MassifHeightmap.h"
#include "MassifTerrainField.h"

/**
 * A stand-in terrain field for presentation tests, built in memory so tests
 * never depend on a fetched elevation model being present.
 *
 * It has to span the whole altitude band the foliage table occupies — valley
 * floor near 1200 m to summits past 3700 m — because a species whose band falls
 * off the end of the test terrain looks exactly like a mistuned one. The long
 * wavelength carries that range; the short one puts faces steep enough to
 * classify as rock and scree next to ground flat enough to pond, so every
 * ground type the table asks for exists somewhere.
 */
namespace MassifTestTerrain
{
	constexpr int32 SampleCountPerSide = 1024;
	constexpr double SampleSpacingMeters = 10.0;

	inline TSharedRef<const FMassifTerrainField> MakeField()
	{
		TArray<double> Altitudes;
		Altitudes.Reserve(SampleCountPerSide * SampleCountPerSide);
		for (int32 Row = 0; Row < SampleCountPerSide; ++Row)
		{
			for (int32 Column = 0; Column < SampleCountPerSide; ++Column)
			{
				const double East = Column * SampleSpacingMeters;
				const double North = Row * SampleSpacingMeters;
				// The long wavelength is one full cycle across the world, so the whole
				// altitude range is present rather than one flank of it.
				Altitudes.Add(2400.0
					+ 1250.0 * FMath::Sin(East / 1628.0)
					+ 280.0 * FMath::Cos(North / 1500.0)
					+ 120.0 * FMath::Sin((East + North) / 260.0));
			}
		}
		return MakeShared<const FMassifTerrainField>(
			FMassifHeightmap::FromAltitudes(Altitudes, SampleCountPerSide, SampleCountPerSide, SampleSpacingMeters));
	}

	/** The same terrain with its water solved, which is what habitat needs to answer at all. */
	inline TSharedRef<const FMassifHabitatField> MakeHabitat(uint32 MasterSeed)
	{
		const TSharedRef<const FMassifTerrainField> Field = MakeField();
		return MakeShared<const FMassifHabitatField>(
			Field,
			FMassifEnvironmentBake::BuildOnCpu(
				Field->GetHeightmap(), MasterSeed, FMassifEnvironmentBakeParams()),
			MasterSeed);
	}
}
