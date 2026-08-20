"""Stretches every Poly Haven height map onto its full range.

The terrain material reads a height map as running 0 to 1 over the layer's
ReliefMeters, because that is the only thing an 8-bit greyscale map can be taken
to mean. The maps as fetched do not: most use well under half the range, and the
worst use an eighth. A layer that claims four centimetres of forest floor is then
drawing seven millimetres, which the parallax raymarch has nothing to find in —
and what little it does find moves the whole tile together rather than its parts
against each other, which reads as the ground sliding rather than as depth.

Stretching each map so its own range fills 0 to 1 makes ReliefMeters mean what it
says, and leaves the layer table as the only place the depth of a surface is
written down.

Percentiles rather than the extremes: several maps hold a handful of pure black
or pure white specks over an otherwise narrow range, and normalising against
those would do nothing at all.

Writes T_<Set>_HN.jpg beside the source, which is what
Tools/create_terrain_arrays.py imports. The originals are left alone, so this can
be rerun and the stretch is never applied twice.

Run with the system interpreter, before Tools/create_terrain_arrays.py:
python3 Tools/normalise_height_maps.py
"""

import glob
import os

import numpy as np
from PIL import Image

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXTURE_ROOT = os.path.join(REPO_ROOT, "Content", "Textures", "PolyHaven")

# The range taken to be the map's own, either side. Wide enough to ignore specks,
# narrow enough that real peaks and hollows survive.
LOW_PERCENTILE = 0.5
HIGH_PERCENTILE = 99.5

# A map flatter than this carried almost nothing to begin with, so stretching it
# is mostly stretching the compressor's blocking. Still done — a weak surface is
# better than none — but worth knowing which sets are behind it.
SUSPECT_SPAN = 0.25

JPEG_QUALITY = 95


def normalised(pixels):
    """The map remapped so its own range fills 0 to 1, and the span it had."""
    low = np.percentile(pixels, LOW_PERCENTILE)
    high = np.percentile(pixels, HIGH_PERCENTILE)
    span = float(high - low)
    if span < 1e-4:
        return pixels, span
    return np.clip((pixels - low) / span, 0.0, 1.0), span


def normalise_file(source_path):
    image = Image.open(source_path).convert("L")
    pixels = np.asarray(image, dtype=np.float32) / 255.0

    stretched, span = normalised(pixels)
    output_path = source_path.replace("_H.jpg", "_HN.jpg")
    Image.fromarray((stretched * 255.0 + 0.5).astype(np.uint8), mode="L").save(
        output_path, quality=JPEG_QUALITY)
    return span


def main():
    sources = sorted(glob.glob(os.path.join(TEXTURE_ROOT, "*", "T_*_H.jpg")))
    if not sources:
        raise RuntimeError(f"No height maps under {TEXTURE_ROOT}")

    suspect = []
    for source_path in sources:
        span = normalise_file(source_path)
        if span < SUSPECT_SPAN:
            suspect.append((os.path.basename(source_path), span))

    print(f"Normalised {len(sources)} height maps")
    for name, span in sorted(suspect, key=lambda entry: entry[1]):
        print(f"  thin to begin with, span {span:.2f}: {name}")


main()
