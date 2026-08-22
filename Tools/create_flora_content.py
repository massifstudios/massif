"""Builds the flora tree assets from the models Tools/flora_obj_to_gltf.py wrote.

Each Content/Models/Flora/<Name>/<Name>.flora.json manifest becomes one static
mesh at /Game/Models/Flora/SM_<Name>, which is where MassifFoliageSpecies looks
for the finished model. The manifest's levels become the mesh's LODs, so a tree
drawn a kilometre away costs two triangles and one texture read.

Level 3 is an impostor: one quad carrying an eight-yaw atlas of the near tree.
M_FloraImpostor turns that quad to face the camera and picks the atlas cell for
the bearing it turned through, which is what keeps a distant spruce looking like
the spruce it stands for instead of a flat cut-out seen edge-on.

Textures are imported from the manifest rather than through the glTF, because a
normal atlas needs sRGB off and a green flip, and a foliage card needs its alpha
kept — none of which survives glTF's material model.

Idempotent: the generated meshes, materials and textures are rebuilt in place.

Run headless with:
UnrealEditor-Cmd Massif.uproject -run=pythonscript \
    -script=Tools/create_flora_content.py -unattended -nosplash
"""

import json
import os

import unreal

MODEL_SOURCE_ROOT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Content", "Models", "Flora")
MODEL_ASSET_ROOT = "/Game/Models/Flora"
TEXTURE_ASSET_ROOT = "/Game/Textures/Flora"
MATERIAL_ASSET_ROOT = "/Game/Materials/Flora"

BARK_MATERIAL = MATERIAL_ASSET_ROOT + "/M_FloraBark"
FOLIAGE_MATERIAL = MATERIAL_ASSET_ROOT + "/M_FloraFoliage"
IMPOSTOR_MATERIAL = MATERIAL_ASSET_ROOT + "/M_FloraImpostor"

# Parameter names the material instances override, shared by every master.
BASE_COLOR_PARAMETER = "BaseColor"
NORMAL_PARAMETER = "Normal"

DEFAULT_COLOR_TEXTURE = "/Engine/EngineMaterials/DefaultDiffuse"
DEFAULT_NORMAL_TEXTURE = "/Engine/EngineMaterials/DefaultNormal"

BARK_ROUGHNESS = 0.9
FOLIAGE_ROUGHNESS = 0.7

# Light that made it through the needles and out the far side. Foliage lit as an
# opaque surface reads as cardboard from below, where a canopy is mostly backlit.
FOLIAGE_TRANSMISSION = unreal.LinearColor(0.055, 0.105, 0.035, 1.0)

# The atlas layout the impostor material is compiled for. A manifest that
# disagrees is rejected rather than drawn from the wrong cells.
IMPOSTOR_ATLAS = {"columns": 4, "rows": 2, "angles": 8}

# Rotating the impostor quad about the trunk sweeps it outside the near mesh's
# bounds, and a primitive culled on its own bounds would vanish before it is
# meant to. In centimetres.
BOUNDS_MARGIN = 100.0


def asset_tools():
    return unreal.AssetToolsHelpers.get_asset_tools()


def static_mesh_subsystem():
    """The LOD editing API, whose module a commandlet does not load on its own."""
    unreal.load_module("StaticMeshEditor")
    subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    if subsystem is None:
        raise RuntimeError("StaticMeshEditorSubsystem is unavailable")
    return subsystem


def expr(material, expression_class, x, y):
    return unreal.MaterialEditingLibrary.create_material_expression(material, expression_class, x, y)


def connect(from_expression, from_output, to_expression, to_input):
    unreal.MaterialEditingLibrary.connect_material_expressions(
        from_expression, from_output, to_expression, to_input)


def recreate_asset(path):
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)


def create_material(path):
    recreate_asset(path)
    directory, name = path.rsplit("/", 1)
    material = asset_tools().create_asset(name, directory, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("used_with_instanced_static_meshes", True)
    material.set_editor_property("used_with_static_mesh", True)
    return material


def texture_parameter(material, name, default_path, x, y):
    """
    The parameter needs some texture to compile against; every instance
    overrides it. Loaded directly rather than through the asset registry,
    which a commandlet has not scanned engine content into.
    """
    parameter = expr(material, unreal.MaterialExpressionTextureSampleParameter2D, x, y)
    parameter.set_editor_property("parameter_name", name)
    parameter.set_editor_property("texture", unreal.load_asset(default_path))
    return parameter


def constant(material, value, x, y):
    node = expr(material, unreal.MaterialExpressionConstant, x, y)
    node.set_editor_property("r", value)
    return node


def constant_vector(material, color, x, y):
    node = expr(material, unreal.MaterialExpressionConstant3Vector, x, y)
    node.set_editor_property("constant", color)
    return node


def finish_material(material, path):
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path)
    unreal.log("Created material " + path)
    return material


def create_bark_material():
    """Opaque wood: the bole and the boughs the near levels still mesh."""
    material = create_material(BARK_MATERIAL)
    color = texture_parameter(material, BASE_COLOR_PARAMETER, DEFAULT_COLOR_TEXTURE, -400, 0)
    unreal.MaterialEditingLibrary.connect_material_property(
        color, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    unreal.MaterialEditingLibrary.connect_material_property(
        constant(material, BARK_ROUGHNESS, -400, 350), "", unreal.MaterialProperty.MP_ROUGHNESS)
    return finish_material(material, BARK_MATERIAL)


def create_foliage_material(alpha_cutoff):
    """
    Twig cards: a cut-out texture on a two-sided quad, lit as foliage so light
    passes through the needles rather than stopping at the card.
    """
    material = create_material(FOLIAGE_MATERIAL)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("opacity_mask_clip_value", alpha_cutoff)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)

    color = texture_parameter(material, BASE_COLOR_PARAMETER, DEFAULT_COLOR_TEXTURE, -400, 0)
    unreal.MaterialEditingLibrary.connect_material_property(
        color, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    unreal.MaterialEditingLibrary.connect_material_property(
        color, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
    unreal.MaterialEditingLibrary.connect_material_property(
        constant(material, FOLIAGE_ROUGHNESS, -400, 350), "", unreal.MaterialProperty.MP_ROUGHNESS)
    unreal.MaterialEditingLibrary.connect_material_property(
        constant_vector(material, FOLIAGE_TRANSMISSION, -400, 450), "",
        unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    return finish_material(material, FOLIAGE_MATERIAL)


def atlas_cell_code(atlas):
    """
    Atlas cell for the bearing the camera looks from, in the quad's own frame.

    The flora bake walks yaw the other way round than Unreal does — flora is
    right-handed and Unreal is not, and the glTF conversion mirrors Y — so the
    angle is negated before it picks a cell. Rounding to the nearest baked yaw
    rather than the one below halves the worst-case error.
    """
    return (
        "float Angle = atan2(Direction.y, Direction.x);\n"
        "float Turns = frac(-Angle / (2.0f * PI));\n"
        "float Cell = fmod(floor(Turns * {angles}.0f + 0.5f), {angles}.0f);\n"
        "float Column = fmod(Cell, {columns}.0f);\n"
        "float Row = floor(Cell / {columns}.0f);\n"
        "return float2((UV.x + Column) / {columns}.0f, (UV.y + Row) / {rows}.0f);"
    ).format(**atlas)


FACING_TURNS_CODE = (
    # The quad is authored facing +Y in mesh space, so a quarter turn short of
    # the camera bearing is what points it at the camera.
    "return atan2(Direction.y, Direction.x) / (2.0f * PI) - 0.25f;"
)


def custom_input(name):
    field = unreal.CustomInput()
    field.set_editor_property("input_name", name)
    return field


def custom_expression(material, name, code, inputs, output_type, x, y):
    node = expr(material, unreal.MaterialExpressionCustom, x, y)
    node.set_editor_property("description", name)
    node.set_editor_property("code", code)
    node.set_editor_property("output_type", output_type)
    node.set_editor_property("inputs", [custom_input(field) for field in inputs])
    return node


def create_impostor_material(alpha_cutoff, atlas):
    """
    The far level: one quad spun to face the camera, textured from the atlas
    cell baked at the bearing it turned through.

    Instance space rather than local space, because the trees are drawn through
    instanced components and each instance carries its own yaw. Reading the
    component's frame would give every tree in the tile the same atlas cell.
    """
    material = create_material(IMPOSTOR_MATERIAL)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("opacity_mask_clip_value", alpha_cutoff)
    material.set_editor_property("two_sided", True)

    object_position = expr(material, unreal.MaterialExpressionObjectPositionWS, -1500, 300)
    camera_position = expr(material, unreal.MaterialExpressionCameraPositionWS, -1500, 150)
    to_camera = expr(material, unreal.MaterialExpressionSubtract, -1300, 200)
    connect(camera_position, "", to_camera, "A")
    connect(object_position, "", to_camera, "B")

    direction = expr(material, unreal.MaterialExpressionTransform, -1100, 200)
    direction.set_editor_property(
        "transform_source_type", unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD)
    direction.set_editor_property(
        "transform_type", unreal.MaterialVectorCoordTransform.TRANSFORM_INSTANCE)
    connect(to_camera, "", direction, "")

    coordinates = expr(material, unreal.MaterialExpressionTextureCoordinate, -1100, 0)
    cell = custom_expression(material, "AtlasCell", atlas_cell_code(atlas), ["Direction", "UV"],
                             unreal.CustomMaterialOutputType.CMOT_FLOAT2, -800, 0)
    connect(direction, "", cell, "Direction")
    connect(coordinates, "", cell, "UV")

    color = texture_parameter(material, BASE_COLOR_PARAMETER, DEFAULT_COLOR_TEXTURE, -500, -100)
    connect(cell, "", color, "UVs")
    unreal.MaterialEditingLibrary.connect_material_property(
        color, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    unreal.MaterialEditingLibrary.connect_material_property(
        color, "A", unreal.MaterialProperty.MP_OPACITY_MASK)

    normal = texture_parameter(material, NORMAL_PARAMETER, DEFAULT_NORMAL_TEXTURE, -500, 300)
    normal.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    connect(cell, "", normal, "UVs")
    unreal.MaterialEditingLibrary.connect_material_property(
        normal, "RGB", unreal.MaterialProperty.MP_NORMAL)

    unreal.MaterialEditingLibrary.connect_material_property(
        constant(material, FOLIAGE_ROUGHNESS, -500, 650), "", unreal.MaterialProperty.MP_ROUGHNESS)

    facing = custom_expression(material, "FacingTurns", FACING_TURNS_CODE, ["Direction"],
                               unreal.CustomMaterialOutputType.CMOT_FLOAT1, -800, 400)
    connect(direction, "", facing, "Direction")

    spin = expr(material, unreal.MaterialExpressionRotateAboutAxis, -400, 800)
    connect(constant_vector(material, unreal.LinearColor(0.0, 0.0, 1.0, 1.0), -800, 700), "",
            spin, "NormalizedRotationAxis")
    connect(facing, "", spin, "RotationAngle")
    connect(object_position, "", spin, "PivotPoint")
    connect(expr(material, unreal.MaterialExpressionWorldPosition, -800, 950), "", spin, "Position")
    unreal.MaterialEditingLibrary.connect_material_property(
        spin, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    return finish_material(material, IMPOSTOR_MATERIAL)


def texture_asset_path(file_name):
    stem = os.path.splitext(os.path.basename(file_name))[0]
    return f"{TEXTURE_ASSET_ROOT}/T_{stem.replace('_', '')}"


def import_texture(source_path, asset_path, is_normal_map):
    recreate_asset(asset_path)
    task = unreal.AssetImportTask()
    task.filename = source_path
    task.destination_path = asset_path.rsplit("/", 1)[0]
    task.destination_name = asset_path.rsplit("/", 1)[1]
    task.automated = True
    task.replace_existing = True
    task.save = True
    asset_tools().import_asset_tasks([task])

    texture = unreal.EditorAssetLibrary.load_asset(asset_path)
    if texture is None:
        raise RuntimeError(f"texture import produced no asset at {asset_path}")
    if is_normal_map:
        # The bake writes the green channel pointing up the image, which is the
        # opposite of what Unreal's tangent basis expects.
        texture.set_editor_property("srgb", False)
        texture.set_editor_property("compression_settings",
                                    unreal.TextureCompressionSettings.TC_NORMALMAP)
        texture.set_editor_property("flip_green_channel", True)
    else:
        texture.set_editor_property("srgb", True)
    unreal.EditorAssetLibrary.save_asset(asset_path)
    return texture


def import_material_textures(model_directory, entry):
    textures = {}
    for map_name, relative_path in entry["textures"].items():
        asset_path = texture_asset_path(relative_path)
        textures[map_name] = import_texture(
            os.path.join(model_directory, relative_path), asset_path, map_name == "normal")
    return textures


def instance_asset_path(model_name, material_name):
    suffix = "".join(part.capitalize() for part in material_name.split("_"))
    return f"{MATERIAL_ASSET_ROOT}/MI_{model_name}{suffix}"


def create_material_instance(model_name, entry, parent, textures):
    asset_path = instance_asset_path(model_name, entry["name"])
    recreate_asset(asset_path)
    directory, name = asset_path.rsplit("/", 1)
    instance = asset_tools().create_asset(
        name, directory, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    instance.set_editor_property("parent", parent)
    for map_name, parameter in (("color", BASE_COLOR_PARAMETER), ("normal", NORMAL_PARAMETER)):
        if map_name in textures:
            unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(
                instance, parameter, textures[map_name])
    unreal.EditorAssetLibrary.save_asset(asset_path)
    return instance


def build_materials(manifest, model_directory):
    """One material instance per material the manifest names, by role."""
    masters = {
        "bark": create_bark_material(),
        "foliage": create_foliage_material(manifest["alphaCutoff"]),
        "impostor": create_impostor_material(manifest["alphaCutoff"], impostor_atlas(manifest)),
    }
    instances = {}
    for entry in manifest["materials"]:
        textures = import_material_textures(model_directory, entry)
        instances[entry["name"]] = create_material_instance(
            manifest["name"], entry, masters[entry["role"]], textures)
    return instances


def impostor_atlas(manifest):
    for entry in manifest["materials"]:
        if entry["role"] != "impostor":
            continue
        if entry["atlas"] != IMPOSTOR_ATLAS:
            raise RuntimeError(
                f"{manifest['name']} was baked as {entry['atlas']}, and M_FloraImpostor reads "
                f"{IMPOSTOR_ATLAS}")
        return entry["atlas"]
    return IMPOSTOR_ATLAS


def make_import_pipeline(name):
    """
    Geometry only. The glTF names its materials so the mesh comes in with the
    right slots, but the materials themselves are authored here.
    """
    pipeline = unreal.InterchangeGenericAssetsPipeline()
    pipeline.set_editor_property("asset_name", name)
    pipeline.set_editor_property("use_source_name_for_asset", False)
    pipeline.set_editor_property("asset_type_sub_folders", False)
    pipeline.set_editor_property("scene_name_sub_folder", False)

    mesh_pipeline = pipeline.get_editor_property("mesh_pipeline")
    mesh_pipeline.set_editor_property("import_static_meshes", True)
    mesh_pipeline.set_editor_property(
        "combine_static_meshes_behavior", unreal.InterchangeCombineStaticMeshesBehavior.DO_NOT_COMBINE)
    # Authored levels, so the mesh keeps its LOD chain instead of Nanite's.
    mesh_pipeline.set_editor_property("build_nanite", False)
    mesh_pipeline.set_editor_property("create_physics_asset", False)

    material_pipeline = pipeline.get_editor_property("material_pipeline")
    material_pipeline.set_editor_property("import_materials", False)
    return pipeline


def imported_static_mesh(task):
    """
    The import reports what it made, and that report is the only way to the
    object: a commandlet's asset registry does not see an asset created during
    its own run, and the importer files the mesh under a folder of its own
    naming rather than the one it was asked for.
    """
    for object_path in task.get_editor_property("imported_object_paths"):
        imported = unreal.load_asset(object_path.split(".")[0])
        if isinstance(imported, unreal.StaticMesh):
            return imported
    return None


def import_level_mesh(source_path, asset_name):
    asset_path = f"{MODEL_ASSET_ROOT}/{asset_name}"
    recreate_asset(asset_path)

    task = unreal.AssetImportTask()
    task.filename = source_path
    task.destination_path = MODEL_ASSET_ROOT
    task.destination_name = asset_name
    task.automated = True
    task.replace_existing = True
    task.save = False
    task.options = make_import_pipeline(asset_name)
    asset_tools().import_asset_tasks([task])

    mesh = imported_static_mesh(task)
    if mesh is None:
        raise RuntimeError(f"glTF import of {source_path} produced no static mesh")
    import_directory = mesh.get_path_name().rsplit("/", 1)[0]
    if import_directory != MODEL_ASSET_ROOT:
        unreal.EditorAssetLibrary.rename_loaded_asset(mesh, asset_path)
        unreal.EditorAssetLibrary.delete_directory(
            f"{MODEL_ASSET_ROOT}/{os.path.splitext(os.path.basename(source_path))[0]}")
    return mesh


def assign_materials(mesh, material_names, instances):
    """
    Slots follow the glTF primitives, which follow the manifest's material list
    for that level, so the two line up by position. Naming the slots after the
    materials keeps that legible in the mesh editor.
    """
    slots = mesh.get_editor_property("static_materials")
    if len(slots) != len(material_names):
        raise RuntimeError(
            f"{mesh.get_name()} imported {len(slots)} material slots for {len(material_names)} "
            "materials")
    mesh.set_editor_property("static_materials", [
        unreal.StaticMaterial(material_interface=instances[name], material_slot_name=name)
        for name in material_names])


def assemble_lods(manifest, model_directory, instances):
    """
    Level 0 is the asset; the rest are copied into it as LODs and their import
    assets dropped. Copying reuses a material slot when the material object is
    the same one, which is why every level is given its instances first.
    """
    name = manifest["name"]
    levels = manifest["levels"]
    subsystem = static_mesh_subsystem()

    destination = None
    coarse_meshes = []
    for level in levels:
        asset_name = f"SM_{name}" if level["level"] == 0 else f"SM_{name}_LOD{level['level']}"
        mesh = import_level_mesh(os.path.join(model_directory, level["file"]), asset_name)
        verify_bounds(mesh, level)
        assign_materials(mesh, level["materials"], instances)
        if level["level"] == 0:
            destination = mesh
            continue
        coarse_meshes.append(mesh)
        subsystem.set_lod_from_static_mesh(destination, level["level"], mesh, 0, True)

    unreal.EditorAssetLibrary.delete_loaded_assets(coarse_meshes)
    return destination


def expected_bounds_centimeters(level):
    """
    The manifest's box in Unreal's frame: glTF is metres with Y up, Unreal is
    centimetres with Z up, so the importer swaps the last two axes and scales by
    a hundred.
    """
    def convert(corner):
        return unreal.Vector(100.0 * corner[0], 100.0 * corner[2], 100.0 * corner[1])

    return convert(level["boundsMin"]), convert(level["boundsMax"])


def verify_bounds(mesh, level):
    """
    Checks the imported mesh against the box the manifest measured.

    Two assumptions ride on this. Size: metres read as centimetres would still
    look like a tree, just one planted in a model railway. And axes: the
    impostor material spins its quad about the trunk and reads an atlas cell
    from the bearing, both in the mesh's own frame, so a mesh whose Y came in
    the other way round would face the camera with the wrong side and the wrong
    cell. Neither shows up as an error anywhere else.
    """
    minimum, maximum = expected_bounds_centimeters(level)
    bounds = mesh.get_bounds()
    for axis in ("x", "y", "z"):
        expected = 0.5 * (getattr(minimum, axis) + getattr(maximum, axis))
        extent = 0.5 * (getattr(maximum, axis) - getattr(minimum, axis))
        tolerance = 1.0 + 0.01 * max(extent, 1.0)
        centre = getattr(bounds.origin, axis)
        if abs(centre - expected) > tolerance or abs(getattr(bounds.box_extent, axis) - extent) > tolerance:
            raise RuntimeError(
                f"{mesh.get_name()} LOD{level['level']} came in centred {centre:.1f} by "
                f"{getattr(bounds.box_extent, axis):.1f} on {axis}, expected {expected:.1f} by "
                f"{extent:.1f}: the importer's axes are not the ones the flora materials assume")


def disable_nanite(mesh):
    """
    Nanite draws its own cluster hierarchy and ignores the LOD chain, which is
    the whole ladder this asset is built around — including the impostor, whose
    material moves vertices that Nanite would not.
    """
    subsystem = static_mesh_subsystem()
    settings = subsystem.get_nanite_settings(mesh)
    settings.set_editor_property("enabled", False)
    subsystem.set_nanite_settings(mesh, settings, True)


def build_model(manifest_path):
    with open(manifest_path, "r", encoding="utf-8") as source:
        manifest = json.load(source)
    model_directory = os.path.dirname(manifest_path)

    instances = build_materials(manifest, model_directory)
    mesh = assemble_lods(manifest, model_directory, instances)

    disable_nanite(mesh)
    static_mesh_subsystem().set_lod_screen_sizes(
        mesh, [level["screenSize"] for level in manifest["levels"]])
    margin = unreal.Vector(BOUNDS_MARGIN, BOUNDS_MARGIN, 0.0)
    mesh.set_editor_property("positive_bounds_extension", margin)
    mesh.set_editor_property("negative_bounds_extension", margin)
    mesh.set_editor_property("light_map_resolution", 8)

    unreal.EditorAssetLibrary.save_loaded_asset(mesh)
    unreal.log(f"Built {mesh.get_path_name()} with "
               f"{static_mesh_subsystem().get_lod_count(mesh)} LODs, "
               f"{2.0 * mesh.get_bounds().box_extent.z / 100.0:.1f} m tall")


def find_manifests():
    if not os.path.isdir(MODEL_SOURCE_ROOT):
        return []
    manifests = []
    for name in sorted(os.listdir(MODEL_SOURCE_ROOT)):
        path = os.path.join(MODEL_SOURCE_ROOT, name, name + ".flora.json")
        if os.path.isfile(path):
            manifests.append(path)
    return manifests


def build_all():
    manifests = find_manifests()
    if not manifests:
        unreal.log_warning(f"No flora models under {MODEL_SOURCE_ROOT}")
        return
    for manifest_path in manifests:
        build_model(manifest_path)
    unreal.log(f"Flora content finished: {len(manifests)} models built")


build_all()
