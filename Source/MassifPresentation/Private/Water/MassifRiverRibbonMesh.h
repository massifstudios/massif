#pragma once

#include "CoreMinimal.h"

struct FMassifRiverReach;

namespace UE::Geometry
{
	class FDynamicMesh3;
}

namespace MassifRiverRibbonMesh
{
	/** What the vertex colours are measured against, so the material reads fractions rather than units. */
	inline constexpr double SpeedReferenceMetersPerSecond = 4.0;
	inline constexpr double DepthReferenceMeters = 2.0;
	inline constexpr double WidthReferenceMeters = 30.0;

	/**
	 * Most the water's own depth may exceed the sheet's clearance over the bed
	 * before the correction for it stops. See the alpha channel below.
	 */
	inline constexpr double ColumnScaleReference = 12.0;

	/**
	 * How far the drawn sheet is held over the measured bed.
	 *
	 * The solved water surface is handed downstream as a running minimum, so that
	 * it never climbs as it goes; where that minimum locks onto a low cell the
	 * surface can end up under the ground measured beneath it. Drawing the sheet
	 * there would bury the river. This is the clearance it is lifted to instead.
	 */
	inline constexpr double SheetClearanceMeters = 0.25;

	/**
	 * Share of a bend's radius the ribbon may reach across it. An offset polyline
	 * folds over itself wherever its half-width exceeds the radius it is turning
	 * through, which on a meander is a bow tie across the inside of the bend.
	 */
	inline constexpr double BendHalfWidthFraction = 0.9;

	/**
	 * The water surface of a set of reaches, as one sheet per reach: four vertices
	 * per node, the sheet itself spanning the channel level at the solved water
	 * level, and a short skirt hanging from each of its edges down to the measured
	 * bed.
	 *
	 * Level across the flow is the whole point — that is what a water surface is,
	 * and it is what lets the material treat the across direction as horizontal.
	 * Where the channel is incised the terrain cuts the sheet off on its own, which
	 * is what the edge of a river actually looks like; the skirts are for the rest,
	 * where the ground falls away and the sheet would otherwise show the opaque
	 * shading model's unfadeable edge.
	 *
	 * UV runs 0 to 1 across the channel and in metres along it, so a texture
	 * scrolled along V travels at the same rate on a brook as on a river however
	 * differently wide they are. The skirts carry the edge U of the side they hang
	 * from, so the bank fade and the bank foam reach down them.
	 *
	 * Vertex colour carries speed, depth and width as fractions of the references
	 * above, and in alpha how far the water's own depth exceeds the clearance the
	 * sheet actually has over the bed. The elevation model is a laser scan, so it
	 * measures the top of a river and not its bottom: the geometry can only be
	 * given a freeboard, and the shading model would read that as a few
	 * centimetres of water. Alpha is what lets the material integrate the column
	 * the network solved instead of the one the geometry has.
	 *
	 * Positions are centimetres local to OriginMeters; Z is absolute altitude, so
	 * the owning component sits over the origin at Z = 0. Pure geometry, no
	 * components, so it runs on the thread pool and headless.
	 */
	TSharedPtr<UE::Geometry::FDynamicMesh3> BuildRibbonMesh(
		TArrayView<const FMassifRiverReach* const> Reaches, FVector2d OriginMeters);
}
