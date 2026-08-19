"""Coordinate conversion between WGS84 and MGI / Austria GK West (EPSG:31254).

The Tirol elevation service publishes everything in GK West, so an area given
as latitude/longitude has to be converted before it can be requested. Pure
standard library: the project has no GDAL/pyproj dependency and one projection
is not worth adding one.

GK West is a Transverse Mercator on the Bessel 1841 ellipsoid, central meridian
10 deg 20', scale factor 1, false easting 0, false northing -5000000. The datum
shift to WGS84 is the seven-parameter Helmert transform EPSG:1618, accurate to
about a metre over Austria -- far below the 50 cm the terrain model resolves.
"""

import math

BESSEL_SEMI_MAJOR_METERS = 6377397.155
BESSEL_INVERSE_FLATTENING = 299.1528128
WGS84_SEMI_MAJOR_METERS = 6378137.0
WGS84_INVERSE_FLATTENING = 298.257223563

GK_WEST_CENTRAL_MERIDIAN_DEGREES = 10.0 + 20.0 / 60.0
GK_WEST_FALSE_NORTHING_METERS = -5000000.0

# EPSG:1618, MGI to WGS84, position vector convention.
HELMERT_TRANSLATION_METERS = (577.326, 90.129, 463.920)
HELMERT_ROTATION_ARCSECONDS = (5.137, 1.474, 5.297)
HELMERT_SCALE_PPM = 2.4232


def _ellipsoid_second_eccentricity_squared(semi_major, inverse_flattening):
    flattening = 1.0 / inverse_flattening
    semi_minor = semi_major * (1.0 - flattening)
    return (semi_major**2 - semi_minor**2) / semi_minor**2


def _geodetic_to_geocentric(latitude_degrees, longitude_degrees, height_meters,
                            semi_major, inverse_flattening):
    flattening = 1.0 / inverse_flattening
    eccentricity_squared = flattening * (2.0 - flattening)
    latitude = math.radians(latitude_degrees)
    longitude = math.radians(longitude_degrees)
    prime_vertical = semi_major / math.sqrt(1.0 - eccentricity_squared * math.sin(latitude) ** 2)
    x = (prime_vertical + height_meters) * math.cos(latitude) * math.cos(longitude)
    y = (prime_vertical + height_meters) * math.cos(latitude) * math.sin(longitude)
    z = (prime_vertical * (1.0 - eccentricity_squared) + height_meters) * math.sin(latitude)
    return x, y, z


def _geocentric_to_geodetic(x, y, z, semi_major, inverse_flattening):
    flattening = 1.0 / inverse_flattening
    eccentricity_squared = flattening * (2.0 - flattening)
    semi_minor = semi_major * (1.0 - flattening)
    second_eccentricity_squared = _ellipsoid_second_eccentricity_squared(semi_major, inverse_flattening)

    distance_from_axis = math.hypot(x, y)
    angle = math.atan2(z * semi_major, distance_from_axis * semi_minor)
    latitude = math.atan2(
        z + second_eccentricity_squared * semi_minor * math.sin(angle) ** 3,
        distance_from_axis - eccentricity_squared * semi_major * math.cos(angle) ** 3)
    longitude = math.atan2(y, x)
    prime_vertical = semi_major / math.sqrt(1.0 - eccentricity_squared * math.sin(latitude) ** 2)
    height = distance_from_axis / math.cos(latitude) - prime_vertical
    return math.degrees(latitude), math.degrees(longitude), height


def _apply_helmert(x, y, z, forward):
    """Position-vector Helmert; forward goes MGI -> WGS84, reverse the other way."""
    rotation_x, rotation_y, rotation_z = (math.radians(seconds / 3600.0)
                                          for seconds in HELMERT_ROTATION_ARCSECONDS)
    scale = 1.0 + HELMERT_SCALE_PPM * 1.0e-6
    translation_x, translation_y, translation_z = HELMERT_TRANSLATION_METERS

    if forward:
        out_x = translation_x + scale * (x - rotation_z * y + rotation_y * z)
        out_y = translation_y + scale * (rotation_z * x + y - rotation_x * z)
        out_z = translation_z + scale * (-rotation_y * x + rotation_x * y + z)
        return out_x, out_y, out_z

    shifted_x = (x - translation_x) / scale
    shifted_y = (y - translation_y) / scale
    shifted_z = (z - translation_z) / scale
    out_x = shifted_x + rotation_z * shifted_y - rotation_y * shifted_z
    out_y = -rotation_z * shifted_x + shifted_y + rotation_x * shifted_z
    out_z = rotation_y * shifted_x - rotation_x * shifted_y + shifted_z
    return out_x, out_y, out_z


def _meridian_arc_meters(latitude, semi_major, inverse_flattening):
    flattening = 1.0 / inverse_flattening
    n = flattening / (2.0 - flattening)
    arc_radius = semi_major / (1.0 + n) * (1.0 + n**2 / 4.0 + n**4 / 64.0)
    return arc_radius * (latitude
                         - 3.0 / 2.0 * (n - n**3 / 8.0) * math.sin(2.0 * latitude)
                         + 15.0 / 16.0 * (n**2 - n**4 / 4.0) * math.sin(4.0 * latitude)
                         - 35.0 / 48.0 * n**3 * math.sin(6.0 * latitude)
                         + 315.0 / 512.0 * n**4 * math.sin(8.0 * latitude))


def _transverse_mercator_forward(latitude_degrees, longitude_degrees):
    semi_major = BESSEL_SEMI_MAJOR_METERS
    inverse_flattening = BESSEL_INVERSE_FLATTENING
    flattening = 1.0 / inverse_flattening
    eccentricity_squared = flattening * (2.0 - flattening)
    second_eccentricity_squared = _ellipsoid_second_eccentricity_squared(semi_major, inverse_flattening)

    latitude = math.radians(latitude_degrees)
    longitude_offset = math.radians(longitude_degrees - GK_WEST_CENTRAL_MERIDIAN_DEGREES)

    prime_vertical = semi_major / math.sqrt(1.0 - eccentricity_squared * math.sin(latitude) ** 2)
    tangent = math.tan(latitude)
    tangent_squared = tangent * tangent
    eta_squared = second_eccentricity_squared * math.cos(latitude) ** 2
    cosine = math.cos(latitude)
    a = longitude_offset * cosine

    easting = prime_vertical * (a
                                + (1.0 - tangent_squared + eta_squared) * a**3 / 6.0
                                + (5.0 - 18.0 * tangent_squared + tangent_squared**2
                                   + 14.0 * eta_squared - 58.0 * tangent_squared * eta_squared) * a**5 / 120.0)
    northing = _meridian_arc_meters(latitude, semi_major, inverse_flattening) + prime_vertical * tangent * (
        a**2 / 2.0
        + (5.0 - tangent_squared + 9.0 * eta_squared + 4.0 * eta_squared**2) * a**4 / 24.0
        + (61.0 - 58.0 * tangent_squared + tangent_squared**2
           + 270.0 * eta_squared - 330.0 * tangent_squared * eta_squared) * a**6 / 720.0)
    return easting, northing + GK_WEST_FALSE_NORTHING_METERS


def _transverse_mercator_inverse(easting, northing):
    semi_major = BESSEL_SEMI_MAJOR_METERS
    inverse_flattening = BESSEL_INVERSE_FLATTENING
    flattening = 1.0 / inverse_flattening
    eccentricity_squared = flattening * (2.0 - flattening)
    second_eccentricity_squared = _ellipsoid_second_eccentricity_squared(semi_major, inverse_flattening)
    semi_minor = semi_major * (1.0 - flattening)

    meridian_arc = northing - GK_WEST_FALSE_NORTHING_METERS
    n = flattening / (2.0 - flattening)
    arc_radius = semi_major / (1.0 + n) * (1.0 + n**2 / 4.0 + n**4 / 64.0)
    rectifying = meridian_arc / arc_radius
    footpoint = (rectifying
                 + (3.0 / 2.0 * n - 27.0 / 32.0 * n**3) * math.sin(2.0 * rectifying)
                 + (21.0 / 16.0 * n**2 - 55.0 / 32.0 * n**4) * math.sin(4.0 * rectifying)
                 + 151.0 / 96.0 * n**3 * math.sin(6.0 * rectifying)
                 + 1097.0 / 512.0 * n**4 * math.sin(8.0 * rectifying))

    tangent = math.tan(footpoint)
    tangent_squared = tangent * tangent
    eta_squared = second_eccentricity_squared * math.cos(footpoint) ** 2
    prime_vertical = semi_major / math.sqrt(1.0 - eccentricity_squared * math.sin(footpoint) ** 2)
    meridian_radius = semi_major * (1.0 - eccentricity_squared) / (
        1.0 - eccentricity_squared * math.sin(footpoint) ** 2) ** 1.5
    d = easting / prime_vertical

    latitude = footpoint - prime_vertical * tangent / meridian_radius * (
        d**2 / 2.0
        - (5.0 + 3.0 * tangent_squared + eta_squared - 9.0 * tangent_squared * eta_squared) * d**4 / 24.0
        + (61.0 + 90.0 * tangent_squared + 45.0 * tangent_squared**2) * d**6 / 720.0)
    longitude_offset = (d
                        - (1.0 + 2.0 * tangent_squared + eta_squared) * d**3 / 6.0
                        + (5.0 + 28.0 * tangent_squared + 24.0 * tangent_squared**2
                           + 6.0 * eta_squared + 8.0 * tangent_squared * eta_squared) * d**5 / 120.0) / math.cos(footpoint)
    return math.degrees(latitude), GK_WEST_CENTRAL_MERIDIAN_DEGREES + math.degrees(longitude_offset)


def wgs84_to_gk_west(latitude_degrees, longitude_degrees):
    """WGS84 lat/lon in degrees to EPSG:31254 easting/northing in meters."""
    x, y, z = _geodetic_to_geocentric(latitude_degrees, longitude_degrees, 0.0,
                                      WGS84_SEMI_MAJOR_METERS, WGS84_INVERSE_FLATTENING)
    x, y, z = _apply_helmert(x, y, z, forward=False)
    latitude, longitude, _ = _geocentric_to_geodetic(x, y, z, BESSEL_SEMI_MAJOR_METERS, BESSEL_INVERSE_FLATTENING)
    return _transverse_mercator_forward(latitude, longitude)


def gk_west_to_wgs84(easting_meters, northing_meters):
    """EPSG:31254 easting/northing in meters to WGS84 lat/lon in degrees."""
    latitude, longitude = _transverse_mercator_inverse(easting_meters, northing_meters)
    x, y, z = _geodetic_to_geocentric(latitude, longitude, 0.0,
                                      BESSEL_SEMI_MAJOR_METERS, BESSEL_INVERSE_FLATTENING)
    x, y, z = _apply_helmert(x, y, z, forward=True)
    latitude, longitude, _ = _geocentric_to_geodetic(x, y, z, WGS84_SEMI_MAJOR_METERS, WGS84_INVERSE_FLATTENING)
    return latitude, longitude
