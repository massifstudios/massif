#!/usr/bin/env python3
"""Download CC0 PBR texture sets from Poly Haven into Content/Textures/PolyHaven.

Poly Haven's API is public and needs no key. Everything there is CC0, so the
downloads can be used and redistributed without attribution (crediting is still
polite).

The downloads are not committed: this script plus its manifest is the source of
truth, and the files are re-fetchable at any time. Run it once after cloning.

    python3 Tools/fetch_polyhaven_textures.py                  # the whole manifest
    python3 Tools/fetch_polyhaven_textures.py --resolution 4k
    python3 Tools/fetch_polyhaven_textures.py --group rock snow
    python3 Tools/fetch_polyhaven_textures.py --list rock      # what is available
    python3 Tools/fetch_polyhaven_textures.py --slugs rocks_ground_06

Files land as Content/Textures/PolyHaven/<group>/T_<Slug>_<Suffix>.<ext>, using
the UE prefix convention so imported assets keep stable names.
"""

import argparse
import json
import shutil
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

API_ROOT = "https://api.polyhaven.com"
REPO_ROOT = Path(__file__).resolve().parent.parent
OUTPUT_ROOT = REPO_ROOT / "Content" / "Textures" / "PolyHaven"

# Poly Haven map names vary by asset. Each entry is the suffix we save under and
# the API keys that may hold it, best first.
MAP_ALIASES = [
    ("D", ["Diffuse", "diffuse", "albedo", "Albedo", "col", "Color"]),
    ("N", ["nor_gl", "nor_dx", "Normal", "normal"]),
    ("R", ["Rough", "rough", "Roughness"]),
    ("AO", ["AO", "ao"]),
    ("H", ["Displacement", "displacement", "Height", "height", "disp"]),
    ("ARM", ["arm", "ARM"]),
]

# The sets the terrain material is built from, named rather than picked by
# download rank: which surfaces the massif needs is a look decision, and a
# re-run months later must fetch the same files the material's layer table
# names. Categories below are only for browsing with --list.
GROUP_SLUGS = {
    "rock": [
        "aerial_grass_rock",
        "aerial_rocks_02",
        "gray_rocks",
        "rock_face_03",
        "rocks_ground_03",
        "rocky_terrain",
        "rocky_terrain_02",
        "rocky_terrain_03",
    ],
    "scree": [
        "aerial_beach_01",
        "dry_river_pebbles",
        "gravelly_sand",
        "river_small_rocks",
        "rocky_trail",
        "rocky_trail_02",
        "sand_01",
    ],
    "ground": [
        "brown_mud_leaves_01",
        "coast_sand_rocks_02",
        "dirt",
        "mud_forest",
    ],
    "forest": [
        # forrest_ground_01 is spelled that way on Poly Haven.
        "forrest_ground_01",
        "forest_ground_04",
        "forest_ground_05",
        "forest_ground_06",
        "forest_leaves_02",
        "bark_brown_01",
        "bark_brown_02",
    ],
    "grass": [
        "leafy_grass",
        "sparse_grass",
    ],
    "snow": [
        "snow_01",
        "snow_02",
        "snow_03",
        "snow_field_aerial",
    ],
}

# Categories Poly Haven tags textures with, per surface we need. Only --list
# reads these; the manifest above is what gets fetched.
GROUP_CATEGORIES = {
    "rock": ["rock", "sandstone"],
    "snow": ["snow"],
    "forest": ["collection: pine_forest", "bark"],
    "ground": ["terrain"],
    "grass": ["grass"],
    "scree": ["gravel", "sand"],
}


def fetch_json(url):
    request = urllib.request.Request(url, headers={"User-Agent": "massif-texture-fetch/1"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def list_assets(categories):
    """Texture slugs in any of the given categories, most downloaded first."""
    found = {}
    for category in categories:
        url = f"{API_ROOT}/assets?t=textures&c={urllib.parse.quote(category)}"
        try:
            found.update(fetch_json(url))
        except urllib.error.HTTPError as error:
            print(f"  ! category '{category}' failed: {error}", file=sys.stderr)
    return sorted(found, key=lambda slug: -found[slug].get("download_count", 0))


def pick_file(files, resolution):
    """The best available format for one map at one resolution, jpg preferred for size."""
    at_resolution = files.get(resolution)
    if not at_resolution:
        return None
    for extension in ("jpg", "png", "exr"):
        entry = at_resolution.get(extension)
        if entry and entry.get("url"):
            return entry["url"], extension
    return None


def download(url, destination):
    if destination.exists() and destination.stat().st_size > 0:
        return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_suffix(destination.suffix + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": "massif-texture-fetch/1"})
    with urllib.request.urlopen(request, timeout=300) as response, partial.open("wb") as handle:
        shutil.copyfileobj(response, handle)
    partial.rename(destination)
    return True


def asset_name(slug):
    return "".join(part.capitalize() for part in slug.replace("-", "_").split("_"))


def fetch_asset(slug, group, resolution):
    try:
        files = fetch_json(f"{API_ROOT}/files/{urllib.parse.quote(slug)}")
    except urllib.error.HTTPError as error:
        print(f"  ! {slug}: {error}", file=sys.stderr)
        return 0

    written = 0
    available = 0
    for suffix, aliases in MAP_ALIASES:
        entry = next((files[alias] for alias in aliases if alias in files), None)
        if entry is None:
            continue
        chosen = pick_file(entry, resolution)
        if chosen is None:
            continue
        url, extension = chosen
        available += 1
        destination = OUTPUT_ROOT / group / f"T_{asset_name(slug)}_{suffix}.{extension}"
        try:
            if download(url, destination):
                print(f"  + {destination.relative_to(REPO_ROOT)}")
                written += 1
            else:
                print(f"  = {destination.relative_to(REPO_ROOT)} (already there)")
        except urllib.error.URLError as error:
            print(f"  ! {destination.name}: {error}", file=sys.stderr)

    if available == 0:
        print(f"  ! {slug}: no maps offered at {resolution}", file=sys.stderr)
    return written


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--resolution", default="2k", help="1k, 2k, 4k or 8k (default 2k)")
    parser.add_argument("--group", nargs="*", default=sorted(GROUP_SLUGS), help="which surface groups to fetch")
    parser.add_argument("--slugs", nargs="*", help="explicit Poly Haven asset slugs, put in the first named group")
    parser.add_argument("--list", metavar="GROUP", help="print available slugs for a group and exit")
    arguments = parser.parse_args()

    if arguments.list:
        categories = GROUP_CATEGORIES.get(arguments.list, [arguments.list])
        for slug in list_assets(categories):
            print(slug)
        return 0

    if arguments.slugs:
        group = arguments.group[0] if arguments.group else "rock"
        print(f"{group}: {len(arguments.slugs)} explicit assets at {arguments.resolution}")
        for slug in arguments.slugs:
            fetch_asset(slug, group, arguments.resolution)
        return 0

    for group in arguments.group:
        slugs = GROUP_SLUGS.get(group)
        if slugs is None:
            print(f"unknown group '{group}', known: {', '.join(sorted(GROUP_SLUGS))}", file=sys.stderr)
            continue
        print(f"{group}: {len(slugs)} assets at {arguments.resolution} -> {', '.join(slugs)}")
        for slug in slugs:
            fetch_asset(slug, group, arguments.resolution)

    print(f"\nDownloaded into {OUTPUT_ROOT.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
