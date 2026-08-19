"""Creates the base content assets: empty L_Massif level and M_Terrain material.

M_Terrain blends Poly Haven texture sets by altitude and slope: rock on steep
faces, snowfield above the snowline, grassy ground on gentle low slopes.
Textures come from Tools/fetch_polyhaven_textures.py (run it first); without
the downloads this falls back to flat colors so a fresh clone still renders.

Idempotent: skips assets that already exist, always recreates the material.
Run headless with:
UnrealEditor-Cmd Massif.uproject -run=pythonscript \
    -script=Tools/create_base_content.py -unattended -nosplash
"""

import os

import unreal

MAP_PATH = "/Game/Maps/L_Massif"
MATERIAL_PATH = "/Game/Materials/M_Terrain"

ROCK_COLOR = (0.145, 0.135, 0.125)
SNOW_COLOR = (0.905, 0.925, 0.965)
GRASS_COLOR = (0.075, 0.145, 0.055)

SNOW_LINE_METERS = 1500.0
SNOW_BAND_METERS = 300.0
GRASS_LINE_METERS = 1350.0
GRASS_BAND_METERS = 200.0

TEXTURE_SOURCE_ROOT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Content", "Textures", "PolyHaven"
)
TEXTURE_ASSET_ROOT = "/Game/Textures/PolyHaven"

# One texture set per surface layer: (layer, source group folder, asset name, tiling meters).
LAYERS = [
    ("rock", "rock", "T_AerialRocks02", 14.0),
    ("ground", "rock", "T_AerialGrassRock", 7.0),
    ("snow", "snow", "T_SnowFieldAerial", 9.0),
]

MAP_SUFFIXES = ("D", "N", "R")


def create_level():
    if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
        unreal.log("Level already exists: " + MAP_PATH)
        return
    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    level_editor.new_level(MAP_PATH)
    level_editor.save_current_level()
    unreal.log("Created level " + MAP_PATH)


def expr(material, expression_class, x, y):
    return unreal.MaterialEditingLibrary.create_material_expression(
        material, expression_class, x, y
    )


def connect(from_expr, from_output, to_expr, to_input):
    unreal.MaterialEditingLibrary.connect_material_expressions(
        from_expr, from_output, to_expr, to_input
    )


def make_altitude_meters(material):
    """World Z in meters."""
    world_position = expr(material, unreal.MaterialExpressionWorldPosition, -1600, -300)
    mask_z = expr(material, unreal.MaterialExpressionComponentMask, -1400, -300)
    mask_z.set_editor_property("r", False)
    mask_z.set_editor_property("g", False)
    mask_z.set_editor_property("b", True)
    mask_z.set_editor_property("a", False)
    connect(world_position, "", mask_z, "")

    to_meters = expr(material, unreal.MaterialExpressionMultiply, -1200, -300)
    to_meters.set_editor_property("const_b", 0.01)
    connect(mask_z, "", to_meters, "A")
    return to_meters


def make_flatness(material, threshold_z=0.62, sharpen_factor=3.0, y=100):
    """1.0 on flat ground, 0.0 on steep faces."""
    vertex_normal = expr(material, unreal.MaterialExpressionVertexNormalWS, -1600, y)
    mask_z = expr(material, unreal.MaterialExpressionComponentMask, -1400, y)
    mask_z.set_editor_property("r", False)
    mask_z.set_editor_property("g", False)
    mask_z.set_editor_property("b", True)
    mask_z.set_editor_property("a", False)
    connect(vertex_normal, "", mask_z, "")

    threshold = expr(material, unreal.MaterialExpressionSubtract, -1200, y)
    threshold.set_editor_property("const_b", threshold_z)
    connect(mask_z, "", threshold, "A")

    sharpen = expr(material, unreal.MaterialExpressionMultiply, -1000, y)
    sharpen.set_editor_property("const_b", sharpen_factor)
    connect(threshold, "", sharpen, "A")

    saturate = expr(material, unreal.MaterialExpressionSaturate, -800, y)
    connect(sharpen, "", saturate, "")
    return saturate


def make_band_mask(material, altitude_expr, line_meters, band_meters, invert, y):
    """0..1 mask for altitude above (or below, if inverted) a line."""
    offset = expr(material, unreal.MaterialExpressionSubtract, -1000, y)
    if invert:
        offset.set_editor_property("const_a", line_meters)
        connect(altitude_expr, "", offset, "B")
    else:
        offset.set_editor_property("const_b", line_meters)
        connect(altitude_expr, "", offset, "A")

    scale = expr(material, unreal.MaterialExpressionMultiply, -800, y)
    scale.set_editor_property("const_b", 1.0 / band_meters)
    connect(offset, "", scale, "A")

    saturate = expr(material, unreal.MaterialExpressionSaturate, -600, y)
    connect(scale, "", saturate, "")
    return saturate


def color_constant(material, rgb, x, y):
    constant = expr(material, unreal.MaterialExpressionConstant3Vector, x, y)
    constant.set_editor_property(
        "constant", unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0)
    )
    return constant


def import_layer_textures():
    """Imports the D/N/R maps every layer needs; returns loaded textures keyed by (layer, suffix)."""
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    textures = {}
    for layer, group, asset_name, _ in LAYERS:
        for suffix in MAP_SUFFIXES:
            asset_path = f"{TEXTURE_ASSET_ROOT}/{group}/{asset_name}_{suffix}"
            if not unreal.EditorAssetLibrary.does_asset_exist(asset_path):
                source = os.path.join(TEXTURE_SOURCE_ROOT, group, f"{asset_name}_{suffix}.jpg")
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
            textures[(layer, suffix)] = texture
    return textures


def configure_texture(texture, suffix):
    """Normal and roughness maps are data, not color: linear, and normals compressed as such."""
    if suffix == "N":
        texture.set_editor_property("srgb", False)
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
    if suffix == "R":
        texture.set_editor_property("srgb", False)
    unreal.EditorAssetLibrary.save_loaded_asset(texture)


def make_layer_uvs(material, tile_meters, y):
    """World-plan UVs: XY position over the tiling size, so tiles of any LOD agree."""
    world_position = expr(material, unreal.MaterialExpressionWorldPosition, -2200, y)
    mask_xy = expr(material, unreal.MaterialExpressionComponentMask, -2000, y)
    mask_xy.set_editor_property("r", True)
    mask_xy.set_editor_property("g", True)
    mask_xy.set_editor_property("b", False)
    mask_xy.set_editor_property("a", False)
    connect(world_position, "", mask_xy, "")

    to_uv = expr(material, unreal.MaterialExpressionMultiply, -1800, y)
    to_uv.set_editor_property("const_b", 0.01 / tile_meters)
    connect(mask_xy, "", to_uv, "A")
    return to_uv


def make_texture_sample(material, texture, sampler_type, uv_expr, x, y):
    sample = expr(material, unreal.MaterialExpressionTextureSample, x, y)
    sample.set_editor_property("texture", texture)
    sample.set_editor_property("sampler_type", sampler_type)
    connect(uv_expr, "", sample, "UVs")
    return sample


def make_layer_blend(material, layer_outputs, ground_mask, snow_mask, x, y, output_index=""):
    """rock -> ground -> snow, each stage keyed by its mask."""
    ground_blend = expr(material, unreal.MaterialExpressionLinearInterpolate, x, y)
    connect(layer_outputs["rock"], output_index, ground_blend, "A")
    connect(layer_outputs["ground"], output_index, ground_blend, "B")
    connect(ground_mask, "", ground_blend, "Alpha")

    snow_blend = expr(material, unreal.MaterialExpressionLinearInterpolate, x + 200, y)
    connect(ground_blend, "", snow_blend, "A")
    connect(layer_outputs["snow"], output_index, snow_blend, "B")
    connect(snow_mask, "", snow_blend, "Alpha")
    return snow_blend


def make_world_space_normal(material, blended_normal, x, y):
    """
    The tile meshes carry no UVs, so there is no tangent basis to decode a
    normal map in. With world-plan UVs the tangent frame is the world axes:
    tilt the vertex normal by the map's XY and renormalise, and output in
    world space (the material has tangent-space normals switched off).
    """
    mask_xy = expr(material, unreal.MaterialExpressionComponentMask, x, y)
    mask_xy.set_editor_property("r", True)
    mask_xy.set_editor_property("g", True)
    mask_xy.set_editor_property("b", False)
    mask_xy.set_editor_property("a", False)
    connect(blended_normal, "", mask_xy, "")

    strength = expr(material, unreal.MaterialExpressionMultiply, x + 150, y)
    strength.set_editor_property("const_b", 0.6)
    connect(mask_xy, "", strength, "A")

    flat_z = expr(material, unreal.MaterialExpressionConstant, x + 150, y + 100)
    flat_z.set_editor_property("r", 0.0)
    tilt = expr(material, unreal.MaterialExpressionAppendVector, x + 300, y)
    connect(strength, "", tilt, "A")
    connect(flat_z, "", tilt, "B")

    vertex_normal = expr(material, unreal.MaterialExpressionVertexNormalWS, x + 300, y + 100)
    tilted = expr(material, unreal.MaterialExpressionAdd, x + 450, y)
    connect(vertex_normal, "", tilted, "A")
    connect(tilt, "", tilted, "B")

    normalised = expr(material, unreal.MaterialExpressionNormalize, x + 600, y)
    connect(tilted, "", normalised, "")
    return normalised


def build_flat_color_graph(material, ground_mask, snow_mask):
    rock = color_constant(material, ROCK_COLOR, -400, -700)
    snow = color_constant(material, SNOW_COLOR, -400, -600)
    grass = color_constant(material, GRASS_COLOR, -400, 200)

    final_color = make_layer_blend(
        material, {"rock": rock, "ground": grass, "snow": snow}, ground_mask, snow_mask, -200, -350
    )

    roughness = expr(material, unreal.MaterialExpressionLinearInterpolate, -100, 150)
    roughness.set_editor_property("const_a", 0.9)
    roughness.set_editor_property("const_b", 0.55)
    connect(snow_mask, "", roughness, "Alpha")

    unreal.MaterialEditingLibrary.connect_material_property(
        final_color, "", unreal.MaterialProperty.MP_BASE_COLOR
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        roughness, "", unreal.MaterialProperty.MP_ROUGHNESS
    )


def build_textured_graph(material, textures, ground_mask, snow_mask):
    color_samples = {}
    normal_samples = {}
    rough_samples = {}
    for index, (layer, _, _, tile_meters) in enumerate(LAYERS):
        row = -700 + index * 500
        uvs = make_layer_uvs(material, tile_meters, row)
        color_samples[layer] = make_texture_sample(
            material, textures[(layer, "D")], unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, uvs, -1600, row
        )
        normal_samples[layer] = make_texture_sample(
            material, textures[(layer, "N")], unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, uvs, -1600, row + 150
        )
        rough_samples[layer] = make_texture_sample(
            material, textures[(layer, "R")], unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, uvs, -1600, row + 300
        )

    final_color = make_layer_blend(material, color_samples, ground_mask, snow_mask, -200, -400)
    roughness = make_layer_blend(material, rough_samples, ground_mask, snow_mask, -200, 200, "R")
    blended_normal = make_layer_blend(material, normal_samples, ground_mask, snow_mask, -200, 500)
    world_normal = make_world_space_normal(material, blended_normal, 100, 500)

    material.set_editor_property("tangent_space_normal", False)
    unreal.MaterialEditingLibrary.connect_material_property(
        final_color, "", unreal.MaterialProperty.MP_BASE_COLOR
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        roughness, "", unreal.MaterialProperty.MP_ROUGHNESS
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        world_normal, "", unreal.MaterialProperty.MP_NORMAL
    )


def create_material():
    if unreal.EditorAssetLibrary.does_asset_exist(MATERIAL_PATH):
        unreal.EditorAssetLibrary.delete_asset(MATERIAL_PATH)
        unreal.log("Deleted existing material for recreation: " + MATERIAL_PATH)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = asset_tools.create_asset(
        "M_Terrain", "/Game/Materials", unreal.Material, unreal.MaterialFactoryNew()
    )
    material.set_editor_property("two_sided", True)

    altitude = make_altitude_meters(material)
    flatness = make_flatness(material)
    # Snow clings to steeper faces than grass can grow on.
    snow_flatness = make_flatness(material, threshold_z=0.45, sharpen_factor=2.5, y=250)

    snow_altitude = make_band_mask(
        material, altitude, SNOW_LINE_METERS, SNOW_BAND_METERS, False, -500
    )
    ground_altitude = make_band_mask(
        material, altitude, GRASS_LINE_METERS, GRASS_BAND_METERS, True, -100
    )

    snow_mask = expr(material, unreal.MaterialExpressionMultiply, -400, -400)
    connect(snow_altitude, "", snow_mask, "A")
    connect(snow_flatness, "", snow_mask, "B")

    ground_mask = expr(material, unreal.MaterialExpressionMultiply, -400, 0)
    connect(ground_altitude, "", ground_mask, "A")
    connect(flatness, "", ground_mask, "B")

    textures = import_layer_textures()
    if textures is None:
        unreal.log_warning("Poly Haven textures missing, building flat-color terrain material")
        build_flat_color_graph(material, ground_mask, snow_mask)
    else:
        build_textured_graph(material, textures, ground_mask, snow_mask)

    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(MATERIAL_PATH)
    unreal.log("Created material " + MATERIAL_PATH)


create_level()
create_material()
