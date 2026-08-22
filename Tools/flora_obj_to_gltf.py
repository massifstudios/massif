"""Converts one flora tree, level set and all, into the glTF this project imports.

The flora simulation (../massif-flora-simulation) grows a plant and writes each
detail level as an OBJ in metres with Z up. Unreal reads glTF, so this rewrites
the level set into one glTF per level plus a manifest naming the levels, the
material roles and the textures. Tools/create_flora_content.py reads that
manifest and builds the static mesh.

The glTF carries geometry and material names only. Textures are copied beside it
under project asset names and imported explicitly, because the importer wants
sRGB off for a normal map, a masked material for a foliage card and an atlas
material for the impostor, and none of that survives a round trip through glTF's
material model.

Norway spruce, as committed, comes from:

    cd ../massif-flora-simulation
    for level in 0 1 2 3; do
        ./build/flora-cli/flora grow 3 30 \\
            --species presets/spruce.json --materials presets/spruce.materials.json \\
            --lod $level --obj /tmp/spruce/spruce_lod$level.obj
    done
    ./build/flora-viewer/flora-viewer 3 30 \\
        --species presets/spruce.json --materials presets/spruce.materials.json \\
        --bake-impostor /tmp/spruce/spruce-impostor 2048

    cd ../massif
    python3 Tools/flora_obj_to_gltf.py --name PiceaAbies --species "Norway Spruce" \\
        --source /tmp/spruce --textures ../massif-flora-simulation/textures \\
        --seed 3 --steps 30
    task flora
"""

import argparse
import json
import os
import shutil
import struct

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUTPUT_ROOT = os.path.join(PROJECT_ROOT, "Content", "Models", "Flora")

# The level the flora LOD ladder replaces the tree with a baked atlas quad. Its
# OBJ names the material it inherited from the near mesh; the atlas is what it
# actually draws, so the material is renamed on the way through.
IMPOSTOR_LEVEL = 3
IMPOSTOR_MATERIAL = "impostor"

# Atlas layout the flora bake writes, and the yaws it holds. The impostor
# material selects a cell from these, so they travel with the asset rather than
# being assumed on the Unreal side.
IMPOSTOR_ATLAS_COLUMNS = 4
IMPOSTOR_ATLAS_ROWS = 2
IMPOSTOR_ANGLES = 8

COMPONENT_TYPE_FLOAT = 5126
COMPONENT_TYPE_UNSIGNED_INT = 5125
TARGET_ARRAY_BUFFER = 34962
TARGET_ELEMENT_ARRAY_BUFFER = 34963


class MeshGroup:
    """One run of triangles drawn with one material."""

    def __init__(self, material):
        self.material = material
        self.indices = []


class LevelMesh:
    """One detail level: shared vertices, one group per material."""

    def __init__(self):
        self.positions = []
        self.normals = []
        self.uvs = []
        self.groups = []


def read_obj(path):
    """
    Reads a flora OBJ. Position, texture coordinate and normal arrays run in
    parallel there — every face vertex is written a/a/a — so one index reaches
    all three and the vertex buffer needs no welding.
    """
    mesh = LevelMesh()
    with open(path, "r", encoding="utf-8") as source:
        for line in source:
            fields = line.split()
            if not fields:
                continue
            keyword = fields[0]
            if keyword == "v":
                mesh.positions.append(tuple(float(value) for value in fields[1:4]))
            elif keyword == "vt":
                mesh.uvs.append(tuple(float(value) for value in fields[1:3]))
            elif keyword == "vn":
                mesh.normals.append(tuple(float(value) for value in fields[1:4]))
            elif keyword == "usemtl":
                mesh.groups.append(MeshGroup(fields[1]))
            elif keyword == "f":
                read_face(mesh, fields[1:])
    mesh.groups = [group for group in mesh.groups if group.indices]
    return mesh


def read_face(mesh, vertices):
    if not mesh.groups:
        mesh.groups.append(MeshGroup("default"))
    corners = [int(vertex.split("/")[0]) - 1 for vertex in vertices]
    for triangle in range(1, len(corners) - 1):
        mesh.groups[-1].indices.extend((corners[0], corners[triangle], corners[triangle + 1]))


def read_mtl(path, texture_directory):
    """
    Material name to the texture files it names. The MTL writes texture paths
    relative to wherever the OBJ was written, which says nothing once the OBJ
    has been moved, so the files are looked up by name in the flora texture
    directory instead.
    """
    materials = {}
    current = None
    map_keywords = {"map_Kd": "color", "map_d": "mask", "map_Bump": "normal", "disp": "depth"}
    with open(path, "r", encoding="utf-8") as source:
        for line in source:
            fields = line.split()
            if not fields:
                continue
            if fields[0] == "newmtl":
                current = {}
                materials[fields[1]] = current
            elif fields[0] in map_keywords and current is not None:
                current[map_keywords[fields[0]]] = os.path.join(
                    texture_directory, os.path.basename(fields[-1]))
    return materials


def to_gltf_position(position):
    """
    Flora is right-handed with Z up and glTF is right-handed with Y up, so the
    conversion is a quarter turn about X. It is a rotation, not a mirror, which
    is what keeps triangle winding and normals valid without touching either.
    """
    x, y, z = position
    return (x, z, -y)


def to_gltf_uv(uv):
    """OBJ counts V up from the bottom of the image, glTF down from the top."""
    return (uv[0], 1.0 - uv[1])


def pack_vec3(values):
    return b"".join(struct.pack("<3f", *value) for value in values)


def pack_vec2(values):
    return b"".join(struct.pack("<2f", *value) for value in values)


def pack_indices(indices):
    return struct.pack("<%dI" % len(indices), *indices)


def bounds_of(positions):
    minimum = [min(position[axis] for position in positions) for axis in range(3)]
    maximum = [max(position[axis] for position in positions) for axis in range(3)]
    return minimum, maximum


def node_name(model_name, level):
    """
    Unreal reads a node called anything_LOD0 as one rung of an LOD ladder held
    in a single file, which is not how the levels are shipped, so the node is
    named in a way that does not trip that.
    """
    return f"{model_name}Detail{level}"


def build_gltf(mesh, material_names, buffer_uri, name):
    """
    One glTF mesh, one primitive per material, all primitives over the same
    vertex accessors. Materials carry names only: what a foliage card or an
    impostor needs is authored in Unreal, not describable here.
    """
    positions = [to_gltf_position(position) for position in mesh.positions]
    normals = [to_gltf_position(normal) for normal in mesh.normals]
    uvs = [to_gltf_uv(uv) for uv in mesh.uvs]

    blobs = [pack_vec3(positions), pack_vec3(normals), pack_vec2(uvs)]
    views = []
    offset = 0
    for blob in blobs:
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(blob),
                      "target": TARGET_ARRAY_BUFFER})
        offset += len(blob)

    minimum, maximum = bounds_of(positions)
    accessors = [
        {"bufferView": 0, "componentType": COMPONENT_TYPE_FLOAT, "count": len(positions),
         "type": "VEC3", "min": minimum, "max": maximum},
        {"bufferView": 1, "componentType": COMPONENT_TYPE_FLOAT, "count": len(normals),
         "type": "VEC3"},
        {"bufferView": 2, "componentType": COMPONENT_TYPE_FLOAT, "count": len(uvs),
         "type": "VEC2"},
    ]

    primitives = []
    for group in mesh.groups:
        blob = pack_indices(group.indices)
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(blob),
                      "target": TARGET_ELEMENT_ARRAY_BUFFER})
        offset += len(blob)
        accessors.append({"bufferView": len(views) - 1,
                          "componentType": COMPONENT_TYPE_UNSIGNED_INT,
                          "count": len(group.indices), "type": "SCALAR"})
        primitives.append({
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": len(accessors) - 1,
            "material": material_names.index(group.material),
        })
        blobs.append(blob)

    document = {
        "asset": {"version": "2.0", "generator": "massif flora_obj_to_gltf"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [{"primitives": primitives, "name": name}],
        "materials": [{"name": name, "doubleSided": name != "bark"} for name in material_names],
        "buffers": [{"uri": buffer_uri, "byteLength": offset}],
        "bufferViews": views,
        "accessors": accessors,
    }
    return document, b"".join(blobs)


def material_role(name):
    if name == IMPOSTOR_MATERIAL:
        return "impostor"
    if name == "bark":
        return "bark"
    return "foliage"


def texture_asset_name(model_name, material_name, map_name):
    """Project-side file name, so the imported texture is named after the tree."""
    suffix = {"bark": "Bark", "impostor": "Impostor"}.get(material_name)
    if suffix is None:
        suffix = "Foliage" + material_name.rsplit("_", 1)[-1]
    if map_name != "color":
        suffix += map_name.capitalize()
    return f"{model_name}_{suffix}.png"


def copy_texture(source, destination_directory, file_name):
    if not os.path.isfile(source):
        raise SystemExit(f"missing texture {source}")
    os.makedirs(destination_directory, exist_ok=True)
    shutil.copyfile(source, os.path.join(destination_directory, file_name))
    return "textures/" + file_name


def level_materials(model_name, mesh, mtl, impostor_maps, output_directory):
    """Manifest entries for the materials one level uses, textures copied out."""
    entries = []
    for group in mesh.groups:
        maps = impostor_maps if group.material == IMPOSTOR_MATERIAL else mtl.get(group.material, {})
        textures = {}
        for map_name in ("color", "normal"):
            if map_name not in maps:
                continue
            textures[map_name] = copy_texture(
                maps[map_name], os.path.join(output_directory, "textures"),
                texture_asset_name(model_name, group.material, map_name))
        entry = {"name": group.material, "role": material_role(group.material),
                 "textures": textures}
        if group.material == IMPOSTOR_MATERIAL:
            entry["atlas"] = {"columns": IMPOSTOR_ATLAS_COLUMNS, "rows": IMPOSTOR_ATLAS_ROWS,
                              "angles": IMPOSTOR_ANGLES}
        entries.append(entry)
    return entries


def rename_impostor_material(mesh):
    """The impostor level draws one group, whatever the near mesh called it."""
    for group in mesh.groups:
        group.material = IMPOSTOR_MATERIAL


def convert_level(arguments, level, output_directory, materials):
    obj_path = os.path.join(arguments.source, f"{arguments.prefix}{level}.obj")
    if not os.path.isfile(obj_path):
        raise SystemExit(f"missing level {level} at {obj_path}")

    mesh = read_obj(obj_path)
    mtl = read_mtl(os.path.splitext(obj_path)[0] + ".mtl", arguments.textures)
    impostor_maps = {}
    if level == IMPOSTOR_LEVEL:
        rename_impostor_material(mesh)
        impostor_maps = {"color": arguments.impostor_prefix + "_albedo.png",
                         "normal": arguments.impostor_prefix + "_normal.png"}

    for entry in level_materials(arguments.name, mesh, mtl, impostor_maps, output_directory):
        if all(existing["name"] != entry["name"] for existing in materials):
            materials.append(entry)

    file_name = f"{arguments.name}_LOD{level}"
    document, blob = build_gltf(mesh, [entry["name"] for entry in materials], file_name + ".bin",
                                node_name(arguments.name, level))
    with open(os.path.join(output_directory, file_name + ".bin"), "wb") as binary:
        binary.write(blob)
    with open(os.path.join(output_directory, file_name + ".gltf"), "w", encoding="utf-8") as text:
        json.dump(document, text, indent=1)

    minimum, maximum = bounds_of([to_gltf_position(position) for position in mesh.positions])
    return {
        "level": level,
        "file": file_name + ".gltf",
        "screenSize": arguments.screen_sizes[level],
        "materials": [group.material for group in mesh.groups],
        "vertices": len(mesh.positions),
        "triangles": sum(len(group.indices) for group in mesh.groups) // 3,
        # In glTF axes and metres, which is what the importer reads. Unreal
        # rewrites those axes on the way in, so the box is what tells the
        # importer's conventions from the ones the impostor material assumes.
        "boundsMin": [round(value, 4) for value in minimum],
        "boundsMax": [round(value, 4) for value in maximum],
        "heightMeters": round(maximum[1] - min(minimum[1], 0.0), 3),
    }


def convert(arguments):
    output_directory = os.path.join(arguments.output, arguments.name)
    os.makedirs(output_directory, exist_ok=True)

    materials = []
    levels = [convert_level(arguments, level, output_directory, materials)
              for level in range(arguments.levels)]

    manifest = {
        "format": "massif-flora-model",
        "version": 1,
        "name": arguments.name,
        "species": arguments.species,
        "source": {"preset": arguments.preset, "seed": arguments.seed, "steps": arguments.steps},
        "heightMeters": max(level["heightMeters"] for level in levels),
        "alphaCutoff": arguments.alpha_cutoff,
        "materials": materials,
        "levels": levels,
    }
    manifest_path = os.path.join(output_directory, arguments.name + ".flora.json")
    with open(manifest_path, "w", encoding="utf-8") as text:
        json.dump(manifest, text, indent=1)

    print(f"wrote {manifest_path}")
    for level in levels:
        print("  LOD{level}  {vertices:6d} vertices  {triangles:6d} triangles  "
              "screen size {screenSize}".format(**level))


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True, help="asset name, e.g. PiceaAbies")
    parser.add_argument("--species", required=True, help="species name for the manifest")
    parser.add_argument("--source", required=True, help="directory holding the flora OBJ levels")
    parser.add_argument("--textures", required=True,
                        help="the flora repository's textures directory")
    parser.add_argument("--prefix", default="spruce_lod", help="OBJ file name before the level")
    parser.add_argument("--impostor-prefix", default=None,
                        help="baked atlas prefix (default <source>/spruce-impostor)")
    parser.add_argument("--levels", type=int, default=4, help="number of detail levels")
    parser.add_argument("--seed", type=int, required=True, help="flora seed the tree grew from")
    parser.add_argument("--steps", type=int, required=True, help="growth steps")
    parser.add_argument("--preset", default="presets/spruce.json", help="flora species preset")
    parser.add_argument("--alpha-cutoff", type=float, default=0.5,
                        help="foliage card cutout threshold")
    parser.add_argument("--screen-sizes", type=float, nargs="+",
                        default=[1.0, 0.4, 0.15, 0.05],
                        help="screen size each level takes over at")
    parser.add_argument("--output", default=DEFAULT_OUTPUT_ROOT,
                        help="directory the model directory is written under")
    arguments = parser.parse_args()
    if arguments.impostor_prefix is None:
        arguments.impostor_prefix = os.path.join(arguments.source, "spruce-impostor")
    if len(arguments.screen_sizes) < arguments.levels:
        raise SystemExit("one screen size per level is required")
    return arguments


convert(parse_arguments())
