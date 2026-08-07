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
        # plate thinning.
        "mirror_z":   True,
        "bake_yaw":   -25.0,
        "bake_pitch":  12.0,
        # C's raw triangles face the opposite way after the root correction
        # and Z mirror. Keeping B's winding renders the far shell through the
        # culled near shell: its silhouette remains plausible, but the visible
        # extrusion stays on the left regardless of the light direction.
        "swap_winding": True,
        "scale":       13.0,
        "colors":     [BIOS_REF_BLUE, BIOS_REF_TEAL,
                       BIOS_REF_RED, BIOS_REF_YELLOW],
        # Keep the P at the source GLB's original vertical level with the S.
        # This deliberately removes the temporary 0.820-unit lift.
        "shift":      (2, 1, 0.0),
        "swoosh":     ([0, 1, 3], 2, 1.0),
        "swoosh_thin": 1.0,
        "swoosh_erode": 0.0,
        # Pull only the P a fraction toward the camera after normalisation.
        # This does not lift or resize it; it only settles the painter's sort
        # so the yellow plate cannot peek through the P's lower front edge.
        "camera_forward": (2, 36.0),
        # Reproduce the Blender reference with the geometry's REAL extrusion
        # faces. Large P/S caps remain flat; only their side walls receive a
        # directional Lambert bake, with extra occlusion inside the P cutout.
        # No artificial contour strips or screen-position gradients are used.
        "shadow_pose": (23.0, 8.0, 4.0),
        "blender_shadow": True,
        "shadow_variants": [
            {
                "name": "BLENDER REF",
                "light": (-0.55, -0.65, -0.52),
                "ambient": 0.38, "inner_ao": 0.62,
            },
            {
                "name": "UP LEFT",
                "light": (-0.58, -0.58, -0.57),
                "ambient": 0.38, "inner_ao": 0.65,
            },
            {
                "name": "UP RIGHT",
                "light": (0.58, -0.58, -0.57),
                "ambient": 0.38, "inner_ao": 0.65,
            },
            {
                "name": "DOWN LEFT",
                "light": (-0.58, 0.58, -0.57),
                "ambient": 0.38, "inner_ao": 0.65,
            },
            {
                "name": "DOWN RIGHT",
                "light": (0.58, 0.58, -0.57),
                "ambient": 0.38, "inner_ao": 0.65,
            },
            {
                "name": "LEFT SIDE",
                "light": (-0.90, 0.0, -0.44),
                "ambient": 0.38, "inner_ao": 0.65,
            },
            {
                "name": "RIGHT SIDE",
                "light": (0.90, 0.0, -0.44),
                "ambient": 0.38, "inner_ao": 0.65,
            },
            {
                "name": "TOP LIGHT",
                "light": (0.0, -0.90, -0.44),
                "ambient": 0.38, "inner_ao": 0.65,
            },
            {
                "name": "BOTTOM LIGHT",
                "light": (0.0, 0.90, -0.44),
                "ambient": 0.38, "inner_ao": 0.65,
            },
            {
                "name": "FRONT LIGHT",
                "light": (0.0, 0.0, -1.0),
                "ambient": 0.35, "inner_ao": 0.65,
            },
            {
                "name": "BACK LIGHT",
                "light": (0.0, 0.0, 1.0),
                "ambient": 0.35, "inner_ao": 0.65,
            },
            {
                "name": "INNER ONLY",
                "shadow_scope": "inner", "uniform_factor": 0.35,
            },
            {
                "name": "OUTER ONLY",
                "shadow_scope": "outer", "uniform_factor": 0.45,
            },
            {
                "name": "ALL SOFT",
                "shadow_scope": "all", "uniform_factor": 0.72,
            },
            {
                "name": "ALL MEDIUM",
                "shadow_scope": "all", "uniform_factor": 0.50,
            },
            {
                "name": "ALL DEEP",
                "shadow_scope": "all", "uniform_factor": 0.28,
            },
        ],
        # Four times the original 330-unit extent, and twice C's previous
        # internal size. The runtime doubles camera depth with it, so the
        # on-screen size is unchanged while vertex rounding has twice the
        # precision. 1320 remains comfortably inside signed GTE coordinates.
        "target_extent": 1320.0,
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

    # C's P and swoosh touch in depth. Move the P a tiny amount toward the
    # camera after all normalisation so the painter's sort cannot let the
    # yellow plate leak through its lower front edge. This is deliberately a
    # depth-only separation; its vertical placement remains source-accurate.
    camera_forward = spec.get("camera_forward")
    if camera_forward:
        forward_mesh, forward_amount = camera_forward
        for i, mi in enumerate(owner):
            if mi == forward_mesh:
                x, y, z = posed[i]
                posed[i] = (x, y, z - forward_amount)

    # Face indices whose colours are controlled by C's five contour-shadow
    # variants. The requested paths are not all visible side walls at this
    # camera angle, so slim inset strips are added on the P and S front caps.
    # This gives narrow, exact contours instead of darkening the large cap
    # triangles that happen to have right-side centroids.
    trace_faces = set()

    if spec.get("trace_shadow"):
        base_face_count = len(faces)
        trace_yaw, trace_pitch, trace_roll = spec["trace_pose"]

        def rotate_trace(p):
            x, y, z = p
            sy = math.sin(math.radians(trace_yaw))
            cy = math.cos(math.radians(trace_yaw))
            sp = math.sin(math.radians(trace_pitch))
            cp = math.cos(math.radians(trace_pitch))
            sr = math.sin(math.radians(trace_roll))
            cr = math.cos(math.radians(trace_roll))
            x1 = cy * x + sy * z
            z1 = -sy * x + cy * z
            y2 = cp * y - sp * z1
            z2 = sp * y + cp * z1
            return (cr * x1 - sr * y2, sr * x1 + cr * y2, z2)

        def face_normal(points, face):
            v0, v1, v2 = face[:3]
            a, b, c = points[v0], points[v1], points[v2]
            u = [b[k] - a[k] for k in range(3)]
            w = [c[k] - a[k] for k in range(3)]
            n = [u[1] * w[2] - u[2] * w[1],
                 u[2] * w[0] - u[0] * w[2],
                 u[0] * w[1] - u[1] * w[0]]
            nl = math.sqrt(sum(t * t for t in n))
            if nl < 1e-12:
                return (0.0, 0.0, 0.0)
            return tuple(t / nl for t in n)

        def visible(face, view_points):
            a, b, c = (view_points[face[i]] for i in range(3))
            return ((b[0] - a[0]) * (c[1] - a[1])
                    - (b[1] - a[1]) * (c[0] - a[0])) > 0

        view = [rotate_trace(p) for p in posed]

        swoosh_meshes = set(spec["swoosh"][0])

        # Find the visible P cap's true polygon boundary. The supplied mesh's
        # cap is normal to (1, 0, -1), while its extrusion walls are normal to
        # that direction. Position-based edge keys join the duplicated GLB
        # vertices before separating the exterior and hole outlines.
        p_mesh = spec["shift"][0]
        inv_sqrt2 = 1.0 / math.sqrt(2.0)
        p_axis = (inv_sqrt2, 0.0, -inv_sqrt2)
        p_cap_faces = []

        for fi in range(base_face_count):
            face = faces[fi]
            if face[3] != p_mesh or not visible(face, view):
                continue
            n = face_normal(verts, face)
            if abs(sum(n[k] * p_axis[k] for k in range(3))) > 0.9:
                p_cap_faces.append(fi)

        edge_faces = {}
        for fi in p_cap_faces:
            v0, v1, v2, _mi = faces[fi]
            for edge in ((v0, v1), (v1, v2), (v2, v0)):
                key = tuple(sorted(
                    tuple(round(c, 3) for c in verts[v]) for v in edge
                ))
                edge_faces.setdefault(key, []).append((fi, edge))

        cap_vertices = {v for fi in p_cap_faces for v in faces[fi][:3]}
        px_lo = min(view[v][0] for v in cap_vertices)
        px_hi = max(view[v][0] for v in cap_vertices)
        py_lo = min(view[v][1] for v in cap_vertices)
        py_hi = max(view[v][1] for v in cap_vertices)

        # Convert a small toward-camera offset from the traced view back into
        # the model's resting coordinate frame. It prevents coplanar overlap
        # without changing the P's silhouette at any pose-tool angle.
        depth = spec.get("trace_depth", 2.0)
        sy = math.sin(math.radians(trace_yaw))
        cy = math.cos(math.radians(trace_yaw))
        sp = math.sin(math.radians(trace_pitch))
        cp = math.cos(math.radians(trace_pitch))
        sr = math.sin(math.radians(trace_roll))
        cr = math.cos(math.radians(trace_roll))
        dx, dy, dz = 0.0, 0.0, -depth
        dx, dy = cr * dx + sr * dy, -sr * dx + cr * dy
        dy, dz = cp * dy + sp * dz, -sp * dy + cp * dz
        dx, dz = cy * dx - sy * dz, sy * dx + cy * dz
        overlay_offset = (dx, dy, dz)
        width = spec.get("trace_width", 0.075)

        def add_trace_strip(fi, edge):
            face = faces[fi]
            strip_mesh = face[3]
            a_index, b_index = edge
            c_index = next(v for v in face[:3] if v not in edge)
            a, b, c = posed[a_index], posed[b_index], posed[c_index]
            edge_v = tuple(b[k] - a[k] for k in range(3))
            cap_n = face_normal(posed, face)
            inward = (cap_n[1] * edge_v[2] - cap_n[2] * edge_v[1],
                      cap_n[2] * edge_v[0] - cap_n[0] * edge_v[2],
                      cap_n[0] * edge_v[1] - cap_n[1] * edge_v[0])
            midpoint = tuple((a[k] + b[k]) * 0.5 for k in range(3))
            if sum(inward[k] * (c[k] - midpoint[k]) for k in range(3)) < 0:
                inward = tuple(-t for t in inward)
            inward_len = math.sqrt(sum(t * t for t in inward))
            if inward_len < 1e-9:
                return
            inward = tuple(width * t / inward_len for t in inward)
            inner_a = tuple(a[k] + inward[k] for k in range(3))
            inner_b = tuple(b[k] + inward[k] for k in range(3))
            quad = (a, b, inner_b, inner_a)
            base = len(posed)
            for p in quad:
                posed.append(tuple(p[k] + overlay_offset[k]
                                   for k in range(3)))
                owner.append(strip_mesh)
            faces.append((base, base + 1, base + 2, strip_mesh))
            trace_faces.add(len(faces) - 1)
            faces.append((base, base + 2, base + 3, strip_mesh))
            trace_faces.add(len(faces) - 1)

        for items in edge_faces.values():
            if len(items) != 1:
                continue
            fi, edge = items[0]
            xns = [(view[v][0] - px_lo) / max(1e-9, px_hi - px_lo)
                   for v in edge]
            yns = [(view[v][1] - py_lo) / max(1e-9, py_hi - py_lo)
                   for v in edge]

            # Test both endpoints, not only the midpoint. That rejects long
            # triangulation seams which merely cross a traced region while
            # retaining the short segments that actually follow its contour.
            top_and_outer = min(xns) >= 0.10 and all(
                yn <= 0.04 + 0.10 * xn + 0.10 * xn * xn * xn
                for xn, yn in zip(xns, yns)
            )
            outer_curve = all(
                0.10 <= yn <= 0.61
                and xn >= (0.55 + 0.45 * math.sqrt(max(
                    0.0, 1.0 - ((yn - 0.35) / 0.235) ** 2
                ))) - 0.045
                for xn, yn in zip(xns, yns)
            )
            inner_stem = (min(xns) >= 0.35 and max(xns) <= 0.47
                          and min(yns) >= 0.27)
            if top_and_outer or outer_curve or inner_stem:
                add_trace_strip(fi, edge)

        # The S trace follows its lower OUTER silhouette. Its coloured bands
        # are separate meshes, so their shared cut edges are merged by source
        # position first; only edges occurring once remain true contours. A
        # sloping lower-envelope test follows the yellow-to-blue path drawn in
        # the annotation and rejects the black inner slit above it.
        s_cap_faces = []
        for fi in range(base_face_count):
            face = faces[fi]
            if face[3] not in swoosh_meshes or not visible(face, view):
                continue
            if abs(face_normal(verts, face)[1]) > 0.9:
                s_cap_faces.append(fi)

        s_edge_faces = {}
        for fi in s_cap_faces:
            v0, v1, v2, _mi = faces[fi]
            for edge in ((v0, v1), (v1, v2), (v2, v0)):
                key = tuple(sorted(
                    tuple(round(c, 3) for c in verts[v]) for v in edge
                ))
                s_edge_faces.setdefault(key, []).append((fi, edge))

        s_cap_vertices = {v for fi in s_cap_faces for v in faces[fi][:3]}
        sx_lo = min(view[v][0] for v in s_cap_vertices)
        sx_hi = max(view[v][0] for v in s_cap_vertices)
        sy_lo = min(view[v][1] for v in s_cap_vertices)
        sy_hi = max(view[v][1] for v in s_cap_vertices)

        for items in s_edge_faces.values():
            if len(items) != 1:
                continue
            fi, edge = items[0]
            edge_screen_x = [view[v][0] for v in edge]
            stem_right = px_lo + 0.47 * (px_hi - px_lo)
            if max(edge_screen_x) > px_lo and min(edge_screen_x) < stem_right:
                # The P owns this screen interval. Its tiny forward offset
                # handles the real mesh contact; do not lay an S contour strip
                # across the red foot and recreate the yellow leak as an
                # overlay.
                continue
            xns = [(view[v][0] - sx_lo) / max(1e-9, sx_hi - sx_lo)
                   for v in edge]
            yns = [(view[v][1] - sy_lo) / max(1e-9, sy_hi - sy_lo)
                   for v in edge]
            lower_outer = all(
                yn >= 0.80 - 0.31 * xn for xn, yn in zip(xns, yns)
            )
            if lower_outer:
                add_trace_strip(fi, edge)

    # Winding: the NCLIP swap and the Z mirror's handedness flip normally
    # cancel. C's source triangles are the exception and explicitly override
    # this: otherwise NCLIP retains its far shell and exposes the extrusion on
    # the opposite screen edge. See points 2 and 5.
    swap = spec.get("swap_winding", not spec["mirror_z"])

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
        (" * intersecting, and winding to suit GTE_CMD_NCLIP. Base colours are"
         if spec.get("trace_shadow") or spec.get("blender_shadow") else
         " * intersecting, and winding to suit GTE_CMD_NCLIP. Colours are flat"),
        (" * flat per sub-mesh; optional tables carry prebaked contour shadows."
         if spec.get("trace_shadow") or spec.get("blender_shadow") else
         " * per sub-mesh - no shading. The intro only scales them for its fade."),
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

    def unit_face_normal(points, face):
        v0, v1, v2 = face[:3]
        a, b, c = points[v0], points[v1], points[v2]
        u = [b[k] - a[k] for k in range(3)]
        w = [c[k] - a[k] for k in range(3)]
        n = [u[1] * w[2] - u[2] * w[1],
             u[2] * w[0] - u[0] * w[2],
             u[0] * w[1] - u[1] * w[0]]
        nl = math.sqrt(sum(t * t for t in n))
        if nl < 1e-12:
            return (0.0, 0.0, 0.0)
        return tuple(t / nl for t in n)

    # Normals for the Blender-style C bake. `verts` is still in the supplied
    # GLB's source frame, where the S caps point along Y and the P caps along
    # the diagonal (1, 0, -1). `shadow_view` applies the current hardware pose
    # so the directional light is described in visible screen coordinates.
    blender_source_normals = None
    blender_view_normals = None
    blender_view = None
    blender_centers = None

    if spec.get("blender_shadow"):
        yaw, pitch, roll = spec.get("shadow_pose", (0.0, 0.0, 0.0))
        sy, cy = math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
        sp, cp = math.sin(math.radians(pitch)), math.cos(math.radians(pitch))
        sr, cr = math.sin(math.radians(roll)), math.cos(math.radians(roll))
        blender_view = []
        for x, y, z in posed:
            x1 = cy * x + sy * z
            z1 = -sy * x + cy * z
            y2 = cp * y - sp * z1
            z2 = sp * y + cp * z1
            blender_view.append((cr * x1 - sr * y2,
                                 sr * x1 + cr * y2, z2))

        blender_source_normals = [unit_face_normal(verts, face)
                                  for face in faces]
        blender_view_normals = [unit_face_normal(blender_view, face)
                                for face in faces]
        blender_centers = []
        for mi in range(len(spec["colors"])):
            mv = [blender_view[i] for i, o in enumerate(owner) if o == mi]
            blender_centers.append(tuple(
                (min(v[a] for v in mv) + max(v[a] for v in mv)) / 2
                for a in range(3)
            ))

    def build_face_colors(variant):
        """Return one RGB triple per face for a shadow-bake configuration."""
        cfg = dict(spec)
        cfg.update(variant)
        screen_right_shadow = cfg.get("screen_right_shadow", False)
        model_light = cfg.get("light", LIGHT)
        flat_dot = cfg.get("flat_dot", FLAT_DOT)
        rim_base = cfg.get("rim_base", RIM_BASE)
        shade_outer_only = cfg.get("shade_outer_only", False)
        shade_right_only_meshes = cfg.get("shade_right_only_meshes", [])
        shade_left_only_meshes = cfg.get("shade_left_only_meshes", [])
        ln = math.sqrt(sum(c * c for c in model_light))
        light = tuple(c / ln for c in model_light)
        colors = []

        # Explicit visible-screen X coordinates for the right-side comparison
        # bakes. This is deliberately independent of surface normals: the
        # right P curve is largely front-facing geometry, so a rim-only Lambert
        # can never place a visible shadow band there.
        screen_x = None
        screen_lo = screen_hi = 0.0
        if screen_right_shadow:
            yaw, pitch, roll = cfg.get("shadow_pose", (0.0, 0.0, 0.0))
            sy, cy = math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
            sp, cp = math.sin(math.radians(pitch)), math.cos(math.radians(pitch))
            sr, cr = math.sin(math.radians(roll)), math.cos(math.radians(roll))
            screen_x = []
            for x, y, z in posed:
                x1 = cy * x + sy * z
                z1 = -sy * x + cy * z
                y2 = cp * y - sp * z1
                screen_x.append(cr * x1 - sr * y2)
            screen_lo, screen_hi = min(screen_x), max(screen_x)

        for fi, (v0, v1, v2, mi) in enumerate(faces):
            if cfg.get("blender_shadow"):
                source_n = blender_source_normals[fi]
                if mi == spec["shift"][0]:
                    inv_sqrt2 = 1.0 / math.sqrt(2.0)
                    cap_dot = abs(source_n[0] * inv_sqrt2
                                  - source_n[2] * inv_sqrt2)
                else:
                    cap_dot = abs(source_n[1])

                # Keep the large artwork caps perfectly flat. Everything not
                # parallel to a cap is a real extrusion/bevel face and gets
                # the same ambient + directional response seen in Blender.
                k = 1.0
                if cap_dot < 0.92:
                    n = tuple(-t for t in blender_view_normals[fi])
                    fc = tuple(sum(blender_view[v][a]
                                   for v in (v0, v1, v2)) / 3
                               for a in range(3))
                    radial = sum(
                        n[a] * (fc[a] - blender_centers[mi][a])
                        for a in range(3)
                    )
                    is_inner = radial > 0
                    scope = cfg.get("shadow_scope", "all")
                    should_shade = (scope == "all"
                                    or (scope == "inner" and is_inner)
                                    or (scope == "outer" and not is_inner))

                    if should_shade:
                        if "uniform_factor" in cfg:
                            k = cfg["uniform_factor"]
                        else:
                            lambert = max(0.0, sum(
                                n[a] * light[a] for a in range(3)
                            ))
                            ambient = cfg.get("ambient", 0.4)
                            k = ambient + (1.0 - ambient) * lambert

                            # The inner P cutout is naturally more occluded
                            # than its outer silhouette. Detect it from the
                            # mesh, not from an arbitrary screen side.
                            if mi == spec["shift"][0] and is_inner:
                                k *= cfg.get("inner_ao", 0.65)

                colors.append(tuple(
                    min(255, max(0, round(ch * k)))
                    for ch in spec["colors"][mi]
                ))
                continue

            if cfg.get("trace_shadow"):
                k = cfg.get("trace_floor", 0.7) if fi in trace_faces else 1.0
                colors.append(tuple(
                    min(255, round(ch * k)) for ch in spec["colors"][mi]
                ))
                continue

            if screen_right_shadow:
                x = (screen_x[v0] + screen_x[v1] + screen_x[v2]) / 3
                xn = (x - screen_lo) / max(1e-9, screen_hi - screen_lo)
                start = cfg.get("right_start", 0.5)
                t = max(0.0, min(1.0, (xn - start) / max(1e-9, 1.0 - start)))
                t = t ** cfg.get("right_power", 1.0)
                floor = cfg.get("right_floor", 0.4)
                k = 1.0 - (1.0 - floor) * t
                colors.append(tuple(
                    min(255, round(ch * k)) for ch in spec["colors"][mi]
                ))
                continue

            # Edge-only rim shading. A face pointing within FLAT_DOT of the
            # camera is flat and keeps full colour; selected rims get one
            # Lambert term.
            a3, b3, c3 = posed[v0], posed[v1], posed[v2]
            u = [b3[k] - a3[k] for k in range(3)]
            w = [c3[k] - a3[k] for k in range(3)]
            n = [u[1] * w[2] - u[2] * w[1],
                 u[2] * w[0] - u[0] * w[2],
                 u[0] * w[1] - u[1] * w[0]]
            nl = math.sqrt(sum(t * t for t in n)) or 1.0
            n = [-t / nl for t in n]

            k = 1.0
            if abs(n[2]) < flat_dot:
                shade = True
                if shade_outer_only:
                    fc = tuple((a3[a] + b3[a] + c3[a]) / 3
                               for a in range(3))
                    radial = sum(
                        n[a] * (fc[a] - mesh_centers[mi][a])
                        for a in range(3)
                    )
                    shade = radial < 0

                    if mi in shade_right_only_meshes:
                        shade = shade and fc[0] > mesh_centers[mi][0]
                    if mi in shade_left_only_meshes:
                        shade = shade and fc[0] <= mesh_centers[mi][0]

                if shade:
                    lam = max(0.0, sum(p * q for p, q in zip(n, light)))
                    k = rim_base + (1.0 - rim_base) * lam

            colors.append(tuple(
                min(255, round(ch * k)) for ch in spec["colors"][mi]
            ))

        return colors

    variants = spec.get("shadow_variants", [spec])
    variant_colors = [build_face_colors(variant) for variant in variants]

    for (v0, v1, v2, _mi), (r, g, b) in zip(faces, variant_colors[0]):
        a, c = (v2, v1) if swap else (v1, v2)
        lines.append(f"\t{{ {v0}, {a}, {c}, {r}, {g}, {b} }},")

    lines += ["};"]

    if len(variants) > 1:
        lines += [
            "",
            f"#define {macro}_SHADE_COUNT {len(variants)}",
            "",
            f"static const char *const {prefix}ShadeNames"
            f"[{macro}_SHADE_COUNT] = {{",
        ]
        for variant in variants:
            lines.append(f"\t{json.dumps(variant['name'])},")
        lines += [
            "};",
            "",
            f"static const uint8_t {prefix}ShadeColors"
            f"[{macro}_SHADE_COUNT][{macro}_FACE_COUNT][3] = {{",
        ]
        for colors in variant_colors:
            lines.append("\t{")
            for r, g, b in colors:
                lines.append(f"\t\t{{ {r}, {g}, {b} }},")
            lines.append("\t},")
        lines.append("};")

    lines += ["", f"#endif // {macro}_H", ""]

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
