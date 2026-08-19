#!/usr/bin/env python3
"""Fetch a square of the Tirol airborne-laserscan terrain model into a .massifdem.

Source: Land Tirol, "Digitales Geländemodell Tirol" (bare-earth DTM, 50 cm
grid), served as OGC WCS 2.0 in EPSG:31254 (MGI / Austria GK West), CC BY 4.0.
The service resamples server-side, so a window is requested at the resolution
the game wants rather than at 50 cm and downsampled here.

The window is cut into request tiles, mosaicked, quantised to uint16 and written
as a single .massifdem that MassifWorldGen memory-maps at startup. A hillshade
PNG is written next to it so the area can be eyeballed without launching UE.

    python3 Tools/fetch_tirol_dem.py                     # 20 km around Sölden at 2 m
    python3 Tools/fetch_tirol_dem.py --lat 47.13 --lon 10.26 --name arlberg

Needs numpy; deliberately no GDAL/rasterio, so it runs on a bare checkout.
"""

import argparse
import math
import os
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from massif_geo import wgs84_to_gk_west, gk_west_to_wgs84  # noqa: E402

WCS_ENDPOINT = "https://gis.tirol.gv.at/arcgis/services/Service_Public/terrain/MapServer/WCSServer"
COVERAGE_ID = "Gelaendemodell_50cm_M28"
# Same model, coarse: one request covers a whole window, which is what the
# up-front void check needs and what the full fetch is too slow to be used for.
SURVEY_COVERAGE_ID = "Gelaendemodell_5m_M28"
SOURCE_CRS = "EPSG:31254"

# The coverage's own extent, from DescribeCoverage. Requests outside it come
# back empty rather than as an error, so the window is checked up front.
COVERAGE_EASTING_RANGE = (-18750.0, 110000.0)
COVERAGE_NORTHING_RANGE = (180000.0, 275000.0)

# The Ötztal at Sölden: 1300 m valley floor under 3400 m glaciated peaks, and a
# ski domain large enough that a 20 km square is all resort-shaped terrain.
# Offset 2 km north-west of the village so the window clears the Italian border,
# where the Tirol model ends.
DEFAULT_CENTER_LATITUDE = 46.97584
DEFAULT_CENTER_LONGITUDE = 10.97800

MAX_TILE_SAMPLES = 2000
REQUEST_TIMEOUT_SECONDS = 300
REQUEST_ATTEMPTS = 3

DEM_MAGIC = b"MASSIFDM"
DEM_VERSION = 1
DEM_HEADER_BYTES = 192

# Anything outside this is a void or a service artefact, not Tirol: the lowest
# ground in the state is around 460 m, and the service returns 0 outside it.
PLAUSIBLE_ALTITUDE_RANGE = (100.0, 4500.0)

# Voids this far under the total are edge artefacts worth interpolating over.
# More than that is a window reaching outside Tirol, which no amount of filling
# turns back into terrain.
VOID_FRACTION_TOLERANCE = 0.0005
SURVEY_RESOLUTION_METERS = 20.0


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lat", type=float, default=DEFAULT_CENTER_LATITUDE,
                        help="WGS84 latitude of the window centre")
    parser.add_argument("--lon", type=float, default=DEFAULT_CENTER_LONGITUDE,
                        help="WGS84 longitude of the window centre")
    parser.add_argument("--extent-meters", type=float, default=20000.0,
                        help="side of the square window in meters")
    parser.add_argument("--resolution-meters", type=float, default=2.0,
                        help="distance between samples in meters")
    parser.add_argument("--name", default="oetztal",
                        help="output basename under the terrain data directory")
    parser.add_argument("--output-directory",
                        default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                             "Data", "Terrain"),
                        help="where the .massifdem and its preview are written")
    return parser.parse_args()


def decode_tiff(buffer):
    """Minimal float32 GeoTIFF reader: enough for what this service returns."""
    if buffer[:4] == b"II*\x00":
        byte_order = "<"
    elif buffer[:4] == b"MM\x00*":
        byte_order = ">"
    else:
        raise ValueError("response part is not a TIFF")

    type_sizes = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8}
    type_formats = {1: "B", 3: "H", 4: "I", 11: "f", 12: "d"}

    first_directory, = struct.unpack_from(byte_order + "I", buffer, 4)
    entry_count, = struct.unpack_from(byte_order + "H", buffer, first_directory)
    tags = {}
    for entry_index in range(entry_count):
        entry_offset = first_directory + 2 + entry_index * 12
        tag, value_type, value_count = struct.unpack_from(byte_order + "HHI", buffer, entry_offset)
        payload_bytes = type_sizes.get(value_type, 1) * value_count
        if payload_bytes <= 4:
            payload_offset = entry_offset + 8
        else:
            payload_offset, = struct.unpack_from(byte_order + "I", buffer, entry_offset + 8)
        if value_type in type_formats:
            tags[tag] = struct.unpack_from(byte_order + type_formats[value_type] * value_count,
                                           buffer, payload_offset)
        else:
            tags[tag] = (bytes(buffer[payload_offset:payload_offset + payload_bytes]),)

    width, height = tags[256][0], tags[257][0]
    if tags[258][0] != 32 or tags.get(339, (1,))[0] != 3:
        raise ValueError("expected 32-bit float samples")
    if tags[259][0] != 1:
        raise ValueError("expected uncompressed samples")

    sample_dtype = np.dtype(np.float32).newbyteorder(byte_order)
    image = np.empty((height, width), np.float32)
    if 322 in tags:
        image = read_tiled(buffer, tags, sample_dtype, width, height)
    else:
        image = read_stripped(buffer, tags, sample_dtype, width, height)

    pixel_scale = tags[33550]
    tiepoint = tags[33922]
    return image, (tiepoint[3], tiepoint[4]), (pixel_scale[0], pixel_scale[1])


def read_tiled(buffer, tags, sample_dtype, width, height):
    tile_width, tile_height = tags[322][0], tags[323][0]
    tiles_across = (width + tile_width - 1) // tile_width
    image = np.empty((height, width), np.float32)
    for tile_index, tile_offset in enumerate(tags[324]):
        tile = np.frombuffer(buffer, sample_dtype, tile_width * tile_height,
                             tile_offset).reshape(tile_height, tile_width)
        top = (tile_index // tiles_across) * tile_height
        left = (tile_index % tiles_across) * tile_width
        rows = min(tile_height, height - top)
        columns = min(tile_width, width - left)
        image[top:top + rows, left:left + columns] = tile[:rows, :columns]
    return image


def read_stripped(buffer, tags, sample_dtype, width, height):
    image = np.empty((height, width), np.float32)
    row = 0
    for strip_offset, strip_bytes in zip(tags[273], tags[279]):
        strip = np.frombuffer(buffer, sample_dtype, strip_bytes // sample_dtype.itemsize,
                              strip_offset).reshape(-1, width)
        image[row:row + strip.shape[0]] = strip
        row += strip.shape[0]
    return image


def extract_coverage_part(response_bytes):
    """WCS 2.0 multipart/related: the GML part first, then the coverage."""
    boundary = b"\n--wcs"
    parts = response_bytes.split(boundary)
    for part in reversed(parts):
        for magic in (b"II*\x00", b"MM\x00*"):
            start = part.find(magic)
            if start >= 0:
                return part[start:]
    raise ValueError("no coverage part in the WCS response")


def request_tile(easting_range, northing_range, sample_counts, coverage_id=COVERAGE_ID):
    """One GetCoverage over an axis-aligned box, resampled server-side."""
    query = [
        ("service", "WCS"),
        ("version", "2.0.1"),
        ("request", "GetCoverage"),
        ("coverageId", coverage_id),
        ("subset", "x(%.4f,%.4f)" % easting_range),
        ("subset", "y(%.4f,%.4f)" % northing_range),
        ("scalesize", "x(%d),y(%d)" % sample_counts),
        ("format", "image/tiff"),
    ]
    url = WCS_ENDPOINT + "?" + urllib.parse.urlencode(query, safe="(),")

    for attempt in range(1, REQUEST_ATTEMPTS + 1):
        try:
            with urllib.request.urlopen(url, timeout=REQUEST_TIMEOUT_SECONDS) as response:
                return decode_tiff(extract_coverage_part(response.read()))
        except (urllib.error.URLError, ValueError, OSError, struct.error) as error:
            if attempt == REQUEST_ATTEMPTS:
                raise
            print("    request failed (%s), retrying" % error)
            time.sleep(2.0 * attempt)
    raise AssertionError("unreachable")


def check_window_inside_coverage(easting_range, northing_range):
    inside = (easting_range[0] >= COVERAGE_EASTING_RANGE[0]
              and easting_range[1] <= COVERAGE_EASTING_RANGE[1]
              and northing_range[0] >= COVERAGE_NORTHING_RANGE[0]
              and northing_range[1] <= COVERAGE_NORTHING_RANGE[1])
    if inside:
        return
    raise SystemExit(
        "window E %.0f..%.0f N %.0f..%.0f leaves the coverage E %.0f..%.0f N %.0f..%.0f\n"
        "Pick a centre further inside Tirol, or a smaller --extent-meters."
        % (easting_range + northing_range + COVERAGE_EASTING_RANGE + COVERAGE_NORTHING_RANGE))


def survey_void_fraction(easting_range, northing_range):
    """Share of the window the model has no ground for, from one coarse request.

    The state boundary is where this bites: a window that reaches into Italy or
    Bavaria comes back as zeroes, and finding that out after an hour of tiles is
    worse than one extra request now.
    """
    sample_count = max(int(round((easting_range[1] - easting_range[0]) / SURVEY_RESOLUTION_METERS)), 2)
    survey, _, _ = request_tile(easting_range, northing_range,
                                (sample_count, sample_count), SURVEY_COVERAGE_ID)
    void = ~np.isfinite(survey)
    void |= survey < PLAUSIBLE_ALTITUDE_RANGE[0]
    void |= survey > PLAUSIBLE_ALTITUDE_RANGE[1]
    return float(void.mean())


def fetch_window(easting_min, northing_max, sample_count, resolution_meters):
    """Mosaic of the whole window, row 0 north, sampled at cell centres."""
    heights = np.empty((sample_count, sample_count), np.float32)
    tile_starts = list(range(0, sample_count, MAX_TILE_SAMPLES))
    tile_total = len(tile_starts) ** 2
    tile_number = 0

    for row_start in tile_starts:
        rows = min(MAX_TILE_SAMPLES, sample_count - row_start)
        for column_start in tile_starts:
            columns = min(MAX_TILE_SAMPLES, sample_count - column_start)
            tile_number += 1

            easting_range = (easting_min + column_start * resolution_meters,
                             easting_min + (column_start + columns) * resolution_meters)
            northing_range = (northing_max - (row_start + rows) * resolution_meters,
                              northing_max - row_start * resolution_meters)

            started = time.monotonic()
            tile, tile_origin, tile_scale = request_tile(easting_range, northing_range, (columns, rows))
            if tile.shape != (rows, columns):
                raise SystemExit("service returned %dx%d samples, expected %dx%d"
                                 % (tile.shape[1], tile.shape[0], columns, rows))
            if abs(tile_scale[0] - resolution_meters) > 1e-6 or abs(tile_origin[0] - easting_range[0]) > 1e-3:
                raise SystemExit("service returned a differently georeferenced tile than requested")

            heights[row_start:row_start + rows, column_start:column_start + columns] = tile
            print("  tile %d/%d  E %.0f..%.0f  N %.0f..%.0f  %.1f s"
                  % (tile_number, tile_total, easting_range[0], easting_range[1],
                     northing_range[0], northing_range[1], time.monotonic() - started))
    return heights


def fill_voids(heights):
    """Replace implausible samples by spreading their nearest valid neighbours inward."""
    void_mask = ~np.isfinite(heights)
    void_mask |= heights < PLAUSIBLE_ALTITUDE_RANGE[0]
    void_mask |= heights > PLAUSIBLE_ALTITUDE_RANGE[1]
    void_count = int(void_mask.sum())
    if void_count == 0:
        return 0
    if void_count == heights.size:
        raise SystemExit("the whole window is void: is the centre inside Tirol?")

    heights[void_mask] = np.nan
    while np.isnan(heights).any():
        padded = np.pad(heights, 1, constant_values=np.nan)
        neighbours = np.stack([padded[:-2, 1:-1], padded[2:, 1:-1], padded[1:-1, :-2], padded[1:-1, 2:]])
        with np.errstate(invalid="ignore"):
            filled = np.nanmean(neighbours, axis=0)
        heights[:] = np.where(np.isnan(heights), filled, heights)
    return void_count


def write_dem(path, heights, resolution_meters, easting_min, northing_max, label):
    minimum = float(heights.min())
    maximum = float(heights.max())
    # uint16 over the window's own altitude band: a 3000 m band quantises to
    # 4.6 cm, well under the source model's own vertical accuracy.
    altitude_step = max((maximum - minimum) / 65535.0, 1.0e-6)
    quantised = np.rint((heights - minimum) / altitude_step).astype(np.uint16)

    header = bytearray(DEM_HEADER_BYTES)
    struct.pack_into("<8sIIII", header, 0, DEM_MAGIC, DEM_VERSION,
                     heights.shape[1], heights.shape[0], 0)
    struct.pack_into("<ddddd", header, 24, resolution_meters, minimum, altitude_step,
                     easting_min, northing_max)
    struct.pack_into("<32s", header, 64, SOURCE_CRS.encode("ascii"))
    struct.pack_into("<96s", header, 96, label.encode("utf-8")[:95])

    with open(path, "wb") as dem_file:
        dem_file.write(header)
        dem_file.write(quantised.astype("<u2").tobytes())
    return minimum, maximum


def write_png(path, image_rgb):
    height, width, _ = image_rgb.shape
    raw = b"".join(b"\x00" + image_rgb[row].tobytes() for row in range(height))

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    with open(path, "wb") as png_file:
        png_file.write(b"\x89PNG\r\n\x1a\n")
        png_file.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        png_file.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        png_file.write(chunk(b"IEND", b""))


def write_preview(path, heights, resolution_meters, preview_size=1200):
    """Hillshade over an altitude ramp: enough to recognise the valley at a glance."""
    stride = max(1, heights.shape[0] // preview_size)
    sampled = heights[::stride, ::stride].astype(np.float64)
    spacing = resolution_meters * stride

    # Row 0 is north, so the row derivative runs the opposite way to northing.
    gradient_row, gradient_east = np.gradient(sampled, spacing)
    gradient_north = -gradient_row

    sun_azimuth = math.radians(315.0)
    sun_altitude = math.radians(45.0)
    sun = (math.sin(sun_azimuth) * math.cos(sun_altitude),
           math.cos(sun_azimuth) * math.cos(sun_altitude),
           math.sin(sun_altitude))
    normal_length = np.sqrt(gradient_east**2 + gradient_north**2 + 1.0)
    shade = np.clip((-gradient_east * sun[0] - gradient_north * sun[1] + sun[2]) / normal_length, 0.0, 1.0)

    band = np.clip((sampled - sampled.min()) / max(float(np.ptp(sampled)), 1.0), 0.0, 1.0)
    ramp = np.stack([0.35 + 0.65 * band, 0.40 + 0.55 * band, 0.35 + 0.45 * band], axis=-1)
    write_png(path, np.clip(ramp * shade[..., None] * 255.0, 0, 255).astype(np.uint8))


def main():
    arguments = parse_arguments()
    resolution_meters = arguments.resolution_meters
    sample_count = int(round(arguments.extent_meters / resolution_meters))
    if sample_count < 2:
        raise SystemExit("--extent-meters must cover at least two samples")

    center_easting, center_northing = wgs84_to_gk_west(arguments.lat, arguments.lon)
    half_extent = sample_count * resolution_meters * 0.5
    easting_range = (center_easting - half_extent, center_easting + half_extent)
    northing_range = (center_northing - half_extent, center_northing + half_extent)
    check_window_inside_coverage(easting_range, northing_range)

    print("Tirol DTM: %.4f N %.4f E, %.1f x %.1f km at %.2f m -> %d x %d samples"
          % (arguments.lat, arguments.lon, sample_count * resolution_meters / 1000.0,
             sample_count * resolution_meters / 1000.0, resolution_meters, sample_count, sample_count))
    print("  EPSG:31254 window E %.0f..%.0f  N %.0f..%.0f" % (easting_range + northing_range))

    void_fraction = survey_void_fraction(easting_range, northing_range)
    if void_fraction > VOID_FRACTION_TOLERANCE:
        raise SystemExit(
            "%.2f%% of this window has no terrain model — it reaches outside Tirol.\n"
            "Move --lat/--lon further into the state or shrink --extent-meters."
            % (100.0 * void_fraction))

    started = time.monotonic()
    heights = fetch_window(easting_range[0], northing_range[1], sample_count, resolution_meters)
    void_count = fill_voids(heights)
    if void_count > 0:
        print("  filled %d void samples (%.4f%%)" % (void_count, 100.0 * void_count / heights.size))

    os.makedirs(arguments.output_directory, exist_ok=True)
    dem_path = os.path.join(arguments.output_directory, arguments.name + ".massifdem")
    label = "Tirol DGM 50cm @ %.4fN %.4fE" % (arguments.lat, arguments.lon)
    minimum, maximum = write_dem(dem_path, heights, resolution_meters,
                                 easting_range[0], northing_range[1], label)
    write_preview(os.path.join(arguments.output_directory, arguments.name + "_preview.png"),
                  heights, resolution_meters)

    print("Wrote %s: altitude %.0f..%.0f m, %.1f MB, %.0f s total"
          % (dem_path, minimum, maximum, os.path.getsize(dem_path) / 1.0e6, time.monotonic() - started))
    print("Source: Land Tirol, Digitales Geländemodell Tirol (CC BY 4.0), via %s" % WCS_ENDPOINT)


if __name__ == "__main__":
    main()
