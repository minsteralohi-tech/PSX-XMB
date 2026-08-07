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

2. Negates Y, because PS1 screen space counts downward, AND negates Z.

   The Z negation is the one that is easy to get wrong, and a previous
   revision did: glTF is right-handed with +Z toward the viewer, so its front
   view has the camera at +Z looking down -Z. The GTE's Z is depth away from
   the camera. Carrying glTF Z straight across therefore views the model from
   BEHIND, which reads as a mirror image - the swoosh sweeps the wrong way and
   the P leans the wrong side.

   Negating Z also reverses handedness, so BAKE_YAW's sign flips with it (see
   below) and the exported winding flips again on top of the NCLIP swap.

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

5. Thickens each sub-mesh along its own thinnest axis. The source geometry is
   a 2-unit-thick relief on a 72-unit-wide logo, which reads as a sticker; the
   BIOS logo has a real extruded edge. The axis differs per part - the P is
   thin in Z, the three swoosh slabs are thin in Y - so this finds each
   sub-mesh's thinnest extent and scales about that sub-mesh's own mid-plane.

6. Bakes flat per-face shading, ON THE EXTRUDED EDGES ONLY. The faces are
   drawn as flat GPU triangles with no lighting, so without something the
   extruded sides are the same colour as the front and the whole thing reads
   flat - but shading everything washes the front faces out and loses the
   logo's flat-colour look.

   So each face is classified first: its normal is compared against its own
   sub-mesh's extrusion axis. Parallel means it is one of the two flat faces,
   and it keeps its colour untouched. Perpendicular means it is an extruded
   rim, and it gets one Lambert term against a fixed light. Roughly 119 of the
   278 faces are flat.

   Because it is baked in the resting pose, the lighting turns with the model
   during the intro's swing-in rather than staying fixed in the world. At this
   size and speed that is invisible, and it costs nothing at runtime.

7. Replaces the materials' colours. The GLB's baseColorFactors are linear and
   convert to garishly bright sRGB; the flat PlayStation palette is what the
   BIOS screen actually shows.

   MATERIAL ORDER IS THE FILE'S OWN, and it matters. An earlier revision
   swapped materials 1 and 3 from a misread of the BIOS screenshot, which put
   the red P against the blue end of the swoosh instead of the yellow one. The
   geometry settles it: the P occupies z 17.5..19.5, inside material 1's slab
   (6.4..20.8), so material 1 is the end the P stands on - and that end is
   yellow.
"""

import json
import math
import os
import struct
import sys

# Resting pose, in degrees: yaw about the vertical, then pitch about the
# horizontal. Tuned by eye against the BIOS reference screen. BAKE_YAW is
# negative because Z is negated above - the mirror correction reverses which
# way a positive yaw turns.
BAKE_YAW   = -22.0
BAKE_PITCH =  28.0

# Model units per GLB unit. The posed model comes out ~328 units across, which
# with the intro's camera gives roughly 110 screen pixels.
SCALE = 4.0

# How much to multiply each sub-mesh's thinnest extent by. 2.6 takes the
# source's 2-unit relief to ~5.2, which reads like the BIOS logo's extrusion.
THICKNESS = 2.6

# Edge shading. LIGHT points from the surface toward the light, in the posed
# frame: X right, Y DOWN (PS1 screen space), Z into the screen - so this is
# above, left and in front.
#
# FLAT_DOT is the classifier: a face whose normal is within this much of its
# sub-mesh's extrusion axis counts as one of the two flat faces and is left at
# full colour. Everything else is an extruded rim and gets RIM_BASE plus
# RIM_RANGE of Lambert.
LIGHT     = (-0.35, -0.60, -0.72)
FLAT_DOT  = 0.60
RIM_BASE  = 0.50
RIM_RANGE = 0.34

# Per-material RGB, in the GLB's own order: the P, then the swoosh from the
# slab the P stands on to the far one. See point 7 above before reordering.
COLORS = [
    (230,   0,  18),   # red     - the P
    (250, 196,   0),   # yellow  - the slab the P stands on
    (  0, 166, 148),   # teal
    (  0,  88, 168),   # blue    - the far end
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
    owner = []                        # sub-mesh each vertex came from

    for mi, mesh in enumerate(gltf["meshes"]):
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
            owner.append(mi)

        idx = struct.unpack_from(f"<{acc_i['count']}I", data, off_i)
        for i in range(0, len(idx), 3):
            faces.append((
                base + idx[i], base + idx[i + 1], base + idx[i + 2],
                prim["material"],
            ))

    # Thicken: per sub-mesh, expand its own thinnest axis about its own
    # mid-plane. Done before centring so each part keeps its position. The
    # axis is kept - the shading pass needs it to tell flat faces from rims.
    mesh_axis = []

    for mi in range(len(gltf["meshes"])):
        mine = [i for i, o in enumerate(owner) if o == mi]
        lo = [min(verts[i][a] for i in mine) for a in range(3)]
        hi = [max(verts[i][a] for i in mine) for a in range(3)]
        axis = min(range(3), key=lambda a: hi[a] - lo[a])
        mid = (lo[axis] + hi[axis]) / 2
        mesh_axis.append(axis)

        for i in mine:
            v = list(verts[i])
            v[axis] = mid + (v[axis] - mid) * THICKNESS
            verts[i] = tuple(v)

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

    def pose(x, y, z):
        x1 =  cyy * x + sy * z
        z1 = -sy  * x + cyy * z
        return (x1, cp * y - sp * z1, sp * y + cp * z1)

    for vx, vy, vz in verts:
        posed.append(pose(
            (vx - cx) * SCALE,
            -(vy - cy) * SCALE,
            -(vz - cz) * SCALE,     # Z negated: see point 2 above
        ))

    # Each sub-mesh's extrusion axis, carried through the same transform, so
    # the shading pass can compare face normals against it.
    axis_dir = []
    for mi, a in enumerate(mesh_axis):
        v = [0.0, 0.0, 0.0]
        v[a] = -1.0 if a else 1.0       # Y and Z were negated, X was not
        if a == 2:
            v[a] = -1.0
        axis_dir.append(pose(*v))

    # Re-centre after posing so the swing turns about the on-screen centre.
    bx = (min(p[0] for p in posed) + max(p[0] for p in posed)) / 2
    by = (min(p[1] for p in posed) + max(p[1] for p in posed)) / 2
    bz = (min(p[2] for p in posed) + max(p[2] for p in posed)) / 2
    posed = [(p[0] - bx, p[1] - by, p[2] - bz) for p in posed]

    # Edge shading, folded into each face's colour.
    ln = math.sqrt(sum(c * c for c in LIGHT))
    light = tuple(c / ln for c in LIGHT)
    shaded = []
    flat_count = 0

    for v0, v1, v2, mat in faces:
        a, b, c = posed[v0], posed[v1], posed[v2]
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        w = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        n = (
            u[1] * w[2] - u[2] * w[1],
            u[2] * w[0] - u[0] * w[2],
            u[0] * w[1] - u[1] * w[0],
        )
        nl = math.sqrt(sum(t * t for t in n)) or 1.0
        n = [t / nl for t in n]

        # Faces the camera can see point at it, i.e. -Z. Flip the ones that
        # do not, so the shading is computed for the side actually shown.
        if n[2] > 0:
            n = [-t for t in n]

        ax = axis_dir[owner[v0]]
        if abs(sum(p * q for p, q in zip(n, ax))) >= FLAT_DOT:
            k = 1.0                      # flat face - leave the colour alone
            flat_count += 1
        else:
            lam = max(0.0, sum(p * q for p, q in zip(n, light)))
            k = RIM_BASE + RIM_RANGE * lam

        shaded.append(tuple(
            min(255, round(ch * k)) for ch in COLORS[mat]
        ))

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
    for (v0, v1, v2, _mat), (r, g, b) in zip(faces, shaded):
        # Winding is left alone: the NCLIP swap and the Z mirror's handedness
        # flip cancel out. Swapping here as well would cull every face.
        lines.append(f"\t{{ {v0}, {v1}, {v2}, {r}, {g}, {b} }},")
    lines += ["};", "", "#endif // PS_LOGO_BIOS_H", ""]

    dst = os.path.join(root, "src", "main", "model", "ps_logo_bios.h")
    with open(dst, "w", newline="\n") as f:
        f.write("\n".join(lines))

    print(f"{dst}: {len(posed)} vertices, {len(faces)} faces, "
          f"{flat_count} of them flat")


if __name__ == "__main__":
    main()
