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
	 * The water surface of a set of reaches, as one strip per reach: two vertices
	 * per node, set the channel's width apart across the flow and sitting at the
	 * water level the network solved.
	 *
	 * UV runs 0 to 1 across the channel and in metres along it, so a texture
	 * scrolled along V travels at the same rate on a brook as on a river however
	 * differently wide they are. Vertex colour carries speed, depth and width as
	 * fractions of the references above, which is how the material knows how fast
	 * to scroll and how deep to tint without being told per reach.
	 *
	 * Positions are centimetres local to OriginMeters; Z is absolute altitude, so
	 * the owning component sits over the origin at Z = 0. Pure geometry, no
	 * components, so it runs on the thread pool and headless.
	 */
	TSharedPtr<UE::Geometry::FDynamicMesh3> BuildRibbonMesh(
		TArrayView<const FMassifRiverReach* const> Reaches, FVector2d OriginMeters);
}
