"""Creates the base content assets: empty L_Massif level and M_Terrain material.

M_Terrain draws the two ground layers the classification chose for each point
(MassifSurfaceMap.h), out of the texture arrays Tools/create_terrain_arrays.py
builds. The sampling itself — triplanar projection and the bend that breaks the
repeat — lives in Shaders/MassifTerrainSurface.ush; this wires a custom node to
it and hangs the debug channel overlay off the result.

Run Tools/fetch_polyhaven_textures.py and Tools/create_terrain_arrays.py first.

Idempotent: skips assets that already exist, always recreates the material.
Run headless with:
UnrealEditor-Cmd Massif.uproject -run=pythonscript \
    -script=Tools/create_base_content.py -unattended -nosplash
"""

import unreal

MAP_PATH = "/Game/Maps/L_Massif"
MATERIAL_PATH = "/Game/Materials/M_Terrain"

SURFACE_INCLUDE = "/Project/MassifTerrainSurface.ush"

GROUND_ARRAYS = {
    "ColorArray": "/Game/Textures/Terrain/TA_GroundColor",
    "NormalArray": "/Game/Textures/Terrain/TA_GroundNormal",
    "ArmArray": "/Game/Textures/Terrain/TA_GroundArm",
    "HeightArray": "/Game/Textures/Terrain/TA_GroundHeight",
}

WHITE_TEXTURE = "/Engine/EngineResources/WhiteSquareTexture"

# How wide, in classification weight, the band the two layers swap over is.
# Outside it one of them takes the pixel outright, so this is what decides how
# much ground the boundary is spread over; the height maps only shape the edge
# inside it.
LAYER_BLEND_CONTRAST = 0.35

# How far the layer maps are read from where the pixel actually is, and over what
# distance that displacement turns over. The classification is stored on the 8 m
# bake grid and its indices cannot be filtered, so without this every boundary
# between two layers is a straight cell edge. An amplitude of a few cells bends
# those edges into contours of the noise instead.
SURFACE_WARP_METERS = 14.0
SURFACE_WARP_WAVELENGTH_METERS = 45.0

# How much of a debug channel's colour is emitted rather than lit, and the floor
# the fixed-direction shading term keeps under it. The channel has to read as the
# colour the legend names, which it cannot do when a dim blue sky is multiplying
# it, but a flat unlit sheet loses the relief the channel is drawn over.
DEBUG_CHANNEL_SHADING_FLOOR = 0.55
DEBUG_SUN_DIRECTION = (0.45, -0.35, 0.82)


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


def connect_property(pin, material_property):
    """Same reasoning as connect(): a rejected material property silently drops the whole input."""
    source, output_name = pin
    connected = unreal.MaterialEditingLibrary.connect_material_property(
        source, output_name, material_property
    )
    if not connected:
        raise RuntimeError(f"Could not connect {type(source).__name__}.'{output_name}' to {material_property}")


def connect_pin(pin, to_expr, to_input):
    """Pins are (expression, output name) pairs: a custom node has several named outputs."""
    source, output_name = pin
    connect(source, output_name, to_expr, to_input)


def make_mask(material, source, channels, x, y):
    """Component mask as a node: channels is (r, g, b, a)."""
    mask = expr(material, unreal.MaterialExpressionComponentMask, x, y)
    for name, enabled in zip(("r", "g", "b", "a"), channels):
        mask.set_editor_property(name, enabled)
    connect(source, "", mask, "")
    return mask


def scalar_parameter(material, name, default, x, y):
    parameter = expr(material, unreal.MaterialExpressionScalarParameter, x, y)
    parameter.set_editor_property("parameter_name", name)
    parameter.set_editor_property("default_value", default)
    return parameter


def texture_object_parameter(material, name, x, y):
    """A world-wide lookup map. The default keeps the material compiling before one is set."""
    parameter = expr(material, unreal.MaterialExpressionTextureObjectParameter, x, y)
    parameter.set_editor_property("parameter_name", name)
    parameter.set_editor_property("texture", unreal.load_asset(WHITE_TEXTURE))
    parameter.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    return parameter


def texture_object(material, asset_path, x, y):
    """A fixed content asset rather than a parameter: the arrays never change at runtime."""
    asset = unreal.load_asset(asset_path)
    if asset is None:
        raise RuntimeError(
            f"Texture array not found: {asset_path}. Run Tools/create_terrain_arrays.py first.")

    node = expr(material, unreal.MaterialExpressionTextureObject, x, y)
    node.set_editor_property("texture", asset)
    return node


def make_world_position_meters(material, x, y):
    world_position = expr(material, unreal.MaterialExpressionWorldPosition, x, y)
    to_meters = expr(material, unreal.MaterialExpressionMultiply, x + 200, y)
    connect(world_position, "", to_meters, "A")
    to_meters.set_editor_property("const_b", 0.01)
    return to_meters


def make_world_uv(material, position_meters, x, y):
    """
    World metres onto the 0-1 range of the world-wide maps, as one multiply-add
    against a parameter carrying both halves of the transform: scale in red and
    green, offset in blue and alpha.

    The offset travels rather than being a constant half in the graph. The maps
    cover exactly the bake's extent, and only the code that built them knows what
    that is; a half hard-coded here is a second, silent claim about where the
    world is centred, and when the two disagree the map lands somewhere other
    than over the ground it describes.
    """
    # Two parameters rather than one carrying both halves: a vector parameter's
    # default output is three components, so there is no alpha to mask an offset
    # out of.
    scale_parameter = expr(material, unreal.MaterialExpressionVectorParameter, x, y + 150)
    scale_parameter.set_editor_property("parameter_name", "WorldToUvScale")
    scale_parameter.set_editor_property("default_value", unreal.LinearColor(1.0, -1.0, 0.0, 0.0))

    offset_parameter = expr(material, unreal.MaterialExpressionVectorParameter, x, y + 260)
    offset_parameter.set_editor_property("parameter_name", "WorldToUvOffset")
    offset_parameter.set_editor_property("default_value", unreal.LinearColor(0.5, 0.5, 0.0, 0.0))

    xy_meters = make_mask(material, position_meters, (True, True, False, False), x, y)
    scale = make_mask(material, scale_parameter, (True, True, False, False), x + 200, y + 150)
    offset = make_mask(material, offset_parameter, (True, True, False, False), x + 200, y + 260)

    scaled = expr(material, unreal.MaterialExpressionMultiply, x + 400, y)
    connect(xy_meters, "", scaled, "A")
    connect(scale, "", scaled, "B")

    placed = expr(material, unreal.MaterialExpressionAdd, x + 600, y)
    connect(scaled, "", placed, "A")
    connect(offset, "", placed, "B")

    # The scale on its own as well: it is what turns a displacement in metres into
    # one in map UV, and the shader has no other way to learn the map's extent.
    return placed, scale


SURFACE_CODE = """
FMassifSurfaceSample Ground = MassifSampleGround(
	IndexMap,
	BlendMap, BlendMapSampler,
	LayerParams,
	ColorArray, ColorArraySampler,
	NormalArray, NormalArraySampler,
	ArmArray, ArmArraySampler,
	HeightArray, HeightArraySampler,
	MapUv, MapUvPerMeter, PositionMeters, VertexNormal,
	BlendContrast, WarpMeters, WarpWavelengthMeters, SnowSlice);

Normal = Ground.NormalWorld;
Roughness = Ground.Roughness;
Occlusion = Ground.Occlusion;
return Ground.BaseColor;
"""

SURFACE_INPUT_NAMES = (
    "IndexMap",
    "BlendMap",
    "LayerParams",
    "ColorArray",
    "NormalArray",
    "ArmArray",
    "HeightArray",
    "MapUv",
    "MapUvPerMeter",
    "PositionMeters",
    "VertexNormal",
    "BlendContrast",
    "WarpMeters",
    "WarpWavelengthMeters",
    "SnowSlice",
)


def make_surface_node(material, x, y):
    """The custom node that calls into MassifTerrainSurface.ush."""
    node = expr(material, unreal.MaterialExpressionCustom, x, y)
    node.set_editor_property("description", "MassifSampleGround")
    node.set_editor_property("code", SURFACE_CODE)
    node.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    node.set_editor_property("include_file_paths", [SURFACE_INCLUDE])

    inputs = []
    for name in SURFACE_INPUT_NAMES:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        inputs.append(custom_input)
    node.set_editor_property("inputs", inputs)

    outputs = []
    for name, output_type in (
        ("Normal", unreal.CustomMaterialOutputType.CMOT_FLOAT3),
        ("Roughness", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
        ("Occlusion", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
    ):
        custom_output = unreal.CustomOutput()
        custom_output.set_editor_property("output_name", name)
        custom_output.set_editor_property("output_type", output_type)
        outputs.append(custom_output)
    node.set_editor_property("additional_outputs", outputs)
    return node


def build_surface_graph(material):
    """Wires everything the ground sampling needs into the custom node."""
    node = make_surface_node(material, 0, 0)

    position_meters = make_world_position_meters(material, -1600, -400)
    connect(position_meters, "", node, "PositionMeters")

    map_uv, map_uv_per_meter = make_world_uv(material, position_meters, -1200, -400)
    connect(map_uv, "", node, "MapUv")
    connect(map_uv_per_meter, "", node, "MapUvPerMeter")

    connect(texture_object_parameter(material, "SurfaceIndexMap", -600, 200), "", node, "IndexMap")
    connect(texture_object_parameter(material, "SurfaceBlendMap", -600, 320), "", node, "BlendMap")
    connect(texture_object_parameter(material, "SurfaceLayerParameters", -600, 440), "", node, "LayerParams")

    for index, (input_name, asset_path) in enumerate(GROUND_ARRAYS.items()):
        connect(texture_object(material, asset_path, -600, 600 + index * 120), "", node, input_name)

    vertex_normal = expr(material, unreal.MaterialExpressionVertexNormalWS, -600, 1200)
    connect(vertex_normal, "", node, "VertexNormal")

    connect(scalar_parameter(material, "LayerBlendContrast", LAYER_BLEND_CONTRAST, -600, 1400), "", node, "BlendContrast")
    connect(
        scalar_parameter(material, "SurfaceWarpMeters", SURFACE_WARP_METERS, -600, 1500),
        "", node, "WarpMeters")
    connect(
        scalar_parameter(
            material, "SurfaceWarpWavelengthMeters", SURFACE_WARP_WAVELENGTH_METERS, -600, 1600),
        "", node, "WarpWavelengthMeters")
    connect(scalar_parameter(material, "SurfaceLayerCount", 1.0, -600, 1700), "", node, "SnowSlice")
    return node, vertex_normal


def make_debug_shading(material, vertex_normal, x, y):
    """
    A fixed-direction shading term, so a debug channel keeps its relief without
    being tinted by whatever the sky happens to be doing. The scene's own light
    is what made every channel read as the same blue wash.
    """
    sun = expr(material, unreal.MaterialExpressionConstant3Vector, x, y)
    sun.set_editor_property("constant", unreal.LinearColor(*DEBUG_SUN_DIRECTION, 1.0))

    facing = expr(material, unreal.MaterialExpressionDotProduct, x + 200, y)
    connect(vertex_normal, "", facing, "A")
    connect(sun, "", facing, "B")

    lit = expr(material, unreal.MaterialExpressionSaturate, x + 400, y)
    connect(facing, "", lit, "")

    shaded = expr(material, unreal.MaterialExpressionLinearInterpolate, x + 600, y)
    shaded.set_editor_property("const_a", DEBUG_CHANNEL_SHADING_FLOOR)
    shaded.set_editor_property("const_b", 1.0)
    connect(lit, "", shaded, "Alpha")
    return shaded


def make_debug_overlay(material, vertex_normal, map_uv, color_pin, roughness_pin):
    """
    Paints a baked channel over the terrain instead of its material
    (ARCHITECTURE 5.7). Blend is a parameter left at zero, so the shipped
    material is the terrain material until something turns a view on.

    The channel is emitted rather than lit. A channel read through the scene's
    lighting is not the colour its legend names, and on a dim or coloured light
    every channel converges on the same wash. Base colour goes to black as the
    overlay comes up, so the lit path stops contributing at all, and a
    fixed-direction shading term keeps the relief legible.
    """
    sample = expr(material, unreal.MaterialExpressionTextureSampleParameter2D, -100, 1900)
    sample.set_editor_property("parameter_name", "DebugChannel")
    sample.set_editor_property("texture", unreal.load_asset(WHITE_TEXTURE))
    sample.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    connect(map_uv, "", sample, "UVs")

    blend = scalar_parameter(material, "DebugChannelBlend", 0.0, -100, 2150)

    unlit_color = expr(material, unreal.MaterialExpressionLinearInterpolate, 350, 1900)
    connect_pin(color_pin, unlit_color, "A")
    unlit_color.set_editor_property("const_b", 0.0)
    connect(blend, "", unlit_color, "Alpha")

    flat_roughness = expr(material, unreal.MaterialExpressionLinearInterpolate, 350, 2150)
    connect_pin(roughness_pin, flat_roughness, "A")
    flat_roughness.set_editor_property("const_b", 1.0)
    connect(blend, "", flat_roughness, "Alpha")

    shaded = expr(material, unreal.MaterialExpressionMultiply, 350, 2400)
    connect(sample, "RGB", shaded, "A")
    connect(make_debug_shading(material, vertex_normal, -900, 2400), "", shaded, "B")

    emissive = expr(material, unreal.MaterialExpressionMultiply, 550, 2400)
    connect(shaded, "", emissive, "A")
    connect(blend, "", emissive, "B")
    return (unlit_color, ""), (flat_roughness, ""), (emissive, "")


def create_material():
    if unreal.EditorAssetLibrary.does_asset_exist(MATERIAL_PATH):
        unreal.EditorAssetLibrary.delete_asset(MATERIAL_PATH)
        unreal.log("Deleted existing material for recreation: " + MATERIAL_PATH)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = asset_tools.create_asset(
        "M_Terrain", "/Game/Materials", unreal.Material, unreal.MaterialFactoryNew()
    )
    material.set_editor_property("two_sided", True)
    # The custom node returns a world-space normal: it builds one out of the
    # world axes it projected on, and there is no tangent basis on the tile
    # meshes to express it against anyway.
    material.set_editor_property("tangent_space_normal", False)

    surface, vertex_normal = build_surface_graph(material)
    map_uv, _ = make_world_uv(material, make_world_position_meters(material, -1600, 1700), -1200, 1700)

    color_pin, roughness_pin, emissive_pin = make_debug_overlay(
        material, vertex_normal, map_uv, (surface, ""), (surface, "Roughness")
    )
    connect_property(color_pin, unreal.MaterialProperty.MP_BASE_COLOR)
    connect_property(roughness_pin, unreal.MaterialProperty.MP_ROUGHNESS)
    connect_property(emissive_pin, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    connect_property((surface, "Normal"), unreal.MaterialProperty.MP_NORMAL)
    connect_property((surface, "Occlusion"), unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)

    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(MATERIAL_PATH)
    unreal.log("Created material " + MATERIAL_PATH)


create_level()
create_material()
