#pragma once

#include "CoreMinimal.h"

class FMassifTerrainField;

namespace UE::Geometry
{
	class FDynamicMesh3;
}

namespace MassifTerrainTileMesh
{
	/**
	 * One square terrain tile sampled from the field: a displaced vertex grid
	 * with normals from the same samples and a skirt around the boundary to
	 * hide the cracks where tiles of different depth meet.
	 *
	 * Positions are centimeters local to the tile center; heights are absolute
	 * altitude, so the owning component sits at the tile center at Z = 0.
	 * Pure geometry, no components, so it runs on the thread pool and headless.
	 */
	TSharedPtr<UE::Geometry::FDynamicMesh3> BuildTileMesh(
		const FMassifTerrainField& Field, FVector2d TileCenterMeters, double TileSizeMeters, int32 QuadsPerSide);
}
