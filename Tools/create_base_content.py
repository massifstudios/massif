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

# The world spans 500-3400 m, so a 1500 m snowline buries the whole massif in
# one layer. These put grass on the valleys and lower flanks, rock on the
# steeps and the high ground, snow on the summits.
SNOW_LINE_METERS = 2600.0
SNOW_BAND_METERS = 400.0
GRASS_LINE_METERS = 2000.0
GRASS_BAND_METERS = 350.0

# Surface normal Z at which a layer gives way to rock, and how sharply. Grass
# stops holding around 30 degrees; snow sits on much steeper ground than that.
GRASS_FLATNESS_THRESHOLD = 0.80
GRASS_FLATNESS_SHARPEN = 5.0
SNOW_FLATNESS_THRESHOLD = 0.55
SNOW_FLATNESS_SHARPEN = 3.0

TEXTURE_SOURCE_ROOT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Content", "Textures", "PolyHaven"
)
TEXTURE_ASSET_ROOT = "/Game/Textures/PolyHaven"

# Two texture sets per surface layer, blended by a macro noise so no single
# tiling pattern covers a whole mountainside: (group folder, asset name,
# tiling meters). Deliberately different tiling per variant, so even where
# the blend lingers the repeats never line up.
LAYERS = [
    ("rock", [("rock", "T_RockFace03", 13.0), ("rock", "T_GrayRocks", 21.0)]),
    ("ground", [("rock", "T_AerialGrassRock", 7.0), ("ground", "T_RockyTerrain02", 11.0)]),
    ("snow", [("snow", "T_SnowFieldAerial", 9.0), ("snow", "T_Snow01", 5.0)]),
]

MAP_SUFFIXES = ("D", "N", "R")

VARIANT_NOISE_WAVELENGTH_METERS = 340.0
TINT_NOISE_WAVELENGTH_METERS = 1700.0

# Altitude jitter fed into the snow and grass band masks. Without it both lines
# are clean contours around the massif, which reads as a painted-on gradient;
# with it they wander over spurs and gullies the way real ones do.
LINE_NOISE_WAVELENGTH_METERS = 420.0
LINE_NOISE_AMPLITUDE_METERS = 140.0

# Kilometre-scale tint the base color is multiplied by. Two colors rather than
# one brightness, so distant slopes vary in hue as well as value.
TINT_COOL = (0.78, 0.86, 0.98)
TINT_WARM = (1.16, 1.06, 0.88)

WORLD_ALIGNED_TEXTURE = "/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedTexture"
WORLD_ALIGNED_NORMAL = "/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedNormal"


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
    """Raises on a bad pin name: a silently dropped connection would surface only as a black material."""
    connected = unreal.MaterialEditingLibrary.connect_material_expressions(
        from_expr, from_output, to_expr, to_input
    )
    if not connected:
        raise RuntimeError(
            f"Could not connect {type(from_expr).__name__}.'{from_output}' "
            f"to {type(to_expr).__name__}.'{to_input}'"
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

    # Only the band masks read this, so jittering it here moves the snow and
    # grass lines off their clean horizontal contours in one place.
    jitter = make_macro_noise(
        material, LINE_NOISE_WAVELENGTH_METERS, -LINE_NOISE_AMPLITUDE_METERS, LINE_NOISE_AMPLITUDE_METERS, -450
    )
    jittered = expr(material, unreal.MaterialExpressionAdd, -1050, -300)
    connect(to_meters, "", jittered, "A")
    connect(jitter, "", jittered, "B")
    return jittered


def make_flatness(material, threshold_z, sharpen_factor, y=100):
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
    """Imports the D/N/R maps every layer variant needs; returns textures keyed by (layer, variant, suffix)."""
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    textures = {}
    for layer, variants in LAYERS:
        for variant, (group, asset_name, _) in enumerate(variants):
            for suffix in MAP_SUFFIXES:
                texture = import_one_texture(asset_tools, group, asset_name, suffix)
                if texture is None:
                    return None
                textures[(layer, variant, suffix)] = texture
    return textures


def import_one_texture(asset_tools, group, asset_name, suffix):
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
    return texture


def configure_texture(texture, suffix):
    """Normal and roughness maps are data, not color: linear, and normals compressed as such."""
    if suffix == "N":
        texture.set_editor_property("srgb", False)
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
    if suffix == "R":
        texture.set_editor_property("srgb", False)
    unreal.EditorAssetLibrary.save_loaded_asset(texture)


def make_world_aligned_sample(material, function_path, output_name, texture, sampler_type, tile_meters, x, y):
    """
    One texture projected on all three world axes and blended by the surface
    normal. The tile meshes carry no UVs, and a plan projection alone stretches
    every cliff face into vertical streaks; world-aligned sampling costs three
    samples and removes both problems. Leaving WorldPosition unconnected lets
    the function use its own world-position node.
    """
    texture_object = expr(material, unreal.MaterialExpressionTextureObject, x - 400, y)
    texture_object.set_editor_property("texture", texture)
    texture_object.set_editor_property("sampler_type", sampler_type)

    texture_size = expr(material, unreal.MaterialExpressionConstant3Vector, x - 400, y + 90)
    centimeters = tile_meters * 100.0
    texture_size.set_editor_property(
        "constant", unreal.LinearColor(centimeters, centimeters, centimeters, 1.0)
    )

    # unreal.load_asset, not EditorAssetLibrary: the engine's material function
    # folders are not all in the asset registry of a commandlet session.
    function = unreal.load_asset(function_path)
    if function is None:
        raise RuntimeError(f"Material function not found: {function_path}")

    call = expr(material, unreal.MaterialExpressionMaterialFunctionCall, x, y)
    call.set_material_function(function)
    connect(texture_object, "", call, "TextureObject")
    connect(texture_size, "", call, "TextureSize")
    return call, output_name


def connect_pin(pin, to_expr, to_input):
    """Pins are (expression, output name) pairs: material functions have several named outputs."""
    source, output_name = pin
    connect(source, output_name, to_expr, to_input)


def make_layer_blend(material, layer_pins, ground_mask, snow_mask, x, y):
    """rock -> ground -> snow, each stage keyed by its mask."""
    ground_blend = expr(material, unreal.MaterialExpressionLinearInterpolate, x, y)
    connect_pin(layer_pins["rock"], ground_blend, "A")
    connect_pin(layer_pins["ground"], ground_blend, "B")
    connect(ground_mask, "", ground_blend, "Alpha")

    snow_blend = expr(material, unreal.MaterialExpressionLinearInterpolate, x + 200, y)
    connect(ground_blend, "", snow_blend, "A")
    connect_pin(layer_pins["snow"], snow_blend, "B")
    connect(snow_mask, "", snow_blend, "Alpha")
    return snow_blend


def build_flat_color_graph(material, ground_mask, snow_mask):
    rock = color_constant(material, ROCK_COLOR, -400, -700)
    snow = color_constant(material, SNOW_COLOR, -400, -600)
    grass = color_constant(material, GRASS_COLOR, -400, 200)

    final_color = make_layer_blend(
        material,
        {"rock": (rock, ""), "ground": (grass, ""), "snow": (snow, "")},
        ground_mask, snow_mask, -200, -350,
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


def make_macro_noise(material, wavelength_meters, output_min, output_max, y):
    noise = expr(material, unreal.MaterialExpressionNoise, -2400, y)
    noise.set_editor_property("scale", 1.0 / (wavelength_meters * 100.0))
    noise.set_editor_property("quality", 2)
    noise.set_editor_property("levels", 2)
    noise.set_editor_property("output_min", output_min)
    noise.set_editor_property("output_max", output_max)
    noise.set_editor_property("turbulence", False)
    return noise


def make_color_variants(material, textures, layer, variants, variant_mask, y):
    """One layer's base color: both texture sets world-aligned at their own tiling, blended by the macro mask."""
    pins = []
    for variant, (_, _, tile_meters) in enumerate(variants):
        pins.append(make_world_aligned_sample(
            material, WORLD_ALIGNED_TEXTURE, "XYZ Texture",
            textures[(layer, variant, "D")], unreal.MaterialSamplerType.SAMPLERTYPE_COLOR,
            tile_meters, -1600, y + variant * 200,
        ))
    blend = expr(material, unreal.MaterialExpressionLinearInterpolate, -1250, y)
    connect_pin(pins[0], blend, "A")
    connect_pin(pins[1], blend, "B")
    connect(variant_mask, "", blend, "Alpha")
    return blend, ""


def make_macro_tint(material, tint_noise, y):
    """Kilometre-scale hue and value drift, so distant slopes stop reading as one sheet."""
    cool = color_constant(material, TINT_COOL, -1000, y)
    warm = color_constant(material, TINT_WARM, -1000, y + 100)
    tint = expr(material, unreal.MaterialExpressionLinearInterpolate, -800, y)
    connect(cool, "", tint, "A")
    connect(warm, "", tint, "B")
    connect(tint_noise, "", tint, "Alpha")
    return tint


def build_textured_graph(material, textures, ground_mask, snow_mask):
    variant_mask = make_macro_noise(material, VARIANT_NOISE_WAVELENGTH_METERS, 0.0, 1.0, -1100)

    color_pins = {}
    normal_pins = {}
    rough_pins = {}
    for index, (layer, variants) in enumerate(LAYERS):
        row = -1400 + index * 900
        base_tile_meters = variants[0][2]
        color_pins[layer] = make_color_variants(material, textures, layer, variants, variant_mask, row)
        # Normal and roughness take the first variant only: a second set of
        # them costs six more samples for detail the base color already carries.
        normal_pins[layer] = make_world_aligned_sample(
            material, WORLD_ALIGNED_NORMAL, "XYZ Texture",
            textures[(layer, 0, "N")], unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL,
            base_tile_meters, -1600, row + 420,
        )
        rough_pins[layer] = make_world_aligned_sample(
            material, WORLD_ALIGNED_TEXTURE, "XYZ Texture",
            textures[(layer, 0, "R")], unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR,
            base_tile_meters, -1600, row + 620,
        )

    blended_color = make_layer_blend(material, color_pins, ground_mask, snow_mask, -200, -400)
    roughness = make_layer_blend(material, rough_pins, ground_mask, snow_mask, -200, 200)
    world_normal = make_layer_blend(material, normal_pins, ground_mask, snow_mask, -200, 500)

    tint_noise = make_macro_noise(material, TINT_NOISE_WAVELENGTH_METERS, 0.0, 1.0, -1250)
    final_color = expr(material, unreal.MaterialExpressionMultiply, 150, -400)
    connect(blended_color, "", final_color, "A")
    connect(make_macro_tint(material, tint_noise, -250), "", final_color, "B")

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
    flatness = make_flatness(material, GRASS_FLATNESS_THRESHOLD, GRASS_FLATNESS_SHARPEN)
    snow_flatness = make_flatness(material, SNOW_FLATNESS_THRESHOLD, SNOW_FLATNESS_SHARPEN, y=250)

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
