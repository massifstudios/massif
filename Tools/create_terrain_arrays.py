"""Builds the terrain's Texture2DArray assets from the surface layer table.

The terrain material samples two layers per pixel out of a table of thirty, and
which two is only known per pixel, so the sets have to live in an array the
shader can index rather than in thirty separate texture parameters. One array
per map: base colour, normal, the packed ARM, and height for the parallax.

Slice order is the order of MassifSurfaceLayers.h, which this reads: the
classification writes slice indices, so the two have to agree. Rerun this
whenever a layer is added, removed or reordered.

Textures come from Tools/fetch_polyhaven_textures.py, and their height maps are
stretched onto the full range by Tools/normalise_height_maps.py; run both first.

Run headless with:
UnrealEditor-Cmd Massif.uproject -run=pythonscript \
    -script=Tools/create_terrain_arrays.py -unattended -nosplash
"""

import os
import re

import unreal

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAYER_TABLE_PATH = os.path.join(
    REPO_ROOT, "Source", "MassifPresentation", "Private", "Terrain", "MassifSurfaceLayers.h")

TEXTURE_SOURCE_ROOT = os.path.join(REPO_ROOT, "Content", "Textures", "PolyHaven")
TEXTURE_ASSET_ROOT = "/Game/Textures/PolyHaven"
ARRAY_ASSET_PATH = "/Game/Textures/Terrain"

# Base colour, normal, and the ARM pack Poly Haven ships occlusion, roughness
# and metallic in. Height drives the parallax and the layer boundary.
#
# HN rather than H: the material reads a height map as running 0 to 1 over the
# layer's ReliefMeters, which the maps as fetched do not do. Run
# Tools/normalise_height_maps.py to write them.
ARRAY_SUFFIXES = ("D", "N", "ARM", "HN")

ARRAY_NAMES = {
    "D": "TA_GroundColor",
    "N": "TA_GroundNormal",
    "ARM": "TA_GroundArm",
    "HN": "TA_GroundHeight",
}

# A slice of an array has to match every other slice, so everything is resized
# to this on import. 2k is what the manifest fetches.
ARRAY_RESOLUTION = 2048


def read_layer_table():
    """(Name, Group, Set) per layer, in the order the header declares them."""
    with open(LAYER_TABLE_PATH, "r", encoding="utf-8") as handle:
        source = handle.read()

    # Each entry names all three in the same order, so one scan over the
    # designated initialisers keeps them together without parsing C++.
    fields = re.findall(
        r'\.(Name|TextureGroup|TextureSet)\s*=\s*TEXT\("([^"]+)"\)', source)
    layers = []
    pending = {}
    for key, value in fields:
        pending[key] = value
        if len(pending) == 3:
            layers.append((pending["Name"], pending["TextureGroup"], pending["TextureSet"]))
            pending = {}
    return layers


def read_snow_set():
    with open(LAYER_TABLE_PATH, "r", encoding="utf-8") as handle:
        source = handle.read()
    group = re.search(r'SnowTextureGroup\s*=\s*TEXT\("([^"]+)"\)', source)
    texture_set = re.search(r'SnowTextureSet\s*=\s*TEXT\("([^"]+)"\)', source)
    if group is None or texture_set is None:
        raise RuntimeError("MassifSurfaceLayers.h names no snow texture set")
    return group.group(1), texture_set.group(1)


def configure_texture(texture, suffix):
    """Normal, ARM and height maps are data, not colour: linear, and normals compressed as such."""
    if suffix != "D":
        texture.set_editor_property("srgb", False)
    if suffix == "N":
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
    if suffix == "HN":
        # One channel, so the greyscale format rather than a colour one carrying
        # the same value three times.
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_GRAYSCALE)
    texture.set_editor_property("max_texture_size", ARRAY_RESOLUTION)
    unreal.EditorAssetLibrary.save_loaded_asset(texture)


def import_texture(asset_tools, group, texture_set, suffix):
    # Files carry the UE texture prefix; the layer table names the set without it.
    asset_path = f"{TEXTURE_ASSET_ROOT}/{group}/T_{texture_set}_{suffix}"
    if not unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        source = os.path.join(TEXTURE_SOURCE_ROOT, group, f"T_{texture_set}_{suffix}.jpg")
        if not os.path.isfile(source):
            unreal.log_warning(f"Missing texture source {source}")
            return None
        task = unreal.AssetImportTask()
        task.filename = source
        task.destination_path = f"{TEXTURE_ASSET_ROOT}/{group}"
        task.automated = True
        task.save = True
        asset_tools.import_asset_tasks([task])

    texture = unreal.EditorAssetLibrary.load_asset(asset_path)
    if texture is None:
        unreal.log_warning(f"Import produced no asset at {asset_path}")
        return None
    configure_texture(texture, suffix)
    return texture


def build_array(asset_tools, name, textures):
    """Refills an existing array rather than replacing it, so the material's references survive."""
    asset_path = f"{ARRAY_ASSET_PATH}/{name}"
    array = unreal.EditorAssetLibrary.load_asset(asset_path)
    if array is None:
        array = asset_tools.create_asset(
            name, ARRAY_ASSET_PATH, unreal.Texture2DArray, unreal.Texture2DArrayFactory())

    array.set_editor_property("source_textures", textures)
    unreal.EditorAssetLibrary.save_loaded_asset(array)
    unreal.log(f"Built {asset_path} with {len(textures)} slices")
    return array


def build_layer_arrays(asset_tools, entries):
    for suffix in ARRAY_SUFFIXES:
        textures = []
        for name, group, texture_set in entries:
            texture = import_texture(asset_tools, group, texture_set, suffix)
            if texture is None:
                unreal.log_warning(f"Layer {name} has no {suffix} map; arrays not built")
                return False
            textures.append(texture)
        build_array(asset_tools, ARRAY_NAMES[suffix], textures)
    return True


def main():
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    layers = read_layer_table()
    if not layers:
        raise RuntimeError(f"No surface layers found in {LAYER_TABLE_PATH}")

    # Snow is the slice after the last layer. It is blended over the finished
    # ground rather than chosen against it, but it is drawn by the same code, so
    # it rides in the same arrays.
    snow_group, snow_set = read_snow_set()
    entries = layers + [("Snow", snow_group, snow_set)]
    unreal.log(f"Surface layers: {len(layers)}, plus snow as slice {len(layers)}")

    build_layer_arrays(asset_tools, entries)


main()
