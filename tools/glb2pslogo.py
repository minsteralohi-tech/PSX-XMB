#!/usr/bin/env python3
"""Convert the PlayStation logo GLBs into the boot sequence's model headers.

Reads the GLBs listed in MODELS below and writes one header each into
src/main/model/. Regenerate with:

    python3 tools/glb2pslogo.py

No dependencies beyond the standard library: GLB is a JSON chunk followed by a
binary chunk, and the only accessors needed here are float POSITION and
uint16/uint32 indices.

THREE SELECTABLE MODELS:

  A  ps_logo_bios.h   a shallow relief - the flat artwork extruded a little,
                      everything in one slab
  B  ps_logo_bios2.h  built the way the real BIOS logo is: the P standing
                      upright on a swoosh lying flat on the ground plane
  C  ps_logo_bios3.h  the newly supplied full-resolution BIOS GLB, kept as a
                      separate pose-tool option so it can be tuned independently

The intro picks among them with PS_LOGO_MODEL in intro_ps1.c, and the pose
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

# Dominant flat-face colours sampled from the supplied original-BIOS screen.
# C uses these independently of A/B so the new comparison option matches that
# reference without silently changing the already hardware-tuned B model.
BIOS_REF_RED    = (255,  31,   3)
BIOS_REF_YELLOW = (232, 163,   0)
BIOS_REF_TEAL   = ( 38, 141, 136)
BIOS_REF_BLUE   = ( 53,  88, 146)

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
        "swoosh_thin": 1.0,
        "swoosh_erode": 0.0,
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
        # The P (sub-mesh 2) stands ON the swoosh plate. 0.682 puts its foot
        # exactly on the plate's top surface - joined edge to edge, no gap and
        # no overlap. It is not a round number because it is measured: build
        # once with 0, read the P's lowest Y against the swoosh's highest, and
        # the difference is this. Re-derive it if the plate thinning changes.
        "shift":      (2, 1, 0.682),
        # Grow the swoosh (sub-meshes 0, 1 and 3) about the P's centre
        "swoosh":     ([0, 1, 3], 2, 1.6),
        # Thin the swoosh plates to match the P's edge ON SCREEN.
        #
        # The raw thicknesses already match almost exactly - the P's slab is
        # 0.948 and the plates are 0.920 - so this is NOT a numeric mismatch.
        # The swoosh lies flat, so its edge faces the camera and projects at
        # nearly full length, while the P stands upright and its edge is
        # foreshortened. Matching what the eye sees means making the plates
        # numerically thinner than the P.
        "swoosh_thin": 0.7,
        # Narrow the ribbon's STROKE without shrinking the S.
        #
        # The 1.6x enlargement above scaled the ribbon's stroke along with its
        # footprint, leaving the S visibly fatter than the P's stem. Scaling
        # back down would undo the enlargement, so instead every boundary
        # vertex is pushed inward along its own rim normal - a real inward
        # offset, which trims the stroke and leaves the S's size alone.
        #
        # 1.0 source units was picked by eye against the P's stem. An
        # area-over-perimeter estimate suggested more than twice that, but it
        # counts the ribbon's flat cut ends as perimeter and so badly
        # under-reads the stroke; the render is the authority here.
        "swoosh_erode": 1.0,
    },
    {
        "glb":        "bios_playstation.glb",
        "header":     "ps_logo_bios3.h",
        "prefix":     "psLogoBios3",
        "macro":      "PS_LOGO_BIOS3",
        "note":       "logo C - the newly supplied full-resolution BIOS model",
        # Preserve the newly supplied model's original proportions. In
        # particular, do NOT inherit B's 1.6x swoosh growth, ribbon erosion or
        # plate thinning. The only non-uniform edit is the measured P lift below
        # that removes an actual intersection without reshaping either part.
        "mirror_z":   True,
        "bake_yaw":   -25.0,
        "bake_pitch":  12.0,
        "scale":       13.0,
        "colors":     [BIOS_REF_BLUE, BIOS_REF_TEAL,
                       BIOS_REF_RED, BIOS_REF_YELLOW],
        # C retains the source plate thickness, so its top is 0.820 source
        # units above the P's lowest point. Lift only the P by that exact
        # amount: the parts touch edge-to-edge without the intersection that
        # lets a yellow face win the painter sort across the red foot.
        "shift":      (2, 1, 0.820),
        "swoosh":     ([0, 1, 3], 2, 1.0),
        "swoosh_thin": 1.0,
        "swoosh_erode": 0.0,
        # Shade only each sub-mesh's OUTER boundary. The earlier generic rim
        # Lambert shaded the cutout walls too, which put the dark band on the
        # logo's internal edges. With the internal rims excluded, this light
        # places the BIOS-style dark band on the outside-right/bottom edges.
        # These overrides apply to C only.
        "light":       (0.6, -0.6, -0.53),
        "rim_base":    0.42,
        "shade_outer_only": True,
        # The P is mesh 2. On that mesh the BIOS shadow belongs on the outer
        # right bowl/curve, not the long outside-left wall of the stem.
        "shade_right_only_meshes": [2],
        # Twice the previous 330-unit posed extent. This is a uniform scale;
        # it does not alter C's original proportions.
        "target_extent": 660.0,
    },
]

# Default posed extent used by A/B. A model can override this; C requests 660
# so its whole object is exactly 2x the old 330-unit pose-tool size.
TARGET_EXTENT = 330.0

# Rim shading, baked in the resting pose. LIGHT points from the surface toward
# the light in that pose - X right, Y DOWN (PS1 screen space), Z into the
# screen - so this is the top-left of the screen, slightly in front.
#
# Deliberately gentle and EDGE-ONLY: a face pointing within FLAT_DOT of the
# camera is one of the flat faces and keeps its colour untouched, so the logo
# still reads as flat colour. Only the extruded rims vary, which shows up
# mainly on the P's curved bowl edge. An earlier revision shaded far harder
# and muddied the flat faces; RIM_BASE is the floor a rim facing fully away
# from the light drops to.
# X is POSITIVE. Read literally that is a light to the right, but it is what
# reproduces the reference BIOS screen's shading, which is the thing being
# matched. Two earlier revisions had it the other way and put the dark rims on
# the wrong edge of the P.
LIGHT    = (0.6, -0.6, -0.53)
FLAT_DOT = 0.55
RIM_BASE = 0.72


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

    # Trim the ribbon's stroke by offsetting its boundary inward. Each rim
    # face contributes its in-plane normal to its vertices; every vertex then
    # moves against that averaged direction. See swoosh_erode above.
    erode = spec["swoosh_erode"]
    if erode > 0:
        # Positions used by more than one colour band are seams. The bands are
        # separate sub-meshes, so each contributes a rim face at every join.
        pos_bands = {}
        for i, o in enumerate(owner):
            if o in swoosh_meshes:
                pos_bands.setdefault(
                    tuple(round(t, 3) for t in verts[i]), set()
                ).add(o)

        shared = [
            o in swoosh_meshes
            and len(pos_bands[tuple(round(t, 3) for t in verts[i])]) >= 2
            for i, o in enumerate(owner)
        ]

        # First pass: split the swoosh's rim faces into seams and candidates,
        # and remember each band's seam normal - the direction of a cut ACROSS
        # the ribbon.
        seam_n, rim = {}, []

        for v0, v1, v2, mi in faces:
            if mi not in swoosh_meshes:
                continue
            a, b, c = verts[v0], verts[v1], verts[v2]
            u = [b[k] - a[k] for k in range(3)]
            w = [c[k] - a[k] for k in range(3)]
            n = [u[1] * w[2] - u[2] * w[1],
                 u[2] * w[0] - u[0] * w[2],
                 u[0] * w[1] - u[1] * w[0]]
            nl = math.sqrt(sum(t * t for t in n))
            if nl < 1e-12:
                continue
            n = [t / nl for t in n]
            if abs(n[1]) > 0.5:      # a flat face, not a rim
                continue
            if shared[v0] and shared[v1] and shared[v2]:
                seam_n[mi] = (n[0], n[2])
                continue
            rim.append((v0, v1, v2, n[0], n[2], mi))

        # Second pass. A rim face running parallel to its band's seam is
        # another cross-cut - the ribbon's free tip, or the end that butts
        # against the P. Eroding those SHORTENS the ribbon, which is what put a
        # gap between the S and the P; their vertices are pinned so the tip
        # stays a straight edge instead of being nicked by the side faces.
        acc = {i: [0.0, 0.0] for i, o in enumerate(owner) if o in swoosh_meshes}
        pinned = set()

        for v0, v1, v2, nx, nz, mi in rim:
            sn = seam_n.get(mi)
            if sn and abs(nx * sn[0] + nz * sn[1]) > 0.75:
                pinned.update(v for v in (v0, v1, v2) if not shared[v])
                continue
            for v in (v0, v1, v2):
                acc[v][0] += nx
                acc[v][1] += nz

        for i, (dx, dz) in acc.items():
            if i in pinned:
                continue
            m = math.hypot(dx, dz)
            if m < 1e-9:
                continue
            x, y, z = verts[i]
            verts[i] = (x - erode * dx / m, y, z - erode * dz / m)

    # Thin the swoosh plates about their own mid-plane. See swoosh_thin above.
    thin = spec["swoosh_thin"]
    if thin != 1.0:
        sv = [verts[i][1] for i, o in enumerate(owner) if o in swoosh_meshes]
        smid = (min(sv) + max(sv)) / 2
        for i, o in enumerate(owner):
            if o in swoosh_meshes:
                x, y, z = verts[i]
                verts[i] = (x, smid + (y - smid) * thin, z)

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

    # Normalise to the model's requested posed extent. A/B use the shared 330
    # default so they remain interchangeable; C explicitly requests 660 for a
    # 2x pose-tool/render size.
    extent = max(bhi[0] - blo[0], bhi[1] - blo[1])
    target_extent = spec.get("target_extent", TARGET_EXTENT)
    if extent > 0 and target_extent > 0:
        k = target_extent / extent
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

    model_light = spec.get("light", LIGHT)
    flat_dot = spec.get("flat_dot", FLAT_DOT)
    rim_base = spec.get("rim_base", RIM_BASE)
    shade_outer_only = spec.get("shade_outer_only", False)
    shade_right_only_meshes = spec.get("shade_right_only_meshes", [])
    ln = math.sqrt(sum(c * c for c in model_light))
    light = tuple(c / ln for c in model_light)

    # Per-sub-mesh centres let C distinguish the outside silhouette from hole
    # and cutout walls. With this file's exported winding the normal below
    # points toward the solid: on an outer rim its dot with (face-centre minus
    # mesh-centre) is negative; on an internal rim it is positive.
    mesh_centers = []
    for mi in range(len(spec["colors"])):
        mv = [posed[i] for i, o in enumerate(owner) if o == mi]
        mesh_centers.append(tuple(
            (min(v[a] for v in mv) + max(v[a] for v in mv)) / 2
            for a in range(3)
        ))

    for v0, v1, v2, mi in faces:
        # Edge-only rim shading. A face pointing within FLAT_DOT of the camera
        # is a flat face and keeps full colour; rims get one gentle Lambert
        # term. See the LIGHT block above.
        a3, b3, c3 = posed[v0], posed[v1], posed[v2]
        u = [b3[k] - a3[k] for k in range(3)]
        w = [c3[k] - a3[k] for k in range(3)]
        n = [u[1] * w[2] - u[2] * w[1],
             u[2] * w[0] - u[0] * w[2],
             u[0] * w[1] - u[1] * w[0]]
        nl = math.sqrt(sum(t * t for t in n)) or 1.0
        # NEGATED. The exported winding swap leaves this cross product pointing
        # INTO the solid, so using it raw lights the side away from the light -
        # which showed up as the shading landing on the wrong edge of the P.
        n = [-t / nl for t in n]

        k = 1.0
        if abs(n[2]) < flat_dot:
            shade = True
            if shade_outer_only:
                fc = tuple((a3[a] + b3[a] + c3[a]) / 3 for a in range(3))
                radial = sum(
                    n[a] * (fc[a] - mesh_centers[mi][a])
                    for a in range(3)
                )
                shade = radial < 0

                # C's red P: keep its left stem wall at the full face colour
                # and put the baked dark band only around the outside-right
                # curve, matching the supplied BIOS capture.
                if mi in shade_right_only_meshes:
                    shade = shade and fc[0] > mesh_centers[mi][0]

            if shade:
                lam = max(0.0, sum(p * q for p, q in zip(n, light)))
                k = rim_base + (1.0 - rim_base) * lam

        r, g, b = (min(255, round(ch * k)) for ch in spec["colors"][mi])
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
