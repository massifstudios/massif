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
	 * Share of a bend's radius the ribbon may reach across it. An offset polyline
	 * folds over itself wherever its half-width exceeds the radius it is turning
	 * through, which on a meander is a bow tie across the inside of the bend.
	 */
	inline constexpr double BendHalfWidthFraction = 0.9;

	/**
	 * The water surface of a set of reaches, as one sheet per reach: four vertices
	 * per node, the sheet itself spanning the channel level at the solved water
	 * level, and a skirt hanging from each of its edges down to the channel invert
	 * the terrain is carved to.
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
	 * sheet has over the bed. One, wherever the terrain carries the whole trench
	 * the network asked for, which is what the carve gives it.
	 *
	 * Positions are centimetres local to OriginMeters; Z is absolute altitude, so
	 * the owning component sits over the origin at Z = 0. Pure geometry, no
	 * components, so it runs on the thread pool and headless.
	 */
	TSharedPtr<UE::Geometry::FDynamicMesh3> BuildRibbonMesh(
		TArrayView<const FMassifRiverReach* const> Reaches, FVector2d OriginMeters);
}
