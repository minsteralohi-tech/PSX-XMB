#!/usr/bin/env python3
"""Convert the PlayStation logo GLB into the boot sequence's model header.

Reads assets/playstation_logo.glb and writes
src/main/model/ps_logo_bios.h - the model the intro's PlayStation screen
draws. Regenerate with:

    python3 tools/glb2pslogo.py

No dependencies beyond the standard library: GLB is a JSON chunk followed by a
binary chunk, and the only accessors needed here are float POSITION and uint32
indices.

WHAT THIS DOES BEYOND UNPACKING VERTICES
----------------------------------------

1. Applies the root node's matrix. Sketchfab exports Z-up FBX to Y-up by
   putting a Y/Z-flipping correction on the scene root, and it is not
   optional - skip it and the logo is upside down and inside out.

2. Negates Y, because PS1 screen space counts downward.

3. Bakes the resting pose in (BAKE_YAW/BAKE_PITCH below), so the exported
   vertices are already in the BIOS screen's three-quarter view and the
   runtime only animates one angle back to zero. Composing that pose from
   three runtime angles instead would mean relying on cosmosRotate()'s axis
   naming, which does not match its parameter names - its "yaw" turns about Z
   and its "roll" about X. Baking it makes the resting orientation a property
   of the data, which is verifiable offline, rather than of three call sites.

4. Reverses winding. GTE_CMD_NCLIP keeps faces whose MAC0 is positive; the
   screen-space cross product that picked this pose offline has the opposite
   sign convention. Swapping vertices 1 and 2 reconciles them - without it the
   logo renders inside out, showing only its back faces.

5. Replaces the materials' colours. The GLB's baseColorFactors are linear and
   convert to garishly bright sRGB; the flat PlayStation palette is what the
   BIOS screen actually shows. Material 1 is blue and material 3 yellow, not
   the reverse: the swoosh slab nearest the camera is its yellow end.
"""

import json
import math
import os
import struct
import sys

# Resting pose, in degrees: yaw about the vertical, then pitch about the
# horizontal. Tuned by eye against the BIOS reference screen.
BAKE_YAW   = 22.0
BAKE_PITCH = 28.0

# Model units per GLB unit. The posed model comes out 326 units across, which
# with the intro's camera gives roughly 110 screen pixels.
SCALE = 4.0

# Per-material RGB: the P, then the swoosh from its far slab to its near one.
COLORS = [
    (230,   0,  18),   # red
    (  0,  90, 170),   # blue    (far)
    (  0, 168, 150),   # teal
    (252, 200,   0),   # yellow  (near)
]


def load_glb(path):
    with open(path, "rb") as f:
        data = f.read()

    magic, _version, _length = struct.unpack_from("<4sII", data, 0)
    if magic != b"glTF":
        sys.exit(f"{path}: not a GLB")

    json_len, json_tag = struct.unpack_from("<I4s", data, 12)
    if json_tag != b"JSON":
        sys.exit(f"{path}: first chunk is not JSON")

    gltf = json.loads(data[20:20 + json_len])
    bin_off = 20 + json_len + 8    # skip the BIN chunk's own header
    return gltf, data, bin_off


def accessor_offset(gltf, acc, bin_off):
    view = gltf["bufferViews"][acc["bufferView"]]
    return bin_off + view.get("byteOffset", 0) + acc.get("byteOffset", 0)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    gltf, data, bin_off = load_glb(
        os.path.join(root, "assets", "playstation_logo.glb")
    )

    m = gltf["nodes"][0]["matrix"]     # column-major
    verts = []                        # (x, y, z), root transform applied
    faces = []                        # (v0, v1, v2, material)

    for mesh in gltf["meshes"]:
        prim = mesh["primitives"][0]
        acc_p = gltf["accessors"][prim["attributes"]["POSITION"]]
        acc_i = gltf["accessors"][prim["indices"]]
        off_p = accessor_offset(gltf, acc_p, bin_off)
        off_i = accessor_offset(gltf, acc_i, bin_off)
        base  = len(verts)

        for i in range(acc_p["count"]):
            x, y, z = struct.unpack_from("<3f", data, off_p + i * 12)
            verts.append((
                m[0] * x + m[4] * y + m[8]  * z + m[12],
                m[1] * x + m[5] * y + m[9]  * z + m[13],
                m[2] * x + m[6] * y + m[10] * z + m[14],
            ))

        idx = struct.unpack_from(f"<{acc_i['count']}I", data, off_i)
        for i in range(0, len(idx), 3):
            faces.append((
                base + idx[i], base + idx[i + 1], base + idx[i + 2],
                prim["material"],
            ))

    # Centre, flip Y for screen-down, scale.
    xs = [v[0] for v in verts]
    ys = [v[1] for v in verts]
    zs = [v[2] for v in verts]
    cx = (min(xs) + max(xs)) / 2
    cy = (min(ys) + max(ys)) / 2
    cz = (min(zs) + max(zs)) / 2

    posed = []
    sy, cyy = math.sin(math.radians(BAKE_YAW)),   math.cos(math.radians(BAKE_YAW))
    sp, cp  = math.sin(math.radians(BAKE_PITCH)), math.cos(math.radians(BAKE_PITCH))

    for vx, vy, vz in verts:
        x = (vx - cx) * SCALE
        y = -(vy - cy) * SCALE
        z = (vz - cz) * SCALE

        x1 =  cyy * x + sy * z
        z1 = -sy  * x + cyy * z
        y2 =  cp  * y - sp * z1
        z2 =  sp  * y + cp * z1
        posed.append((x1, y2, z2))

    # Re-centre after posing so the swing turns about the on-screen centre.
    bx = (min(p[0] for p in posed) + max(p[0] for p in posed)) / 2
    by = (min(p[1] for p in posed) + max(p[1] for p in posed)) / 2
    bz = (min(p[2] for p in posed) + max(p[2] for p in posed)) / 2
    posed = [(p[0] - bx, p[1] - by, p[2] - bz) for p in posed]

    lines = [
        "#ifndef PS_LOGO_BIOS_H",
        "#define PS_LOGO_BIOS_H",
        "",
        "/*",
        " * PlayStation logo, converted from assets/playstation_logo.glb by",
        " * tools/glb2pslogo.py. Generated - do not hand-edit; see that script",
        " * for what it bakes in and why.",
        " *",
        " * This is NOT the model in ps_logo_model.h. That one is an older,",
        " * coarser conversion still used by the TEST logo theme and PS4 v2;",
        " * this one is the real logo and is what the boot sequence draws.",
        " */",
        "",
        '#include "main/model/ps_logo_model.h"',
        "",
        f"#define PS_LOGO_BIOS_VERTEX_COUNT {len(posed)}",
        f"#define PS_LOGO_BIOS_FACE_COUNT {len(faces)}",
        "",
        "static const PSLogoVertex psLogoBiosVertices"
        "[PS_LOGO_BIOS_VERTEX_COUNT] = {",
    ]
    for x, y, z in posed:
        lines.append(f"\t{{ {round(x)}, {round(y)}, {round(z)}, 0 }},")
    lines += ["};", "", "static const PSLogoFace psLogoBiosFaces"
              "[PS_LOGO_BIOS_FACE_COUNT] = {"]
    for v0, v1, v2, mat in faces:
        r, g, b = COLORS[mat]
        # v2 and v1 swapped: see the winding note above.
        lines.append(f"\t{{ {v0}, {v2}, {v1}, {r}, {g}, {b} }},")
    lines += ["};", "", "#endif // PS_LOGO_BIOS_H", ""]

    dst = os.path.join(root, "src", "main", "model", "ps_logo_bios.h")
    with open(dst, "w", newline="\n") as f:
        f.write("\n".join(lines))

    print(f"{dst}: {len(posed)} vertices, {len(faces)} faces")


if __name__ == "__main__":
    main()
