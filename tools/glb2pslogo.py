#!/usr/bin/env python3
"""Convert the PlayStation logo GLBs into the boot sequence's model headers.

Reads the GLBs listed in MODELS below and writes one header each into
src/main/model/. Regenerate with:

    python3 tools/glb2pslogo.py

No dependencies beyond the standard library: GLB is a JSON chunk followed by a
binary chunk, and the only accessors needed here are float POSITION and
uint16/uint32 indices.

TWO MODELS, because they are genuinely different shapes rather than different
exports of one:

  A  ps_logo_bios.h   a shallow relief - the flat artwork extruded a little,
                      everything in one slab
  B  ps_logo_bios2.h  built the way the real BIOS logo is: the P standing
                      upright on a swoosh lying flat on the ground plane

The intro picks between them with PS_LOGO_MODEL in intro_ps1.c, and the pose
tool on the boot menu switches live with L1.

WHAT THIS DOES BEYOND UNPACKING VERTICES
----------------------------------------

1. Applies the root node's matrix. Sketchfab exports Z-up FBX to Y-up by
   putting a correction on the scene root, and it is not optional - skip it
   and the logo is upside down and inside out.

2. Negates Y, because PS1 screen space counts downward, and optionally negates
   Z (`mirror_z`).

   The Z negation is the one that is easy to get wrong, and an earlier
   revision did: glTF is right-handed with +Z toward the viewer, so its front
   view has the camera at +Z looking down -Z, while the GTE's Z is depth away
   from the camera. Carrying glTF Z straight across therefore views the model
   from BEHIND, which reads as a mirror image. Whether a given file needs it
   depends on what its own root correction already did, so it is per-model and
   was settled by rendering both ways rather than by reasoning.

   Negating Z also reverses handedness, so `bake_yaw` flips sign with it and
   the exported winding flips again on top of the NCLIP swap - which is why
   model A ends up NOT swapped and model B does.

3. Bakes the resting pose in (`bake_yaw` / `bake_pitch`), so the exported
   vertices are already in the BIOS screen's three-quarter view and the
   runtime only interpolates from the intro's start pose to zero. Composing
   the pose from runtime angles instead would mean relying on cosmosRotate()'s
   axis naming, which does not match its parameter names - its "yaw" turns
   about Z and its "roll" about X. Baking it makes the resting orientation a
   property of the data, verifiable offline.

4. Separates the two parts that interpenetrate (`shift_*`).

   THIS IS NOT COSMETIC. In model A the P sits inside the yellow slab; in
   model B the P's foot sinks into the swoosh plate. xmb_bg.c draws these with
   a software painter's algorithm, bucketing whole faces by depth, and no
   per-face sort can resolve intersecting geometry - the two tear into each
   other along the intersection line. That was the stray yellow wedge on the
   P's stem. Nudging the part clear removes the intersection rather than
   papering over it; the fraction of a pixel it moves is invisible.

5. Reverses winding when needed. GTE_CMD_NCLIP keeps faces whose MAC0 is
   positive; the screen-space cross product that picked these poses offline
   has the opposite sign convention. See point 2 for why that interacts with
   mirror_z.

6. Assigns flat colours per sub-mesh. No shading: it was tried, on the
   extruded rims only, and it muddied what should read as flat colour. The
   GLBs' own material colours are no help - A's are linear factors that
   convert to garish sRGB, and B's have none at all - so the sub-mesh order
   was identified by rendering each in a debug colour and the flat PlayStation
   palette assigned by hand.
"""

import json
import math
import os
import struct
import sys

RED    = (230,   0,  18)
YELLOW = (250, 196,   0)
TEAL   = (  0, 166, 148)
BLUE   = (  0,  88, 168)

MODELS = [
    {
        "glb":        "playstation_logo.glb",
        "header":     "ps_logo_bios.h",
        "prefix":     "psLogoBios",
        "macro":      "PS_LOGO_BIOS",
        "note":       "logo A - the Sketchfab relief model",
        "mirror_z":   True,
        "bake_yaw":   -22.0,
        "bake_pitch":  28.0,
        "scale":        4.0,
        # sub-mesh order: the P, then the swoosh from the end the P stands on
        "colors":     [RED, YELLOW, TEAL, BLUE],
        # Push the P (sub-mesh 0) forward in Z until its front face is flush
        # with the yellow slab's at 20.8 - touching, but not intersecting.
        "shift":      (0, 2, 1.3),
        # Grow the swoosh (sub-meshes 1-3) about the P's centre
        "swoosh":     ([1, 2, 3], 0, 1.6),
    },
    {
        "glb":        "playstation_logo_bios.glb",
        "header":     "ps_logo_bios2.h",
        "prefix":     "psLogoBios2",
        "macro":      "PS_LOGO_BIOS2",
        "note":       "logo B - the system-BIOS model",
        # This model DOES need the Z mirror, same as A. Without it its P comes
        # out with a cramped, wrong-way bowl - obvious once the P is rendered
        # on its own, easy to miss with the swoosh drawn around it.
        "mirror_z":   True,
        "bake_yaw":   -25.0,   # sign flipped with mirror_z, as in A
        "bake_pitch":  12.0,
        "scale":       13.0,
        # sub-mesh order here is blue, teal, the P, yellow - identified by
        # rendering each one in a debug colour, not guessed
        "colors":     [BLUE, TEAL, RED, YELLOW],
        # the P (sub-mesh 2) stands ON the swoosh plate; lift its foot clear
        "shift":      (2, 1, 0.9),
        # Grow the swoosh (sub-meshes 0, 1 and 3) about the P's centre
        "swoosh":     ([0, 1, 3], 2, 1.6),
    },
]

# Every model is normalised to this posed extent, so they draw the same size
# and the intro's PS_LOGO_*_CAM_Z stays valid when switching between them.
TARGET_EXTENT = 330.0


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


def convert(spec, assets, models_dir):
    gltf, data, bin_off = load_glb(os.path.join(assets, spec["glb"]))

    m = gltf["nodes"][0].get(
        "matrix", [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    )
    verts, faces, owner = [], [], []

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

        fmt = "H" if acc_i["componentType"] == 5123 else "I"
        idx = struct.unpack_from(f"<{acc_i['count']}{fmt}", data, off_i)
        for i in range(0, len(idx), 3):
            faces.append((base + idx[i], base + idx[i + 1], base + idx[i + 2],
                          mi))

    # Separate the interpenetrating parts. See point 4 above - load-bearing.
    smesh, saxis, samount = spec["shift"]
    for i, o in enumerate(owner):
        if o == smesh:
            v = list(verts[i])
            v[saxis] += samount
            verts[i] = tuple(v)

    # Enlarge the swoosh about the P's centre. Lying flat and foreshortened it
    # reads far smaller against the P than the reference screen shows; growing
    # it about the P keeps the P planted mid-swoosh and pushes the swoosh
    # outward. X and Z only - both models' swooshes are thin in Y, and scaling
    # that axis would just make the plates chunkier.
    swoosh_meshes, pivot_mesh, factor = spec["swoosh"]
    pivot_v = [verts[i] for i, o in enumerate(owner) if o == pivot_mesh]
    pivot = [(min(v[a] for v in pivot_v) + max(v[a] for v in pivot_v)) / 2
             for a in range(3)]

    for i, o in enumerate(owner):
        if o in swoosh_meshes:
            v = list(verts[i])
            for a in (0, 2):
                v[a] = pivot[a] + (v[a] - pivot[a]) * factor
            verts[i] = tuple(v)

    lo = [min(v[a] for v in verts) for a in range(3)]
    hi = [max(v[a] for v in verts) for a in range(3)]
    ctr = [(lo[a] + hi[a]) / 2 for a in range(3)]

    mz = -1.0 if spec["mirror_z"] else 1.0
    sy, cy = math.sin(math.radians(spec["bake_yaw"])), \
             math.cos(math.radians(spec["bake_yaw"]))
    sp, cp = math.sin(math.radians(spec["bake_pitch"])), \
             math.cos(math.radians(spec["bake_pitch"]))

    posed = []
    for vx, vy, vz in verts:
        x =  (vx - ctr[0]) * spec["scale"]
        y = -(vy - ctr[1]) * spec["scale"]
        z =  (vz - ctr[2]) * spec["scale"] * mz

        x1 =  cy * x + sy * z
        z1 = -sy * x + cy * z
        posed.append((x1, cp * y - sp * z1, sp * y + cp * z1))

    # Re-centre after posing so the intro's swing turns about the same point
    # the model is drawn at.
    blo = [min(p[a] for p in posed) for a in range(3)]
    bhi = [max(p[a] for p in posed) for a in range(3)]
    posed = [tuple(p[a] - (blo[a] + bhi[a]) / 2 for a in range(3))
             for p in posed]

    # Normalise to a fixed posed extent. Enlarging the swoosh grows the whole
    # bounding box, which would silently change how big the logo draws and
    # invalidate the pose tool's CAM Z; this keeps the models interchangeable.
    extent = max(bhi[0] - blo[0], bhi[1] - blo[1])
    if extent > 0:
        k = TARGET_EXTENT / extent
        posed = [tuple(c * k for c in p) for p in posed]

    # Winding: the NCLIP swap and the Z mirror's handedness flip cancel out,
    # so a mirrored model is NOT swapped here. See points 2 and 5.
    swap = not spec["mirror_z"]

    macro, prefix = spec["macro"], spec["prefix"]
    lines = [
        f"#ifndef {macro}_H",
        f"#define {macro}_H",
        "",
        "/*",
        f" * PlayStation {spec['note']}, converted from assets/{spec['glb']}",
        " * by tools/glb2pslogo.py. Generated - do not hand-edit; see that",
        " * script for what it bakes in and why.",
        " *",
        " * Baked in: the resting pose, the shift that stops the two parts",
        " * intersecting, and winding to suit GTE_CMD_NCLIP. Colours are flat",
        " * per sub-mesh - no shading. The intro only scales them for its fade.",
        " */",
        "",
        '#include "main/model/ps_logo_model.h"',
        "",
        f"#define {macro}_VERTEX_COUNT {len(posed)}",
        f"#define {macro}_FACE_COUNT {len(faces)}",
        "",
        f"static const PSLogoVertex {prefix}Vertices"
        f"[{macro}_VERTEX_COUNT] = {{",
    ]
    for x, y, z in posed:
        lines.append(f"\t{{ {round(x)}, {round(y)}, {round(z)}, 0 }},")
    lines += ["};", "",
              f"static const PSLogoFace {prefix}Faces[{macro}_FACE_COUNT] = {{"]

    for v0, v1, v2, mi in faces:
        r, g, b = spec["colors"][mi]
        a, c = (v2, v1) if swap else (v1, v2)
        lines.append(f"\t{{ {v0}, {a}, {c}, {r}, {g}, {b} }},")

    lines += ["};", "", f"#endif // {macro}_H", ""]

    dst = os.path.join(models_dir, spec["header"])
    with open(dst, "w", newline="\n") as f:
        f.write("\n".join(lines))

    print(f"{spec['header']}: {len(posed)} vertices, {len(faces)} faces, "
          f"winding {'swapped' if swap else 'as-is'}")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets = os.path.join(root, "assets")
    models_dir = os.path.join(root, "src", "main", "model")

    for spec in MODELS:
        convert(spec, assets, models_dir)


if __name__ == "__main__":
    main()
