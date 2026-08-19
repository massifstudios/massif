"""Imports the downloaded Poly Haven vegetation models as Nanite static meshes.

Run Tools/fetch_polyhaven_models.py first. Each <slug>/<slug>_<res>.fbx under
Content/Models/PolyHaven becomes /Game/Models/PolyHaven/<group>/SM_<Name>,
which is where MassifFoliageSpecies looks for it. Missing downloads are
skipped, so this is safe to run repeatedly as assets arrive.

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

REIMPORT = "--reimport" in unreal.SystemLibrary.get_command_line()


def asset_name(slug):
    return "SM_" + "".join(part.capitalize() for part in slug.replace("-", "_").split("_"))


def find_source_fbx(group, slug):
    directory = os.path.join(MODEL_SOURCE_ROOT, group, slug)
    if not os.path.isdir(directory):
        return None
    for entry in sorted(os.listdir(directory)):
        if entry.endswith(".fbx"):
            return os.path.join(directory, entry)
    return None


def make_import_task(source, destination_path, name):
    options = unreal.FbxImportUI()
    options.set_editor_property("import_mesh", True)
    options.set_editor_property("import_as_skeletal", False)
    options.set_editor_property("import_materials", True)
    options.set_editor_property("import_textures", True)
    options.set_editor_property("static_mesh_import_data", unreal.FbxStaticMeshImportData())
    options.static_mesh_import_data.set_editor_property("combine_meshes", True)
    options.static_mesh_import_data.set_editor_property("generate_lightmap_u_vs", False)
    options.static_mesh_import_data.set_editor_property("build_nanite", True)

    task = unreal.AssetImportTask()
    task.filename = source
    task.destination_path = destination_path
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = True
    task.options = options
    return task


def import_model(group, slug):
    name = asset_name(slug)
    asset_path = f"{MODEL_ASSET_ROOT}/{group}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path) and not REIMPORT:
        unreal.log(f"Already imported: {asset_path}")
        return True

    source = find_source_fbx(group, slug)
    if source is None:
        unreal.log_warning(f"No downloaded FBX for {group}/{slug}, skipping")
        return False

    unreal.log(f"Importing {source} -> {asset_path}")
    task = make_import_task(source, f"{MODEL_ASSET_ROOT}/{group}", name)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    mesh = unreal.EditorAssetLibrary.load_asset(asset_path)
    if mesh is None:
        unreal.log_warning(f"Import produced no asset at {asset_path}")
        return False
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
