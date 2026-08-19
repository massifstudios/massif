#pragma once

#include "CoreMinimal.h"
#include "MassifHeightmap.h"
#include "MassifTerrainField.h"

/**
 * A stand-in terrain field for presentation tests: rolling ground in the
 * altitude band the foliage species occupy, gentle enough that plants and mesh
 * tiles both have somewhere to land. Built in memory, so tests never depend on
 * a fetched elevation model being present.
 */
namespace MassifTestTerrain
{
	constexpr int32 SampleCountPerSide = 1024;
	constexpr double SampleSpacingMeters = 4.0;

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
				Altitudes.Add(1450.0
					+ 260.0 * FMath::Sin(East / 900.0)
					+ 190.0 * FMath::Cos(North / 700.0)
					+ 40.0 * FMath::Sin((East + North) / 260.0));
			}
		}
		return MakeShared<const FMassifTerrainField>(
			FMassifHeightmap::FromAltitudes(Altitudes, SampleCountPerSide, SampleCountPerSide, SampleSpacingMeters));
	}
}
