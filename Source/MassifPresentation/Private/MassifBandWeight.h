#pragma once

#include "CoreMinimal.h"

/**
 * How well a value sits inside a band: 0 outside it, 1 well inside, ramping over
 * the fade at both ends.
 *
 * Layers, species and anything else described by the conditions it belongs in
 * are scored this way, so they all have to agree on what "belongs in" means —
 * and a copy per file puts several identically named functions into one unity
 * translation unit, where their anonymous namespaces are no longer separate.
 */
inline double MassifBandWeight(double Value, double Minimum, double Maximum, double Fade)
{
	const double RisingEdge = (Value - Minimum) / Fade;
	const double FallingEdge = (Maximum - Value) / Fade;
	return FMath::Clamp(FMath::Min(RisingEdge, FallingEdge), 0.0, 1.0);
}
