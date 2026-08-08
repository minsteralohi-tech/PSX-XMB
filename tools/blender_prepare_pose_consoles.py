"""Convert the two supplied low-poly PlayStation consoles for the pose tool.

Run with Blender in background mode:

    blender --background --python tools/blender_prepare_pose_consoles.py

Both originals are already below the existing 560-triangle PS-logo budget, so
their geometry is preserved in full.  Their embedded texture is sampled once
per triangle into flat PS1 colours; this keeps the pose preview tiny and avoids
adding UV/texture state to the existing untextured GTE renderer.
"""

import pathlib

import bmesh
import bpy


ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET_EXTENT = 1320.0

MODELS = (
    {
        "source": ROOT / "assets" / "playstation_model_source" / "PSX.glb",
        "output": ROOT / "src" / "main" / "model" / "ps_console_classic.h",
        "guard": "PS_CONSOLE_CLASSIC_H",
        "macro": "PS_CONSOLE_CLASSIC",
        "symbol": "psConsoleClassic",
        "label": "original PlayStation console",
    },
    {
        "source": ROOT / "assets" / "ps_one_pixel.glb",
        "output": ROOT / "src" / "main" / "model" / "ps_console_psone.h",
        "guard": "PS_CONSOLE_PSONE_H",
        "macro": "PS_CONSOLE_PSONE",
        "symbol": "psConsolePSone",
        "label": "PS one console",
    },
)


def linear_to_srgb(value):
    value = max(0.0, min(1.0, value))
    if value <= 0.0031308:
        return value * 12.92
    return 1.055 * (value ** (1.0 / 2.4)) - 0.055


def base_color_image(material):
    if not material or not material.use_nodes:
        return None
    for node in material.node_tree.nodes:
        if node.type != "BSDF_PRINCIPLED":
            continue
        socket = node.inputs.get("Base Color")
        if socket and socket.is_linked:
            source = socket.links[0].from_node
            if source.type == "TEX_IMAGE":
                return source.image
    return next(
        (node.image for node in material.node_tree.nodes
         if node.type == "TEX_IMAGE" and node.image),
        None,
    )


def fallback_color(material):
    if material and material.use_nodes:
        node = next(
            (n for n in material.node_tree.nodes if n.type == "BSDF_PRINCIPLED"),
            None,
        )
        if node:
            rgba = node.inputs["Base Color"].default_value
            return tuple(round(linear_to_srgb(rgba[i]) * 255) for i in range(3))
    return (160, 160, 160)


def sample_image(image, u, v, fallback):
    if image is None or image.size[0] <= 0 or image.size[1] <= 0:
        return fallback
    width, height = image.size
    x = min(width - 1, max(0, int((u % 1.0) * (width - 1) + 0.5)))
    y = min(height - 1, max(0, int((v % 1.0) * (height - 1) + 0.5)))
    offset = (y * width + x) * 4
    return tuple(
        min(255, max(0, round(linear_to_srgb(image.pixels[offset + i]) * 255)))
        for i in range(3)
    )


def convert(spec):
    source = spec["source"]
    if not source.exists():
        raise RuntimeError(f"missing source GLB: {source}")

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(source))
    objects = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    if not objects:
        raise RuntimeError(f"{source.name} contains no mesh objects")

    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    if len(objects) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    triangulate = obj.modifiers.new("PS1 triangulate", "TRIANGULATE")
    bpy.ops.object.modifier_apply(modifier=triangulate.name)
    mesh = obj.data

    bm = bmesh.new()
    bm.from_mesh(mesh)
    # Join coincident glTF seam vertices before repairing winding. BMesh keeps
    # UVs on face loops, so the per-triangle texture sampling still works.
    weld_distance = max(obj.dimensions) * 0.000001
    bmesh.ops.remove_doubles(bm, verts=list(bm.verts), dist=weld_distance)
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()
    mesh.calc_loop_triangles()

    uv_layer = mesh.uv_layers.active
    images = [base_color_image(slot.material) for slot in obj.material_slots]
    fallbacks = [fallback_color(slot.material) for slot in obj.material_slots]

    coords = [tuple(obj.matrix_world @ vertex.co) for vertex in mesh.vertices]
    lo = [min(v[axis] for v in coords) for axis in range(3)]
    hi = [max(v[axis] for v in coords) for axis in range(3)]
    centre = [(lo[axis] + hi[axis]) * 0.5 for axis in range(3)]
    extent = max(hi[axis] - lo[axis] for axis in range(3))
    scale = TARGET_EXTENT / extent

    quantized = [
        (
            round((x - centre[0]) * scale),
            round(-(z - centre[2]) * scale),
            round(-(y - centre[1]) * scale),
        )
        for x, y, z in coords
    ]

    unique = []
    unique_map = {}
    remap = []
    for vertex in quantized:
        index = unique_map.get(vertex)
        if index is None:
            index = len(unique)
            unique_map[vertex] = index
            unique.append(vertex)
        remap.append(index)

    faces = []
    for tri in mesh.loop_triangles:
        indices = [remap[i] for i in tri.vertices]
        if len(set(indices)) < 3:
            continue
        a, b, c = (unique[i] for i in indices)
        cross = (
            (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]),
            (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]),
            (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]),
        )
        if cross == (0, 0, 0):
            continue

        material_index = mesh.polygons[tri.polygon_index].material_index
        image = images[material_index] if material_index < len(images) else None
        fallback = fallbacks[material_index] if material_index < len(fallbacks) else (160, 160, 160)
        if uv_layer is not None:
            u = sum(uv_layer.data[i].uv.x for i in tri.loops) / 3.0
            v = sum(uv_layer.data[i].uv.y for i in tri.loops) / 3.0
            color = sample_image(image, u, v, fallback)
        else:
            color = fallback

        # The Blender-to-GTE axis map reverses handedness.
        faces.append((indices[0], indices[2], indices[1], *color))

    guard = spec["guard"]
    macro = spec["macro"]
    symbol = spec["symbol"]
    lines = [
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        '#include "main/model/ps_logo_model.h"',
        "",
        f"/* Generated from {source.relative_to(ROOT).as_posix()}.",
        f" * Complete {spec['label']}: {len(faces)} triangles, texture sampled to flat face colours. */",
        f"#define {macro}_VERTEX_COUNT {len(unique)}",
        f"#define {macro}_FACE_COUNT {len(faces)}",
        "",
        f"static const PSLogoVertex {symbol}Vertices[{macro}_VERTEX_COUNT] = {{",
    ]
    lines.extend(f"\t{{ {x}, {y}, {z}, 0 }}," for x, y, z in unique)
    lines.extend(["};", "", f"static const PSLogoFace {symbol}Faces[{macro}_FACE_COUNT] = {{"])
    lines.extend(
        f"\t{{ {a}, {b}, {c}, {r}, {g}, {blue} }},"
        for a, b, c, r, g, blue in faces
    )
    lines.extend(["};", "", f"#endif // {guard}", ""])
    spec["output"].write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"{spec['output'].name}: {len(faces)} triangles, {len(unique)} quantized vertices")


def main():
    for spec in MODELS:
        convert(spec)


if __name__ == "__main__":
    main()
