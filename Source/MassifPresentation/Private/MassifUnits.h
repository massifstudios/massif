#pragma once

/**
 * The sim and world generation work in metres; the engine works in centimetres.
 * Every presentation file that crosses between the two needs this, and defining
 * it in each of them puts several identically named constants into one unity
 * translation unit, where their anonymous namespaces are no longer separate.
 */
namespace MassifUnits
{
	inline constexpr double MetersToCentimeters = 100.0;
	inline constexpr double CentimetersToMeters = 0.01;
}
