"""Creates M_River, the water surface the river ribbons are drawn with.

The ribbon meshes (MassifRiverRibbonMesh.h) carry everything the material needs:
UV0 runs across and along the channel, UV1 is the flow direction, and the vertex
colour holds speed, depth and width as fractions of fixed references. So there
is one material for every river in the world and no per-reach parameters.

The shading model is Single Layer Water, which is what makes it read as water
rather than as a tinted sheet: it reflects the sky off the surface, refracts what
is behind it, and integrates absorption and scattering through the column down to
whatever the scene depth says is under it. So no colour is painted on — only the
surface normal that reflection is taken along, the foam, and the coefficients the
column is integrated with.

The ripple layers and the foam live in Shaders/MassifRiverSurface.ush; this wires
a custom node to it.

Idempotent: always recreates the material.
Run headless with:
UnrealEditor-Cmd Massif.uproject -run=pythonscript \
    -script=Tools/create_river_content.py -unattended -nosplash
"""

import unreal

MATERIAL_PATH = "/Game/Materials/M_River"
RIVER_INCLUDE = "/Project/MassifRiverSurface.ush"

# A tiling normal map, used as a slope field rather than as a surface: two
# layers of it at different sizes and drift rates are the ripples.
RIPPLE_TEXTURE = "/Engine/EngineMaterials/RandomNormal2"

# What the ribbon mesh's vertex colours are fractions of. These have to match
# MassifRiverRibbonMesh.h, which is what wrote them.
SPEED_REFERENCE_METERS_PER_SECOND = 4.0
WIDTH_REFERENCE_METERS = 30.0
COLUMN_SCALE_REFERENCE = 12.0

# How far the ripple layers tilt the surface. Water is close to flat and reads as
# water through its reflections, so a large number here looks like crumpled foil.
RIPPLE_STEEPNESS = 0.35

# Share of the half-width the channel fades out over. The ribbon ends square at
# the channel width and has no bank geometry, so without this every stream is
# drawn with a hard line down each side.
BANK_FADE_FRACTION = 0.35

# Speed, as a fraction of the reference, at which water starts to aerate, and the
# span it goes fully white over. An alpine stream is broken water most of the way
# down, which is most of what makes it read as fast.
FOAM_SPEED_THRESHOLD = 0.22
FOAM_SPEED_FADE = 0.5

# How the water column takes light apart, per metre of it. The Oetztaler Ache is
# glacier-fed, so it is milky rather than clear: scattering is high and close to
# even across the spectrum, which is what makes rock flour read as pale turquoise,
# while absorption kills the reds first and leaves the blue-green behind.
WATER_ABSORPTION_PER_METER = (0.55, 0.16, 0.10)
WATER_SCATTERING_PER_METER = (0.35, 0.55, 0.60)

# Forward bias of the scattering. Rock flour throws light onwards rather than
# back, which is why a glacial river glows where the sun is behind it.
WATER_PHASE_G = 0.3

# Water is IOR 1.33, which is a reflectance of about 2% head-on. UE's default 0.5
# means IOR 1.5, and the difference shows as water that looks like glass.
WATER_SPECULAR = 0.25

RIVER_INPUT_NAMES = (
    "ChannelUv",
    "FlowDirection",
    "ChannelState",
    "ColumnFraction",
    "SurfaceNormal",
    "RippleMap",
    "TimeSeconds",
    "RippleSteepness",
    "BankFadeFraction",
    "FoamThreshold",
    "FoamFade",
    "SpeedReference",
    "WidthReference",
    "ColumnScaleReference",
)

RIVER_CODE = """
float SpeedFraction = ChannelState.r;
float WidthMeters = max(ChannelState.b * WidthReference, 0.2f);

// The elevation model measures the top of a river, not its bottom, so the wedge
// the geometry could be given is a freeboard rather than the channel's depth.
// Scaling the coefficients up by the ratio between the two integrates the column
// the network solved rather than the couple of centimetres actually modelled.
ColumnScale = ColumnFraction * ColumnScaleReference;

// The channel's own frame, in metres: across the flow from bank to bank, and
// along it in the distance the ribbon has already run.
float2 ChannelMeters = float2((ChannelUv.x - 0.5f) * WidthMeters, ChannelUv.y);
float DriftMeters = TimeSeconds * SpeedFraction * SpeedReference;

// Three layers at unrelated sizes, rates and orientations. Two alone still tile
// together often enough to read as a regular corrugation running down the
// channel, and turning each off the others' lattice is what breaks that up.
FMassifRipple Coarse = MassifSampleRipple(
    RippleMap, RippleMapSampler, ChannelMeters, 1.7f, DriftMeters,
    float2(0.0f, 0.0f), MassifRippleOrientation(0.0f));
FMassifRipple Medium = MassifSampleRipple(
    RippleMap, RippleMapSampler, ChannelMeters, 0.62f, DriftMeters * 0.62f,
    float2(0.31f, 0.17f), MassifRippleOrientation(0.7f));
FMassifRipple Fine = MassifSampleRipple(
    RippleMap, RippleMapSampler, ChannelMeters, 0.21f, DriftMeters * 0.31f,
    float2(0.73f, 0.44f), MassifRippleOrientation(-1.3f));

float2 Slope = Coarse.Slope + 0.55f * Medium.Slope + 0.3f * Fine.Slope;
float RippleHeight = 0.5f * Coarse.Height + 0.3f * Medium.Height + 0.2f * Fine.Height;

float3 AlongFlow = float3(FlowDirection, 0.0f);
AlongFlow = normalize(AlongFlow - SurfaceNormal * dot(AlongFlow, SurfaceNormal));

// Ripples flatten towards the bank, where the water is thin enough that the bed
// under it is what shapes the surface.
float BankFade = MassifBankFade(ChannelUv.x, BankFadeFraction);
Normal = MassifRippleNormal(Slope, SurfaceNormal, AlongFlow, RippleSteepness * BankFade);

float Foam = max(
    MassifFoamAmount(SpeedFraction, RippleHeight, FoamThreshold, FoamFade),
    MassifBankFoam(ChannelUv.x, SpeedFraction));

// Foam is the only thing here with a colour of its own. Open water is left
// almost black, because what is seen of it is reflected, refracted or scattered
// out of the column rather than lit off its surface.
Normal = lerp(Normal, SurfaceNormal, Foam);
Roughness = lerp(0.02f, 0.60f, Foam);
return lerp(float3(0.01f, 0.02f, 0.02f), float3(0.82f, 0.86f, 0.88f), Foam);
"""


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


def connect_pin(pin, to_expr, to_input):
    """Pins are (expression, output name) pairs: a custom node has several named outputs."""
    source, output_name = pin
    connect(source, output_name, to_expr, to_input)


def connect_property(pin, material_property):
    source, output_name = pin
    connected = unreal.MaterialEditingLibrary.connect_material_property(
        source, output_name, material_property
    )
    if not connected:
        raise RuntimeError(f"Could not connect {type(source).__name__}.'{output_name}' to {material_property}")


def scalar_parameter(material, name, default, x, y):
    parameter = expr(material, unreal.MaterialExpressionScalarParameter, x, y)
    parameter.set_editor_property("parameter_name", name)
    parameter.set_editor_property("default_value", default)
    return parameter


def texture_coordinate(material, index, x, y):
    node = expr(material, unreal.MaterialExpressionTextureCoordinate, x, y)
    node.set_editor_property("coordinate_index", index)
    return node


def make_river_node(material, x, y):
    """The custom node that calls into MassifRiverSurface.ush."""
    node = expr(material, unreal.MaterialExpressionCustom, x, y)
    node.set_editor_property("description", "MassifRiverSurface")
    node.set_editor_property("code", RIVER_CODE)
    node.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    node.set_editor_property("include_file_paths", [RIVER_INCLUDE])

    inputs = []
    for name in RIVER_INPUT_NAMES:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        inputs.append(custom_input)
    node.set_editor_property("inputs", inputs)

    outputs = []
    for name, output_type in (
        ("Normal", unreal.CustomMaterialOutputType.CMOT_FLOAT3),
        ("Roughness", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
        ("ColumnScale", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
    ):
        custom_output = unreal.CustomOutput()
        custom_output.set_editor_property("output_name", name)
        custom_output.set_editor_property("output_type", output_type)
        outputs.append(custom_output)
    node.set_editor_property("additional_outputs", outputs)
    return node


def make_ripple_texture(material, x, y):
    asset = unreal.load_asset(RIPPLE_TEXTURE)
    if asset is None:
        raise RuntimeError("Ripple normal map not found: " + RIPPLE_TEXTURE)

    node = expr(material, unreal.MaterialExpressionTextureObject, x, y)
    node.set_editor_property("texture", asset)
    node.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    return node


def build_river_graph(material):
    node = make_river_node(material, 0, 0)

    connect(texture_coordinate(material, 0, -700, -400), "", node, "ChannelUv")
    connect(texture_coordinate(material, 1, -700, -280), "", node, "FlowDirection")

    # The default pin is RGB only, so alpha has to come across as its own input.
    vertex_color = expr(material, unreal.MaterialExpressionVertexColor, -700, -160)
    connect(vertex_color, "", node, "ChannelState")
    connect(vertex_color, "A", node, "ColumnFraction")

    vertex_normal = expr(material, unreal.MaterialExpressionVertexNormalWS, -700, -40)
    connect(vertex_normal, "", node, "SurfaceNormal")

    connect(make_ripple_texture(material, -700, 80), "", node, "RippleMap")
    connect(expr(material, unreal.MaterialExpressionTime, -700, 220), "", node, "TimeSeconds")

    for index, (name, default) in enumerate((
        ("RippleSteepness", RIPPLE_STEEPNESS),
        ("BankFadeFraction", BANK_FADE_FRACTION),
        ("FoamThreshold", FOAM_SPEED_THRESHOLD),
        ("FoamFade", FOAM_SPEED_FADE),
        ("SpeedReference", SPEED_REFERENCE_METERS_PER_SECOND),
        ("WidthReference", WIDTH_REFERENCE_METERS),
        ("ColumnScaleReference", COLUMN_SCALE_REFERENCE),
    )):
        connect(scalar_parameter(material, name, default, -700, 340 + index * 110), "", node, name)
    return node


def vector_parameter(material, name, value, x, y):
    parameter = expr(material, unreal.MaterialExpressionVectorParameter, x, y)
    parameter.set_editor_property("parameter_name", name)
    parameter.set_editor_property(
        "default_value", unreal.LinearColor(value[0], value[1], value[2], 1.0))
    return parameter


def scale_coefficients(material, name, value, scale_pin, x, y):
    """One set of per-metre coefficients, stretched to the column the network solved."""
    scaled = expr(material, unreal.MaterialExpressionMultiply, x + 250, y)
    connect(vector_parameter(material, name, value, x, y), "", scaled, "A")
    connect_pin(scale_pin, scaled, "B")
    return scaled


def make_water_column(material, scale_pin, x, y):
    """
    What the shading model integrates through the water between the surface and
    whatever the scene depth says is behind it. This is where the colour comes
    from: absorption takes the reds out over distance and scattering puts the rock
    flour's own pale turquoise back in.

    Both are per metre, and the metres available are only the freeboard the wedge
    was built with, because a laser scan measures the top of a river rather than
    its bed. So the coefficients are scaled by how much deeper the channel really
    is, which the ribbon put in vertex alpha.
    """
    node = expr(material, unreal.MaterialExpressionSingleLayerWaterMaterialOutput, x, y)
    connect(
        scale_coefficients(material, "WaterAbsorption", WATER_ABSORPTION_PER_METER, scale_pin, x - 700, y),
        "", node, "AbsorptionCoefficients")
    connect(
        scale_coefficients(material, "WaterScattering", WATER_SCATTERING_PER_METER, scale_pin, x - 700, y + 200),
        "", node, "ScatteringCoefficients")
    connect(
        scalar_parameter(material, "WaterPhaseG", WATER_PHASE_G, x - 700, y + 400), "", node, "PhaseG")
    return node


def create_material():
    if unreal.EditorAssetLibrary.does_asset_exist(MATERIAL_PATH):
        unreal.EditorAssetLibrary.delete_asset(MATERIAL_PATH)
        unreal.log("Deleted existing material for recreation: " + MATERIAL_PATH)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = asset_tools.create_asset(
        "M_River", "/Game/Materials", unreal.Material, unreal.MaterialFactoryNew()
    )

    # Single Layer Water rather than a translucent sheet. It draws in the opaque
    # pass and gets screen-space reflection, refraction of what is behind it, and
    # the absorption and scattering integral through the column — none of which a
    # translucent material has, and all of which are what water actually is.
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    # The ribbon is a single sheet, and the camera goes under it as soon as the
    # player stands in a stream.
    material.set_editor_property("two_sided", True)
    # The custom node returns a world-space normal: it builds one out of the
    # channel's own frame, and the ribbon carries no tangent basis to express it
    # against.
    material.set_editor_property("tangent_space_normal", False)

    river = build_river_graph(material)

    connect_property((river, ""), unreal.MaterialProperty.MP_BASE_COLOR)
    connect_property((river, "Normal"), unreal.MaterialProperty.MP_NORMAL)
    connect_property((river, "Roughness"), unreal.MaterialProperty.MP_ROUGHNESS)
    connect_property(
        (scalar_parameter(material, "WaterSpecular", WATER_SPECULAR, 400, 700), ""),
        unreal.MaterialProperty.MP_SPECULAR)
    make_water_column(material, (river, "ColumnScale"), 400, 1000)

    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(MATERIAL_PATH)
    unreal.log("Created material " + MATERIAL_PATH)


create_material()
