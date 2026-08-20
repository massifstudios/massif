#!/usr/bin/env python3
"""Download CC0 vegetation models from Poly Haven into Content/Models/PolyHaven.

Same deal as fetch_polyhaven_textures.py: public API, no key, everything CC0.
The downloads are not committed; this script and its manifest are the source of
truth, and Tools/create_foliage_content.py turns them into UE assets.

    python3 Tools/fetch_polyhaven_models.py                 # the whole manifest
    python3 Tools/fetch_polyhaven_models.py --group tree
    python3 Tools/fetch_polyhaven_models.py --resolution 2k
    python3 Tools/fetch_polyhaven_models.py --list trees    # what is available

Each asset lands as Content/Models/PolyHaven/<group>/<slug>/<slug>_<res>.gltf
with the .bin geometry and textures/ folder beside it, which is the layout the
glTF's relative paths expect.

glTF rather than FBX: Poly Haven's FBX bundles for these assets arrive as
archives around a .blend, which Unreal cannot open, while the glTF variant is a
plain .gltf plus .bin that Interchange imports directly.

Poly Haven's conifers are scan-grade with no authored LODs: pine_tree_01 is
about 960 MB of geometry and fir_tree_01 about 490 MB. They are meant to be
imported with Nanite on. The saplings and ground cover are far smaller.
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
OUTPUT_ROOT = REPO_ROOT / "Content" / "Models" / "PolyHaven"

# The download variant to ask the API for, and the extension it lands under.
MODEL_FORMAT = "gltf"

# Poly Haven slugs per group, one per species in MassifFoliageSpecies. Conifers
# for the treeline forests, saplings and ground cover for the layer below them.
# Extend as more assets are wanted; anything missing from a download is
# skipped, never fatal.
GROUPS = {
    "tree": ["fir_tree_01", "pine_tree_01"],
    "sapling": ["fir_sapling_medium", "pine_sapling_medium"],
    "groundcover": ["grass_medium_01", "grass_medium_02", "shrub_01"],
}

# Categories to browse with --list, so picking new slugs does not need the website.
LIST_CATEGORIES = {
    "tree": ["trees"],
    "sapling": ["trees"],
    "groundcover": ["grass", "ground cover"],
}

REQUEST_HEADERS = {"User-Agent": "massif-model-fetch/1"}
RETRY_COUNT = 4


def fetch_json(url):
    last_error = None
    for _ in range(RETRY_COUNT):
        try:
            request = urllib.request.Request(url, headers=REQUEST_HEADERS)
            with urllib.request.urlopen(request, timeout=120) as response:
                return json.load(response)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            last_error = error
    raise last_error


def list_assets(categories):
    found = {}
    for category in categories:
        url = f"{API_ROOT}/assets?t=models&c={urllib.parse.quote(category)}"
        try:
            found.update(fetch_json(url))
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            print(f"  ! category '{category}' failed: {error}", file=sys.stderr)
    return sorted(found, key=lambda slug: -found[slug].get("download_count", 0))


def download(url, destination, expected_bytes):
    """
    Resumable: these are hundreds of megabytes over a CDN that hands out a few
    kilobytes a second, so a run that dies partway has to pick up where it left
    off rather than start again. The .part file's length is the resume offset.
    """
    if destination.exists() and destination.stat().st_size == expected_bytes:
        return False

    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_suffix(destination.suffix + ".part")
    have_bytes = partial.stat().st_size if partial.exists() else 0
    if have_bytes >= expected_bytes:
        have_bytes = 0

    headers = dict(REQUEST_HEADERS)
    if have_bytes > 0:
        headers["Range"] = f"bytes={have_bytes}-"

    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request, timeout=600) as response:
        resumed = response.status == 206
        mode = "ab" if resumed else "wb"
        with partial.open(mode) as handle:
            shutil.copyfileobj(response, handle)

    if partial.stat().st_size != expected_bytes:
        raise urllib.error.URLError(
            f"got {partial.stat().st_size} of {expected_bytes} bytes, rerun to resume"
        )
    partial.rename(destination)
    return True


def report(destination, written, size_bytes):
    relative = destination.relative_to(REPO_ROOT)
    marker = "+" if written else "="
    print(f"  {marker} {relative} ({size_bytes / 1e6:.1f} MB)", flush=True)


def fetch_asset(slug, group, resolution):
    """Downloads one asset's glTF plus the geometry and textures it references."""
    try:
        files = fetch_json(f"{API_ROOT}/files/{urllib.parse.quote(slug)}")
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
        print(f"  ! {slug}: {error}", file=sys.stderr)
        return False

    entry = files.get(MODEL_FORMAT, {}).get(resolution, {}).get(MODEL_FORMAT)
    if entry is None:
        print(f"  ! {slug}: no {MODEL_FORMAT} at {resolution}", file=sys.stderr)
        return False

    asset_directory = OUTPUT_ROOT / group / slug
    payload = [(f"{slug}_{resolution}.{MODEL_FORMAT}", entry)]
    payload += [(relative, included) for relative, included in entry.get("include", {}).items()]

    for relative, item in payload:
        destination = asset_directory / relative
        try:
            written = download(item["url"], destination, item["size"])
            report(destination, written, item["size"])
        except (urllib.error.URLError, TimeoutError) as error:
            print(f"  ! {destination.name}: {error}", file=sys.stderr)
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--resolution", default="1k", help="texture resolution: 1k, 2k, 4k or 8k (default 1k)")
    parser.add_argument("--group", nargs="*", default=sorted(GROUPS), help="which model groups to fetch")
    parser.add_argument("--slugs", nargs="*", help="explicit Poly Haven slugs, put in the first named group")
    parser.add_argument("--list", metavar="GROUP", help="print available slugs for a group and exit")
    arguments = parser.parse_args()

    if arguments.list:
        categories = LIST_CATEGORIES.get(arguments.list, [arguments.list])
        for slug in list_assets(categories):
            print(slug)
        return 0

    if arguments.slugs:
        group = arguments.group[0] if arguments.group else "tree"
        print(f"{group}: {len(arguments.slugs)} explicit assets at {arguments.resolution}")
        for slug in arguments.slugs:
            fetch_asset(slug, group, arguments.resolution)
        return 0

    for group in arguments.group:
        slugs = GROUPS.get(group)
        if slugs is None:
            print(f"unknown group '{group}', known: {', '.join(sorted(GROUPS))}", file=sys.stderr)
            continue
        print(f"{group}: {len(slugs)} assets at {arguments.resolution} -> {', '.join(slugs)}", flush=True)
        for slug in slugs:
            fetch_asset(slug, group, arguments.resolution)

    print(f"\nDownloaded into {OUTPUT_ROOT.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
