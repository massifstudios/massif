"""Creates the stand-in meshes the foliage table draws until its models exist.

One primitive per growth form, not one per species: MassifFoliageSpecies points
every species at the shape it grows in and supplies its own tint, so fifty-odd
species read apart on the hillside from sixteen meshes.

Each shape is an engine basic shape duplicated and rescaled at build time to a
plausible real size in metres — a 20 m spruce cone, a 6 cm cushion — so a
species' MinScale/MaxScale range means what it says and swapping in a real model
does not resize the world.

Idempotent: assets that already exist are rebuilt in place.

Run headless with:
UnrealEditor-Cmd Massif.uproject -run=pythonscript \
    -script=Tools/create_placeholder_flora.py -unattended -nosplash -nullrhi
"""

import unreal

MESH_PACKAGE_PATH = "/Game/Models/Placeholder"
MATERIAL_PACKAGE_PATH = "/Game/Materials"
MATERIAL_NAME = "M_FloraPlaceholder"
MATERIAL_PATH = MATERIAL_PACKAGE_PATH + "/" + MATERIAL_NAME

CONE = "/Engine/BasicShapes/Cone"
SPHERE = "/Engine/BasicShapes/Sphere"
CYLINDER = "/Engine/BasicShapes/Cylinder"

# Foliage reads as matte at a distance; a specular highlight on a placeholder
# only makes it harder to tell one tint from another.
PLACEHOLDER_ROUGHNESS = 0.85

# name, source shape, and the size in metres it is built to (width, depth, height).
# The engine basic shapes are one metre across, so the scale is the size.
ARCHETYPES = [
    ("SM_PhConiferSpire", CONE, (2.2, 2.2, 20.0)),
    ("SM_PhConiferBroad", CONE, (3.5, 3.5, 14.0)),
    ("SM_PhConiferOpen", CONE, (3.0, 3.0, 20.0)),
    ("SM_PhBroadleafRound", SPHERE, (6.0, 6.0, 8.0)),
    ("SM_PhTreeSmall", SPHERE, (3.5, 3.5, 5.0)),
    ("SM_PhShrubDense", SPHERE, (2.2, 2.2, 2.2)),
    ("SM_PhShrubLow", SPHERE, (1.4, 1.4, 0.6)),
    ("SM_PhDwarfShrub", SPHERE, (0.6, 0.6, 0.28)),
    ("SM_PhTussockGrass", CONE, (0.35, 0.35, 0.40)),
    ("SM_PhTurfGrass", CONE, (0.20, 0.20, 0.13)),
    ("SM_PhMeadowGrass", CONE, (0.30, 0.30, 0.60)),
    ("SM_PhSedgeTall", CYLINDER, (0.10, 0.10, 0.45)),
    ("SM_PhFlowerTall", CYLINDER, (0.06, 0.06, 0.45)),
    ("SM_PhFlowerLow", CYLINDER, (0.05, 0.05, 0.14)),
    ("SM_PhFlowerCushion", SPHERE, (0.16, 0.16, 0.08)),
    ("SM_PhRosette", CYLINDER, (0.14, 0.14, 0.05)),
]


def expr(material, expression_class, x, y):
    return unreal.MaterialEditingLibrary.create_material_expression(material, expression_class, x, y)


def create_material():
    """Flat colour driven by a Tint parameter the foliage manager sets per species.

    Flagged for instanced static meshes: foliage is drawn through instanced
    components, and a material without that flag is silently replaced by the grey
    engine default.
    """
    if unreal.EditorAssetLibrary.does_asset_exist(MATERIAL_PATH):
        unreal.EditorAssetLibrary.delete_asset(MATERIAL_PATH)

    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        MATERIAL_NAME, MATERIAL_PACKAGE_PATH, unreal.Material, unreal.MaterialFactoryNew()
    )
    material.set_editor_property("used_with_instanced_static_meshes", True)
    material.set_editor_property("used_with_static_mesh", True)
    material.set_editor_property("two_sided", True)

    tint = expr(material, unreal.MaterialExpressionVectorParameter, -400, 0)
    tint.set_editor_property("parameter_name", "Tint")
    tint.set_editor_property("default_value", unreal.LinearColor(0.25, 0.45, 0.15, 1.0))

    roughness = expr(material, unreal.MaterialExpressionConstant, -400, 250)
    roughness.set_editor_property("r", PLACEHOLDER_ROUGHNESS)

    unreal.MaterialEditingLibrary.connect_material_property(
        tint, "", unreal.MaterialProperty.MP_BASE_COLOR
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        roughness, "", unreal.MaterialProperty.MP_ROUGHNESS
    )

    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(MATERIAL_PATH)
    unreal.log("Created material " + MATERIAL_PATH)
    return material


def build_settings(size_meters):
    """Non-uniform build scale, with normals recomputed because it is non-uniform."""
    settings = unreal.MeshBuildSettings()
    settings.set_editor_property("recompute_normals", True)
    settings.set_editor_property("build_scale3d", unreal.Vector(*size_meters))
    return settings


def create_placeholder_mesh(editor_subsystem, material, name, source_path, size_meters):
    asset_path = MESH_PACKAGE_PATH + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        unreal.EditorAssetLibrary.delete_asset(asset_path)

    mesh = unreal.EditorAssetLibrary.duplicate_asset(source_path, asset_path)
    if mesh is None:
        unreal.log_warning(f"Could not duplicate {source_path} to {asset_path}")
        return False

    editor_subsystem.set_lod_build_settings(mesh, 0, build_settings(size_meters))
    mesh.set_editor_property(
        "static_materials",
        [unreal.StaticMaterial(material_interface=material, material_slot_name="Placeholder")],
    )
    unreal.EditorAssetLibrary.save_asset(asset_path)
    unreal.log(f"Created {asset_path} at {size_meters[0]:.2f} x {size_meters[2]:.2f} m")
    return True


def create_all():
    material = create_material()
    # The commandlet does not pull in the static mesh editor on its own, and the
    # subsystem that owns build settings comes back None without it.
    unreal.load_module("StaticMeshEditor")
    editor_subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)

    created = 0
    for name, source_path, size_meters in ARCHETYPES:
        if create_placeholder_mesh(editor_subsystem, material, name, source_path, size_meters):
            created += 1
    unreal.log(f"Placeholder flora finished: {created} of {len(ARCHETYPES)} shapes")


create_all()
