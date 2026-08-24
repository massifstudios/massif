"""Generates a terrain model with InfiniteDiffusion and writes it as a .massifdem.

InfiniteDiffusion (Goslin, SIGGRAPH 2026, https://github.com/xandergos/terrain-diffusion)
samples a diffusion model lazily over an unbounded canvas, so a seed defines a
whole Earth-like world and any window of it can be asked for directly. This
takes one window out of that world and writes it in the container the engine
reads, so a generated massif and a surveyed one are the same file to the game.

The world is Earth-like, which means about half of it is ocean and much of the
rest is unremarkable. So a window is not taken at the origin: a large region is
sampled once, every candidate window in it is scored for being wholly above sea
level and for how much relief it carries, and the best one is written out.

Deterministic: the same seed and the same window arguments produce the same
terrain, which is the property that makes this usable as world generation rather
than as a one-off asset.

Resolution is the model's own, 30 m per pixel. That is an order of magnitude
coarser than the surveyed Tirol model, and nothing below thirty metres exists in
the output — there is no crag detail and no channel narrower than a pixel.

Runs under the terrain-diffusion virtual environment rather than the system
Python, and from that checkout, because the pipeline resolves its global climate
rasters relative to the working directory:

    task diffusion-dem -- --seed 1337
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from massif_dem import write_dem, write_preview  # noqa: E402

DEFAULT_MODEL = "xandergos/terrain-diffusion-30m"

# There is no survey and no projection behind these metres: the window's own
# north-west corner is the origin, and the field records what made it.
COORDINATE_SYSTEM = "INFINITEDIFFUSION"

# How much of the world to look at before choosing a window from it. Sampling is
# lazy and fast — a hundred kilometres of coastline and mountain costs seconds —
# and a search this wide is what makes the difference between a window that is
# all sea and one worth building a resort in.
DEFAULT_SURVEY_PIXELS = 4096

# Candidate windows are placed on a lattice this many pixels apart, which is also
# the block the search reduces over. Finer finds a slightly better window and
# costs scoring time, not generation time.
CANDIDATE_STRIDE_PIXELS = 32

# How much of the region is asked for at once. Sampling is lazy and seamless, so
# a region is the same terrain however it is cut up — but a hundred kilometres in
# one call is tens of gigabytes of video memory.
SAMPLE_CHUNK_PIXELS = 2048

# Lowest altitude a window may contain. Above sea level by a clear margin, so the
# window holds no coast and no lake bed the game would have to explain.
DEFAULT_MIN_ALTITUDE_METERS = 50.0


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--extent-meters", type=float, default=10000.0,
                        help="side of the square window in meters")
    parser.add_argument("--seed", type=int, default=1337,
                        help="world seed: picks which Earth-like world the window comes from")
    parser.add_argument("--pick", type=int, default=0,
                        help="which of the scored windows to take, 0 being the best")
    parser.add_argument("--min-altitude", type=float, default=DEFAULT_MIN_ALTITUDE_METERS,
                        help="lowest altitude the window may contain, in meters")
    parser.add_argument("--survey-pixels", type=int, default=DEFAULT_SURVEY_PIXELS,
                        help="side of the region searched for a window, in model pixels")
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--terrain-diffusion", default=os.getcwd(),
                        help="the terrain-diffusion checkout to import and to resolve its data against")
    parser.add_argument("--name", default="diffusion",
                        help="output file stem under the terrain directory")
    parser.add_argument("--output-directory", default=os.path.join("Data", "Terrain"))
    return parser.parse_args()


def sample_world(checkout, model, seed, survey_pixels):
    """The region the window is chosen from, as altitudes in metres."""
    # Imported here and from an explicit checkout: the package is not installed,
    # and a script run from elsewhere puts its own directory on the path, not the
    # one it was launched from.
    sys.path.insert(0, os.path.abspath(checkout))
    from terrain_diffusion.inference.world_pipeline import WorldPipeline

    world = WorldPipeline.from_pretrained(
        model, seed=seed, caching_strategy="direct", torch_compile=False)
    world.to("cuda")
    world.bind(hdf5_file=None)

    altitudes = np.empty((survey_pixels, survey_pixels), dtype=np.float32)
    tiles = -(-survey_pixels // SAMPLE_CHUNK_PIXELS)
    with world:
        for tile_index in range(tiles * tiles):
            row = (tile_index // tiles) * SAMPLE_CHUNK_PIXELS
            column = (tile_index % tiles) * SAMPLE_CHUNK_PIXELS
            row_end = min(row + SAMPLE_CHUNK_PIXELS, survey_pixels)
            column_end = min(column + SAMPLE_CHUNK_PIXELS, survey_pixels)
            tile = world.get(row, column, row_end, column_end, with_climate=False)["elev"]
            altitudes[row:row_end, column:column_end] = tile.numpy()
            print("  sampled %d of %d tiles" % (tile_index + 1, tiles * tiles), flush=True)
    return altitudes, float(world.native_resolution)


def block_extremes(altitudes, block_pixels):
    """Lowest and highest altitude within each block of the surveyed region."""
    rows = altitudes.shape[0] // block_pixels
    columns = altitudes.shape[1] // block_pixels
    trimmed = altitudes[:rows * block_pixels, :columns * block_pixels]
    blocks = trimmed.reshape(rows, block_pixels, columns, block_pixels)
    return blocks.min(axis=(1, 3)), blocks.max(axis=(1, 3))


def window_extremes(altitudes, window_pixels):
    """
    Lowest and highest altitude of every window on the candidate lattice, taken
    over blocks rather than over pixels.

    A hundred-kilometre window holds eleven million samples and there are tens of
    thousands of candidates, so reducing each window directly is a quarter of a
    trillion reads. Reducing blocks once and then windows of blocks is the same
    answer for a thousandth of the work.

    The window is rounded up to whole blocks, so what is measured covers a little
    more ground than the window it stands for: being wholly above sea level is
    then guaranteed rather than merely likely, and the relief is at worst a small
    overestimate.
    """
    from numpy.lib.stride_tricks import sliding_window_view

    block_lowest, block_highest = block_extremes(altitudes, CANDIDATE_STRIDE_PIXELS)
    window_blocks = -(-window_pixels // CANDIDATE_STRIDE_PIXELS)
    if window_blocks > block_lowest.shape[0]:
        return np.empty((0, 0)), np.empty((0, 0))

    lowest = sliding_window_view(block_lowest, (window_blocks, window_blocks)).min(axis=(2, 3))
    highest = sliding_window_view(block_highest, (window_blocks, window_blocks)).max(axis=(2, 3))
    return lowest, highest


def score_windows(altitudes, window_pixels, min_altitude_meters):
    """Every candidate window that is wholly land, most relief first."""
    lowest, highest = window_extremes(altitudes, window_pixels)
    relief = highest - lowest

    rows, columns = np.nonzero(lowest >= min_altitude_meters)
    if rows.size == 0:
        return []

    order = np.argsort(-relief[rows, columns])
    return [(int(rows[index] * CANDIDATE_STRIDE_PIXELS),
             int(columns[index] * CANDIDATE_STRIDE_PIXELS),
             float(relief[rows[index], columns[index]]),
             float(lowest[rows[index], columns[index]]))
            for index in order]


def main():
    arguments = parse_arguments()
    if arguments.survey_pixels % 256 != 0:
        raise SystemExit("--survey-pixels must be a multiple of 256")

    print("InfiniteDiffusion: seed %d, surveying %d x %d px for a %.1f km window"
          % (arguments.seed, arguments.survey_pixels, arguments.survey_pixels,
             arguments.extent_meters / 1000.0))

    altitudes, resolution_meters = sample_world(
        arguments.terrain_diffusion, arguments.model, arguments.seed, arguments.survey_pixels)
    window_pixels = int(round(arguments.extent_meters / resolution_meters))
    if window_pixels < 4 or window_pixels + CANDIDATE_STRIDE_PIXELS > arguments.survey_pixels:
        raise SystemExit(
            "A %.1f km window is %d px, which leaves nothing to choose from in a %d px survey.\n"
            "Raise --survey-pixels to at least %d."
            % (arguments.extent_meters / 1000.0, window_pixels, arguments.survey_pixels,
               256 * -(-(window_pixels + CANDIDATE_STRIDE_PIXELS) // 256)))

    print("  surveyed %.0f x %.0f km at %.0f m/px, altitude %.0f..%.0f m"
          % (arguments.survey_pixels * resolution_meters / 1000.0,
             arguments.survey_pixels * resolution_meters / 1000.0, resolution_meters,
             altitudes.min(), altitudes.max()))

    candidates = score_windows(altitudes, window_pixels, arguments.min_altitude)
    if not candidates:
        raise SystemExit(
            "No window of this size sits wholly above %.0f m in that world.\n"
            "Try another --seed, a wider --survey-pixels, or a lower --min-altitude."
            % arguments.min_altitude)
    if arguments.pick >= len(candidates):
        raise SystemExit("--pick %d, but only %d windows qualified"
                         % (arguments.pick, len(candidates)))

    row, column, relief_meters, lowest_meters = candidates[arguments.pick]
    heights = altitudes[row:row + window_pixels, column:column + window_pixels]
    print("  %d windows qualified; taking #%d at pixel %d, %d: %.0f m of relief over %.0f m"
          % (len(candidates), arguments.pick, column, row, relief_meters, lowest_meters))

    os.makedirs(arguments.output_directory, exist_ok=True)
    dem_path = os.path.join(arguments.output_directory, arguments.name + ".massifdem")
    label = "InfiniteDiffusion %s seed %d window %d,%d" % (
        arguments.model.rsplit("/", 1)[-1], arguments.seed, column, row)
    minimum, maximum = write_dem(dem_path, heights, resolution_meters,
                                 0.0, window_pixels * resolution_meters, label,
                                 COORDINATE_SYSTEM)
    write_preview(os.path.join(arguments.output_directory, arguments.name + "_preview.png"),
                  heights, resolution_meters)

    print("Wrote %s: %d x %d samples at %.0f m, altitude %.0f..%.0f m, %.1f MB"
          % (dem_path, window_pixels, window_pixels, resolution_meters, minimum, maximum,
             os.path.getsize(dem_path) / 1.0e6))


if __name__ == "__main__":
    main()
