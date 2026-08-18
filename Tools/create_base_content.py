"""Creates the base content assets: empty L_Massif level and M_Terrain material.

Idempotent: skips assets that already exist. Run headless with:
UnrealEditor-Cmd Massif.uproject -run=pythonscript \
    -script=Tools/create_base_content.py -unattended -nosplash
"""

import unreal

MAP_PATH = "/Game/Maps/L_Massif"
MATERIAL_PATH = "/Game/Materials/M_Terrain"

ROCK_COLOR = (0.145, 0.135, 0.125)
SNOW_COLOR = (0.905, 0.925, 0.965)
GRASS_COLOR = (0.075, 0.145, 0.055)

SNOW_LINE_METERS = 2100.0
SNOW_BAND_METERS = 180.0
GRASS_LINE_METERS = 1750.0
GRASS_BAND_METERS = 250.0


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


def make_flatness(material):
    """1.0 on flat ground, 0.0 on steep faces."""
    vertex_normal = expr(material, unreal.MaterialExpressionVertexNormalWS, -1600, 100)
    mask_z = expr(material, unreal.MaterialExpressionComponentMask, -1400, 100)
    mask_z.set_editor_property("r", False)
    mask_z.set_editor_property("g", False)
    mask_z.set_editor_property("b", True)
    mask_z.set_editor_property("a", False)
    connect(vertex_normal, "", mask_z, "")

    threshold = expr(material, unreal.MaterialExpressionSubtract, -1200, 100)
    threshold.set_editor_property("const_b", 0.70)
    connect(mask_z, "", threshold, "A")

    sharpen = expr(material, unreal.MaterialExpressionMultiply, -1000, 100)
    sharpen.set_editor_property("const_b", 5.0)
    connect(threshold, "", sharpen, "A")

    saturate = expr(material, unreal.MaterialExpressionSaturate, -800, 100)
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


def create_material():
    if unreal.EditorAssetLibrary.does_asset_exist(MATERIAL_PATH):
        unreal.log("Material already exists: " + MATERIAL_PATH)
        return
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = asset_tools.create_asset(
        "M_Terrain", "/Game/Materials", unreal.Material, unreal.MaterialFactoryNew()
    )
    material.set_editor_property("two_sided", True)

    altitude = make_altitude_meters(material)
    flatness = make_flatness(material)

    snow_altitude = make_band_mask(
        material, altitude, SNOW_LINE_METERS, SNOW_BAND_METERS, False, -500
    )
    grass_altitude = make_band_mask(
        material, altitude, GRASS_LINE_METERS, GRASS_BAND_METERS, True, -100
    )

    snow_mask = expr(material, unreal.MaterialExpressionMultiply, -400, -400)
    connect(snow_altitude, "", snow_mask, "A")
    connect(flatness, "", snow_mask, "B")

    grass_mask = expr(material, unreal.MaterialExpressionMultiply, -400, 0)
    connect(grass_altitude, "", grass_mask, "A")
    connect(flatness, "", grass_mask, "B")

    rock = color_constant(material, ROCK_COLOR, -400, -700)
    snow = color_constant(material, SNOW_COLOR, -400, -600)
    grass = color_constant(material, GRASS_COLOR, -400, 200)

    rock_grass = expr(material, unreal.MaterialExpressionLinearInterpolate, -200, -150)
    connect(rock, "", rock_grass, "A")
    connect(grass, "", rock_grass, "B")
    connect(grass_mask, "", rock_grass, "Alpha")

    final_color = expr(material, unreal.MaterialExpressionLinearInterpolate, -100, -350)
    connect(rock_grass, "", final_color, "A")
    connect(snow, "", final_color, "B")
    connect(snow_mask, "", final_color, "Alpha")

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
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(MATERIAL_PATH)
    unreal.log("Created material " + MATERIAL_PATH)


create_level()
create_material()
