"""Imports the downloaded Poly Haven vegetation models as Nanite static meshes.

Run Tools/fetch_polyhaven_models.py first. Each <slug>/<slug>_<res>.gltf under
Content/Models/PolyHaven becomes /Game/Models/PolyHaven/<group>/SM_<Name>,
which is where MassifFoliageSpecies looks for it. Missing downloads are
skipped, so this is safe to run repeatedly as assets arrive.

glTF goes through Interchange rather than the FBX importer. One source holds
several whole plants side by side in a scene, so meshes are never combined and
their scene placement is not baked into the vertices: each variant keeps its own
origin, and the middle-sized one becomes the species mesh. Baking would leave
every instance drawn metres away from where the scatter put it.

Poly Haven's conifers are scan-grade with no authored LODs, so the meshes are
imported with Nanite on rather than decimated.

Idempotent: an asset that already exists is left alone unless --reimport.
Run headless with:
UnrealEditor-Cmd Massif.uproject -run=pythonscript \
    -script=Tools/create_foliage_content.py -unattended -nosplash
"""

import os

import unreal

MODEL_SOURCE_ROOT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Content", "Models", "PolyHaven"
)
MODEL_ASSET_ROOT = "/Game/Models/PolyHaven"

# Interchange parents its glTF materials to M_Default in the engine's plugin
# content, which is not flagged for instanced static meshes. Foliage is drawn
# through instanced components, so the renderer would swap in the grey default
# material. The project keeps its own copy of that chain, flagged for the way
# the foliage manager actually draws it.
FOLIAGE_MATERIAL_ROOT = "/Game/Materials/Foliage"
ENGINE_MATERIAL_ROOT = "/InterchangeAssets/"

# Poly Haven ships .gltf with a .bin beside it; .glb is accepted for hand-placed
# models. The stale .fbx archives from earlier downloads are not: they are zipped
# .blend files that Unreal cannot open.
SOURCE_EXTENSIONS = (".gltf", ".glb")


REIMPORT = "--reimport" in unreal.SystemLibrary.get_command_line()


def asset_name(slug):
    return "SM_" + "".join(part.capitalize() for part in slug.replace("-", "_").split("_"))


def find_source_model(group, slug):
    directory = os.path.join(MODEL_SOURCE_ROOT, group, slug)
    if not os.path.isdir(directory):
        return None
    for entry in sorted(os.listdir(directory)):
        if entry.endswith(SOURCE_EXTENSIONS):
            return os.path.join(directory, entry)
    return None


def warn_if_download_started(group, slug):
    """
    Imported assets land in the same tree as the downloads, so most directories
    without a source model are Interchange's own output. Only a directory that
    holds neither is a download that never finished.
    """
    directory = os.path.join(MODEL_SOURCE_ROOT, group, slug)
    if not os.path.isdir(directory):
        return
    for _, _, files in os.walk(directory):
        if any(entry.endswith(".uasset") for entry in files):
            return
    unreal.log_warning(f"No downloaded glTF for {group}/{slug}, skipping")


def make_import_pipeline(name):
    pipeline = unreal.InterchangeGenericAssetsPipeline()
    pipeline.set_editor_property("asset_name", name)
    pipeline.set_editor_property("use_source_name_for_asset", False)
    pipeline.set_editor_property("asset_type_sub_folders", False)
    pipeline.set_editor_property("scene_name_sub_folder", False)

    mesh_pipeline = pipeline.get_editor_property("mesh_pipeline")
    mesh_pipeline.set_editor_property("import_static_meshes", True)
    mesh_pipeline.set_editor_property(
        "combine_static_meshes_behavior", unreal.InterchangeCombineStaticMeshesBehavior.DO_NOT_COMBINE)
    mesh_pipeline.set_editor_property("build_nanite", True)
    mesh_pipeline.set_editor_property("create_physics_asset", False)

    # Without this every mesh carries its position in the source scene, so a
    # plant modelled ten metres from the origin is drawn ten metres from the
    # spot it was planted on.
    common_meshes = pipeline.get_editor_property("common_meshes_properties")
    common_meshes.set_editor_property("bake_meshes", False)
    return pipeline


def make_import_task(source, destination_path, name):
    task = unreal.AssetImportTask()
    task.filename = source
    task.destination_path = destination_path
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = True
    task.options = make_import_pipeline(name)
    return task


def project_copy_of(asset):
    """The project's copy of an engine material asset, made on first use."""
    path = f"{FOLIAGE_MATERIAL_ROOT}/{asset.get_name()}"
    existing = unreal.EditorAssetLibrary.load_asset(path)
    if existing is not None:
        return existing
    return unreal.EditorAssetLibrary.duplicate_asset(asset.get_path_name(), path)


def ensure_foliage_root_material(root):
    copy = project_copy_of(root)
    if copy is None:
        return None
    copy.set_editor_property("used_with_instanced_static_meshes", True)
    copy.set_editor_property("used_with_static_mesh", True)
    copy.set_editor_property("used_with_nanite", True)
    unreal.MaterialEditingLibrary.recompile_material(copy)
    unreal.EditorAssetLibrary.save_asset(copy.get_path_name())
    return copy


def preferred_engine_parent(parent):
    """
    Poly Haven exports leaf and needle cards as translucent, which Nanite cannot
    raster and which foliage does not want anyway: the cutouts are hard-edged.
    Those materials take the alpha-masked parent instead.
    """
    if "Blend" not in parent.get_name():
        return parent
    masked = unreal.EditorAssetLibrary.load_asset(
        f"{ENGINE_MATERIAL_ROOT}gltf/MaterialInstances/MI_Default_Mask_DS")
    if masked is None:
        return parent
    return masked


def ensure_foliage_parent(engine_parent):
    """Copies one of Interchange's blend-mode parents, reparented to the flagged root."""
    parent = preferred_engine_parent(engine_parent)
    copy = project_copy_of(parent)
    if copy is None:
        return None
    root = ensure_foliage_root_material(parent.get_base_material())
    if root is not None:
        copy.set_editor_property("parent", root)
    unreal.EditorAssetLibrary.save_asset(copy.get_path_name())
    return copy


def retarget_material(material):
    if not isinstance(material, unreal.MaterialInstanceConstant):
        return
    parent = material.get_editor_property("parent")
    if parent is None or not parent.get_path_name().startswith(ENGINE_MATERIAL_ROOT):
        return

    foliage_parent = ensure_foliage_parent(parent)
    if foliage_parent is None:
        return
    material.set_editor_property("parent", foliage_parent)
    unreal.EditorAssetLibrary.save_asset(material.get_path_name())
    unreal.log(f"Reparented {material.get_name()} to {foliage_parent.get_name()}")


def retarget_mesh_materials(mesh):
    for slot in mesh.get_editor_property("static_materials"):
        retarget_material(slot.material_interface)
    unreal.EditorAssetLibrary.save_asset(mesh.get_path_name())


def imported_static_meshes(task):
    meshes = []
    for object_path in task.get_editor_property("imported_object_paths"):
        package_path = object_path.split(".")[0]
        imported = unreal.EditorAssetLibrary.load_asset(package_path)
        if isinstance(imported, unreal.StaticMesh):
            meshes.append((package_path, imported))
    return meshes


def resolve_imported_mesh(task, asset_path):
    """
    One Poly Haven source holds several whole plants: seventeen grass clumps
    from tiny to tall, three saplings. The middle one by triangle count becomes
    the species mesh, so the scatter plants a representative plant rather than
    the largest one, and the rest stay beside it under their own names for a
    later pass that plants variants.
    """
    mesh = unreal.EditorAssetLibrary.load_asset(asset_path)
    if mesh is not None:
        return mesh

    meshes = imported_static_meshes(task)
    if not meshes:
        return None

    meshes.sort(key=lambda entry: (entry[1].get_num_triangles(0), entry[0]))
    package_path = meshes[len(meshes) // 2][0]
    unreal.log(f"Chose {package_path} of {len(meshes)} meshes -> {asset_path}")
    unreal.EditorAssetLibrary.rename_asset(package_path, asset_path)
    return unreal.EditorAssetLibrary.load_asset(asset_path)


def import_model(group, slug):
    name = asset_name(slug)
    asset_path = f"{MODEL_ASSET_ROOT}/{group}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path) and not REIMPORT:
        unreal.log(f"Already imported: {asset_path}")
        return True

    source = find_source_model(group, slug)
    if source is None:
        warn_if_download_started(group, slug)
        return False

    unreal.log(f"Importing {source} -> {asset_path}")
    task = make_import_task(source, f"{MODEL_ASSET_ROOT}/{group}", name)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    mesh = resolve_imported_mesh(task, asset_path)
    if mesh is None:
        unreal.log_warning(f"Import produced no asset at {asset_path}")
        return False

    retarget_mesh_materials(mesh)
    unreal.log(f"Imported {asset_path}")
    return True


def import_all():
    if not os.path.isdir(MODEL_SOURCE_ROOT):
        unreal.log_warning(f"No downloads at {MODEL_SOURCE_ROOT}; run Tools/fetch_polyhaven_models.py")
        return

    imported = 0
    for group in sorted(os.listdir(MODEL_SOURCE_ROOT)):
        group_directory = os.path.join(MODEL_SOURCE_ROOT, group)
        if not os.path.isdir(group_directory):
            continue
        for slug in sorted(os.listdir(group_directory)):
            if import_model(group, slug):
                imported += 1
    unreal.log(f"Foliage import finished: {imported} meshes available")


import_all()
