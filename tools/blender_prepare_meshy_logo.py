"""Build a clean PS1-friendly header from the supplied Meshy logo GLB.

Run with Blender in background mode:

    blender --background --python tools/blender_prepare_meshy_logo.py

The original GLB stays in assets/ as the editable source.  Its ~60k triangles
cannot fit or render sensibly on a stock PS1, so this applies a decimation,
repairs the face winding and emits one uniform cyan colour.  The source texture
used fractured black/cyan face detail that read as holes after decimation; it is
deliberately not carried into this solid-colour version.
"""

import pathlib

import bmesh
import bpy


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets" / "meshy_playstation_logo.glb"
OUTPUT = ROOT / "src" / "main" / "model" / "ps_logo_meshy.h"

TARGET_TRIANGLES = 560
TARGET_EXTENT = 1320.0
SOLID_CYAN = (0, 182, 244)


def main():
    if not SOURCE.exists():
        raise RuntimeError(f"missing source GLB: {SOURCE}")

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(SOURCE))

    objects = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    if not objects:
        raise RuntimeError("GLB contains no mesh objects")

    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    if len(objects) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    mesh = obj.data
    # glTF keeps UV/normal seams as duplicate vertices. Decimating those
    # disconnected copies independently was the real source of the fractured
    # silhouette. Weld only truly coincident points first; loop UV data is not
    # needed by this solid-colour output.
    bm = bmesh.new()
    bm.from_mesh(mesh)
    weld_distance = max(obj.dimensions) * 0.000001
    bmesh.ops.remove_doubles(bm, verts=list(bm.verts), dist=weld_distance)
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()
    mesh.calc_loop_triangles()
    source_triangles = len(mesh.loop_triangles)
    if source_triangles > TARGET_TRIANGLES:
        modifier = obj.modifiers.new("PS1 triangle budget", "DECIMATE")
        modifier.decimate_type = "COLLAPSE"
        modifier.ratio = TARGET_TRIANGLES / source_triangles
        modifier.use_collapse_triangulate = True
        bpy.ops.object.modifier_apply(modifier=modifier.name)

    triangulate = obj.modifiers.new("PS1 triangulate", "TRIANGULATE")
    bpy.ops.object.modifier_apply(modifier=triangulate.name)

    mesh = obj.data
    # Decimation can leave individual triangles with reversed winding.  The
    # PS1 renderer correctly culls back faces, so those bad triangles appeared
    # as black cracks.  Recalculate the whole connected surface consistently
    # before quantising it for the GTE.
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()
    mesh.calc_loop_triangles()

    coords = [tuple(obj.matrix_world @ vertex.co) for vertex in mesh.vertices]
    lo = [min(v[axis] for v in coords) for axis in range(3)]
    hi = [max(v[axis] for v in coords) for axis in range(3)]
    centre = [(lo[axis] + hi[axis]) * 0.5 for axis in range(3)]
    # Blender imports glTF's Y-up scene as Z-up. The artwork therefore spans
    # Blender X/Z and its shallow extrusion is Blender Y.
    extent = max(hi[0] - lo[0], hi[2] - lo[2])
    scale = TARGET_EXTENT / extent

    quantized = []
    for x, y, z in coords:
        # Map Blender X/Z to screen X/Y; its shallow Y extrusion becomes GTE
        # depth. Screen-space Y points down.
        quantized.append((
            round((x - centre[0]) * scale),
            round(-(z - centre[2]) * scale),
            round(-(y - centre[1]) * scale),
        ))

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
        cross_x = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1])
        cross_y = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2])
        cross_z = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
        if cross_x == 0 and cross_y == 0 and cross_z == 0:
            continue

        # Negating screen Y reverses the projected winding. Swap the final two
        # indices so GTE NCLIP retains the front-facing triangles.
        faces.append((indices[0], indices[2], indices[1], *SOLID_CYAN))

    if len(unique) > 65535:
        raise RuntimeError("decimated mesh still exceeds 16-bit vertex indices")
    if any(abs(component) > 32767 for vertex in unique for component in vertex):
        raise RuntimeError("normalised mesh exceeds signed GTE coordinates")

    lines = [
        "#ifndef PS_LOGO_MESHY_H",
        "#define PS_LOGO_MESHY_H",
        "",
        "#include \"main/model/ps_logo_model.h\"",
        "",
        "/* Generated from assets/meshy_playstation_logo.glb.",
        f" * Source: {source_triangles} triangles; PS1 render mesh: {len(faces)} triangles.",
        " * Face winding is repaired and all faces use one solid cyan colour. */",
        f"#define PS_LOGO_MESHY_VERTEX_COUNT {len(unique)}",
        f"#define PS_LOGO_MESHY_FACE_COUNT {len(faces)}",
        "",
        "static const PSLogoVertex psLogoMeshyVertices[PS_LOGO_MESHY_VERTEX_COUNT] = {",
    ]
    lines.extend(f"\t{{ {x}, {y}, {z}, 0 }}," for x, y, z in unique)
    lines.extend([
        "};",
        "",
        "static const PSLogoFace psLogoMeshyFaces[PS_LOGO_MESHY_FACE_COUNT] = {",
    ])
    lines.extend(
        f"\t{{ {a}, {b}, {c}, {r}, {g}, {blue} }},"
        for a, b, c, r, g, blue in faces
    )
    lines.extend(["};", "", "#endif // PS_LOGO_MESHY_H", ""])

    OUTPUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(
        f"{OUTPUT.name}: {source_triangles} -> {len(faces)} triangles, "
        f"{len(unique)} quantized vertices"
    )


if __name__ == "__main__":
    main()
