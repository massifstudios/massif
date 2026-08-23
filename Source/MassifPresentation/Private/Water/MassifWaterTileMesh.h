#pragma once

#include "CoreMinimal.h"

class FMassifRiverField;

namespace UE::Geometry
{
	class FDynamicMesh3;
}

namespace MassifWaterTileMesh
{
	/** Side of the square a water tile covers, and how finely it is cut inside that. */
	inline constexpr double TileSizeMeters = 64.0;
	inline constexpr int32 QuadsPerSide = 128;
	inline constexpr double CellSizeMeters = TileSizeMeters / QuadsPerSide;

	/** What the vertex colours are measured against, so the material reads fractions rather than units. */
	inline constexpr double SpeedReferenceMetersPerSecond = 4.0;
	inline constexpr double DepthReferenceMeters = 2.0;
	inline constexpr double WidthReferenceMeters = 30.0;

	/**
	 * Most the water's own depth may exceed the clearance the surface has over the
	 * bed before the material's correction for it stops. One now: the terrain is
	 * cut to the whole trench the network asked for, so the geometry carries the
	 * column and nothing needs scaling up to stand in for it.
	 */
	inline constexpr double ColumnScaleReference = 12.0;

	/**
	 * The water surface over one tile of the world, cut out of a lattice by the
	 * river field.
	 *
	 * The lattice is global — a vertex belongs to the world rather than to the
	 * tile — so neighbouring tiles place their shared boundary vertices at the
	 * same points, and the crossings where the mesh meets the water's edge are
	 * interpolated from the same two distances on either side. There is nothing to
	 * stitch.
	 *
	 * A confluence needs no special case. The surface altitude is a single valued
	 * function of position (MassifRiverField.h), so two channels meeting produce
	 * one sheet rather than two in the same place, whatever order their reaches
	 * were traced in.
	 *
	 * There is no level of detail here and nothing to morph between: the field
	 * answers the same altitude at a point however finely it is being asked, so a
	 * tile built now and the same tile built at any other time agree exactly.
	 *
	 * TimeSlice is the half-month of the year the water is drawn at. It moves the
	 * surface and the wetted edge together — a river withdrawn into its own
	 * thalweg in February is a narrower mesh, not the same mesh lowered through
	 * its banks.
	 *
	 * SolvedPatchMeters is a box some other surface is already drawing, which is
	 * cut out of this one. A river drawn a level per station and a patch solved
	 * over the same bed are two sheets in the same place, and two sheets in the
	 * same place is what a hall of mirrors looks like. Cut as part of the distance
	 * rather than by dropping whole tiles, so the seam lands on the box's own edge
	 * — where the patch is held at the level this surface draws, and the two meet
	 * without a lip.
	 *
	 * UV0 runs 0 to 1 across the channel and in metres along it, UV1 is the flow
	 * direction, and vertex colour carries speed, depth and width as fractions of
	 * the references above. Alpha is what the material multiplies the water's
	 * absorption by, and is one trench's worth — the geometry has the column now.
	 *
	 * Normals are straight up. A water surface is level; the channel's own
	 * gradient belongs in the foam rather than in the surface it is reflected off.
	 *
	 * Positions are centimetres local to the tile's low corner; Z is absolute
	 * altitude. Pure geometry, no components, so it runs on the thread pool and
	 * headless.
	 */
	TSharedPtr<UE::Geometry::FDynamicMesh3> BuildWaterTileMesh(
		const FMassifRiverField& RiverField, int32 TileX, int32 TileY, int32 TimeSlice,
		const FBox2d& SolvedPatchMeters);

	/** Low corner of the tile, which is what its component is placed at. */
	FVector2d TileOriginMeters(int32 TileX, int32 TileY);
}
