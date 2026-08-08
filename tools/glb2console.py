#!/usr/bin/env python3
"""Convert the two supplied PlayStation console GLBs into TEXTURED pose models.

Writes into src/main/model/. Regenerate with:

    python tools/glb2console.py

No dependencies beyond the standard library and Pillow. GLB is a JSON chunk
followed by a binary chunk; the only accessors needed are float POSITION, float
TEXCOORD_0 and uint16/uint32 indices.

WHY THIS EXISTS
---------------

tools/blender_prepare_pose_consoles.py already converted both of these models,
but it sampled each triangle's texture ONCE and stored the result as a single
flat face colour. For a console whose entire visual interest is its texture -
the disc lid, the SONY and PlayStation lettering, the vents, the buttons - that
throws away everything worth looking at. Measured on its output:

    ps_console_classic.h   296 faces,  6 unique colours (191 identical grey)
    ps_console_psone.h      80 faces,  5 unique colours ( 74 identical grey)

i.e. two featureless white blobs. This tool keeps the per-vertex UVs instead
and emits the texture alongside the geometry, which is what "GPU: Test
PlayStation Model" (src/main/model_test.c) has always done for the PS one - and
that screen has been correct on hardware from the start.

CONVENTIONS, AND HOW THEY WERE ESTABLISHED
------------------------------------------

Not guessed. model_data.h + modeltex.h are the known-good pair that model_test.c
draws, so they were decoded and matched back against ps_one_pixel.glb:

  * Vertices are the glTF node hierarchy applied to POSITION, with NO axis
    permutation - model_data.h is exactly the world-space coordinates times 100.

  * UVs are `tu = round(u * (W - 1))`, `tv = round((1 - v) * (H - 1))`. The V
    flip and the (size - 1) - rather than size - multiplier both matter: with
    them, all 80 of the PS one's faces reproduce model_data.h's stored UVs
    exactly; without the flip, none of them do.

  * The texture is the embedded PNG flipped top-to-bottom and resampled NEAREST
    (98% of modeltex.h's texels reproduce bit-for-bit; the remainder is
    resampling phase on a 1024 -> 128 reduction). NEAREST is not laziness -
    these are pixel-art textures whose native grid is an exact multiple of the
    target, so any smoothing filter would blur art that is already the right
    resolution.

The pose-tool axis map is separate and is NOT model_test's:

    X =  x        Y = -y        Z = -z          (glTF world space)

centred on the bounding box and scaled so the longest axis is TARGET_EXTENT.

Both negations are needed and neither is cosmetic. Y flips because PS1 screen
space counts downward. Z flips because glTF is right-handed with +Z pointing at
the viewer, while the GTE's Z is depth AWAY from the camera - carrying glTF Z
straight across views the model from BEHIND, which reads as a mirror image and
would put the SONY lettering on backwards. tools/glb2pslogo.py hit exactly this
and documents it at length; the superseded
tools/blender_prepare_pose_consoles.py did NOT flip Z, which nobody could have
noticed because its output had no texture to read.

WINDING
-------

GTE_CMD_NCLIP keeps faces whose MAC0 is positive, and MAC0 has the same sign as
the Z component of (v1-v0) x (v2-v0) in the space above. Worked through for a
glTF front face (counter-clockwise seen from +Z), say a=(0,0,0) b=(1,0,0)
c=(0,1,0): the map sends it to (0,0,0) (1,0,0) (0,-1,0), whose cross product has
Z = -1. Negative, so glTF's own winding is exactly backwards for NCLIP and every
face has to be swapped. That is unconditional here, not a per-model guess.

What IS per-model is whether the source winding is self-consistent to begin
with. It is for the PS one; PSX.glb has 96 directed edges traversed twice, i.e.
its faces disagree with their neighbours - harmless in a renderer that honours
its doubleSided material, fatal for backface culling. So the mesh is re-oriented
here first (flood fill across shared edges, then the divergence theorem per
connected component to point the shell outward), which is what
bmesh.ops.recalc_face_normals() was doing in the retired Blender script.
"""

import argparse
import io
import json
import pathlib
import re
import struct
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("Pillow is required: python -m pip install pillow")


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODEL_DIR = ROOT / "src" / "main" / "model"

# Same extent every other pose-tool model is normalised to, so the tool's
# existing CAM Z range and step sizes stay meaningful when switching models
# with L1.
TARGET_EXTENT = 1320

# Colour 0x0000 is fully transparent to the PS1 GPU, so solid black has to be
# encoded as something else. These triangles are drawn with blending disabled,
# which makes the GPU ignore the STP bit entirely, so semitransparent black is
# the right choice - it stays genuinely black instead of tools/convertImage.py's
# 0x0421 dark grey compromise (that one is for blended sprites).
STP_BLACK = 0x8000

COMPONENT = {
    5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
    5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4),
}
NUM_COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


MODELS = (
    {
        "source": ROOT / "assets" / "playstation_model_source" / "PSX.glb",
        "stem": "ps_console_classic",
        "guard": "PS_CONSOLE_CLASSIC_H",
        "macro": "PS_CONSOLE_CLASSIC",
        "symbol": "psConsoleClassic",
        "label": "original PlayStation (SCPH-5501)",
        # 256x256 at 8bpp. The source art's native grid is 4x at 1024, so this
        # is a lossless 4:1 reduction and the SONY/PlayStation lettering and the
        # underside's vent lines survive - at 128 they do not.
        "texture": {"size": 256, "depth": 8, "emit": True},
    },
    {
        "source": ROOT / "assets" / "ps_one_pixel.glb",
        "stem": "ps_console_psone",
        "guard": "PS_CONSOLE_PSONE_H",
        "macro": "PS_CONSOLE_PSONE",
        "symbol": "psConsolePSone",
        "label": "PS one (SCPH-100)",
        # Deliberately NOT emitted: this is the same 128x128 16bpp texture
        # model_test.c already carries as modeltex.h, and that one is proven on
        # hardware. Emitting a second copy would cost 32 KB of RAM for identical
        # bytes, so the generated header reuses modeltexTextureData instead and
        # the UVs below are produced in its exact space. `verify` re-derives the
        # texture anyway and checks it still matches, so the two cannot silently
        # drift apart.
        "texture": {"size": 128, "depth": 16, "emit": False,
                    "reuse": "modeltexTextureData",
                    "verify": MODEL_DIR / "modeltex.c"},
    },
)


def load_glb(path):
    data = path.read_bytes()
    if data[:4] != b"glTF":
        raise RuntimeError(f"{path.name} is not a GLB")
    total = struct.unpack_from("<III", data, 0)[2]
    offset, chunks = 12, []
    while offset < total:
        length, _kind = struct.unpack_from("<II", data, offset)
        chunks.append(data[offset + 8: offset + 8 + length])
        offset += 8 + length
    return json.loads(chunks[0].decode("utf-8")), chunks[1]


def read_accessor(gltf, binary, index):
    acc = gltf["accessors"][index]
    view = gltf["bufferViews"][acc["bufferView"]]
    fmt, size = COMPONENT[acc["componentType"]]
    count = NUM_COMPONENTS[acc["type"]]
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = view.get("byteStride") or size * count
    return [
        struct.unpack_from("<" + fmt * count, binary, base + i * stride)
        for i in range(acc["count"])
    ]


def matrix_of(node):
    """A node's local transform, as a row-major 4x4."""
    if "matrix" in node:
        m = node["matrix"]                      # glTF stores column-major
        return [[m[c * 4 + r] for c in range(4)] for r in range(4)]

    result = [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]
    if "rotation" in node:
        x, y, z, w = node["rotation"]
        result = [
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0.0],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0.0],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0.0],
            [0.0, 0.0, 0.0, 1.0],
        ]
    if "scale" in node:
        for c, s in enumerate(node["scale"]):
            for r in range(3):
                result[r][c] *= s
    if "translation" in node:
        for r, t in enumerate(node["translation"]):
            result[r][3] = t
    return result


def multiply(a, b):
    return [
        [sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)]
        for r in range(4)
    ]


def transform(matrix, point):
    x, y, z = point
    return tuple(
        matrix[r][0] * x + matrix[r][1] * y + matrix[r][2] * z + matrix[r][3]
        for r in range(3)
    )


def collect_primitives(gltf, binary):
    """Walk the scene graph, returning world-space (positions, uvs, indices)."""
    identity = [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]
    out = []

    def visit(index, parent):
        node = gltf["nodes"][index]
        world = multiply(parent, matrix_of(node))
        if "mesh" in node:
            for prim in gltf["meshes"][node["mesh"]]["primitives"]:
                attrs = prim["attributes"]
                if "TEXCOORD_0" not in attrs:
                    raise RuntimeError("primitive has no TEXCOORD_0 - this tool "
                                       "only converts textured meshes")
                positions = [transform(world, p)
                             for p in read_accessor(gltf, binary, attrs["POSITION"])]
                uvs = read_accessor(gltf, binary, attrs["TEXCOORD_0"])
                indices = [i[0] for i in
                           read_accessor(gltf, binary, prim["indices"])]
                out.append((positions, uvs, indices))
        for child in node.get("children", []):
            visit(child, world)

    for root in gltf["scenes"][gltf.get("scene", 0)]["nodes"]:
        visit(root, identity)
    return out


def reorient(triangles, positions):
    """Make a mesh's winding self-consistent and outward-facing.

    `triangles` are welded vertex-index triples; `positions` are the welded
    world-space coordinates. Returns a list of booleans, one per triangle, that
    is True where the triangle must be reversed.

    Two stages, because they answer different questions:

      1. Flood fill across shared edges makes neighbouring faces AGREE. Two
         correctly-wound adjacent triangles traverse their shared edge in
         opposite directions; if they traverse it the same way, one of them is
         inside out. This propagates an arbitrary seed's choice across each
         connected component.

      2. That leaves each component consistently wound but possibly consistently
         INSIDE OUT, which stage 1 cannot detect - it only ever compared faces
         against each other. The divergence theorem settles it: for a closed
         surface, sum(dot(a, cross(b, c))) / 6 is the enclosed volume, positive
         when the winding is counter-clockwise-outward in a right-handed space.
         Negative means the whole component is inverted, so flip it.
    """
    adjacency = {}
    for index, tri in enumerate(triangles):
        for k in range(3):
            a, b = tri[k], tri[(k + 1) % 3]
            adjacency.setdefault((min(a, b), max(a, b)), []).append((index, a, b))

    flip = [False] * len(triangles)
    seen = [False] * len(triangles)
    components = []

    for seed in range(len(triangles)):
        if seen[seed]:
            continue
        seen[seed] = True
        component = [seed]
        stack = [seed]
        while stack:
            current = stack.pop()
            tri = triangles[current]
            for k in range(3):
                a, b = tri[k], tri[(k + 1) % 3]
                if flip[current]:
                    a, b = b, a
                for other, oa, ob in adjacency[(min(a, b), max(a, b))]:
                    if other == current or seen[other]:
                        continue
                    if flip[other]:
                        oa, ob = ob, oa
                    # Same direction along a shared edge => one is inside out.
                    if (oa, ob) == (a, b):
                        flip[other] = not flip[other]
                    seen[other] = True
                    component.append(other)
                    stack.append(other)
        components.append(component)

    for component in components:
        volume = 0.0
        for index in component:
            tri = triangles[index]
            a, b, c = (positions[v] for v in tri)
            if flip[index]:
                b, c = c, b
            cross = (
                b[1] * c[2] - b[2] * c[1],
                b[2] * c[0] - b[0] * c[2],
                b[0] * c[1] - b[1] * c[0],
            )
            volume += sum(a[i] * cross[i] for i in range(3))
        if volume < 0.0:
            for index in component:
                flip[index] = not flip[index]

    return flip, len(components)


def embedded_image(gltf, binary):
    view = gltf["bufferViews"][gltf["images"][0]["bufferView"]]
    offset = view.get("byteOffset", 0)
    raw = binary[offset: offset + view["byteLength"]]
    return Image.open(io.BytesIO(raw)).convert("RGB")


def to_bgr555(rgb):
    r, g, b = rgb
    r5 = ((r * 31) + 127) // 255
    g5 = ((g * 31) + 127) // 255
    b5 = ((b * 31) + 127) // 255
    value = r5 | (g5 << 5) | (b5 << 10)
    # 0x0000 would be transparent; see STP_BLACK above.
    return STP_BLACK if value == 0 else value


def build_texture(image, size):
    """Flip, resample and return the texture in the proven model_test space."""
    flipped = image.transpose(Image.FLIP_TOP_BOTTOM)
    return flipped.resize((size, size), Image.NEAREST)


def quantize(texture):
    """Exact palette for a pixel-art texture. Refuses to dither."""
    colours = texture.getcolors(1 << 16)
    if colours is None or len(colours) > 256:
        raise RuntimeError(
            f"texture needs more than 256 colours "
            f"({'many' if colours is None else len(colours)}) - it is not the "
            f"pixel art this tool assumes"
        )
    palette = [c for _count, c in sorted(colours, key=lambda c: -c[0])]
    lookup = {c: i for i, c in enumerate(palette)}
    indices = [lookup[p] for p in texture.getdata()]
    return indices, palette


def convert(spec, check_only=False):
    gltf, binary = load_glb(spec["source"])
    primitives = collect_primitives(gltf, binary)

    texspec = spec["texture"]
    size = texspec["size"]
    texture = build_texture(embedded_image(gltf, binary), size)

    # --- geometry -------------------------------------------------------
    positions, uvs, indices = [], [], []
    for prim_pos, prim_uv, prim_idx in primitives:
        base = len(positions)
        positions.extend(prim_pos)
        uvs.extend(prim_uv)
        indices.extend(i + base for i in prim_idx)

    lo = [min(p[a] for p in positions) for a in range(3)]
    hi = [max(p[a] for p in positions) for a in range(3)]
    centre = [(lo[a] + hi[a]) / 2.0 for a in range(3)]
    extent = max(hi[a] - lo[a] for a in range(3))
    scale = TARGET_EXTENT / extent

    # Pose-tool space: X = x, Y = -y, Z = -z, centred. See the module docstring
    # for why both negations are load-bearing.
    def to_gte(p):
        return (
            int(round((p[0] - centre[0]) * scale)),
            int(round(-(p[1] - centre[1]) * scale)),
            int(round(-(p[2] - centre[2]) * scale)),
        )

    quantized = [to_gte(p) for p in positions]

    unique, unique_map, remap = [], {}, []
    for vertex in quantized:
        index = unique_map.get(vertex)
        if index is None:
            index = len(unique)
            unique_map[vertex] = index
            unique.append(vertex)
        remap.append(index)

    def texel(index):
        u, v = uvs[index][0], uvs[index][1]
        tu = int(round(u * (size - 1)))
        tv = int(round((1.0 - v) * (size - 1)))
        return max(0, min(size - 1, tu)), max(0, min(size - 1, tv))

    # Drop degenerate triangles first: they carry no area, so they contribute
    # nothing to the flood fill except spurious edge adjacencies.
    triangles, corner_sets = [], []
    for t in range(len(indices) // 3):
        corners = [indices[t * 3 + k] for k in range(3)]
        vs = [remap[c] for c in corners]
        if len(set(vs)) < 3:
            continue
        triangles.append(tuple(vs))
        corner_sets.append(corners)

    # Welded world-space positions, for the divergence-theorem stage. Taken
    # before the axis map so "outward" is decided in glTF's own right-handed
    # space, where the sign convention is the documented one.
    welded_world = [None] * len(unique)
    for source_index, target in enumerate(remap):
        if welded_world[target] is None:
            welded_world[target] = positions[source_index]

    flip, components = reorient(triangles, welded_world)
    clashes = sum(1 for f in flip if f)

    faces = []
    for index, (vs, corners) in enumerate(zip(triangles, corner_sets)):
        vs, corners = list(vs), list(corners)
        if flip[index]:
            vs = [vs[0], vs[2], vs[1]]
            corners = [corners[0], corners[2], corners[1]]
        # glTF's counter-clockwise front faces are backwards for NCLIP in this
        # space - see the module docstring. Unconditional.
        vs = [vs[0], vs[2], vs[1]]
        corners = [corners[0], corners[2], corners[1]]
        faces.append((vs, [texel(c) for c in corners]))

    # --- texture --------------------------------------------------------
    report = [
        f"{spec['stem']}: {len(faces)} triangles, {len(unique)} vertices, "
        f"{components} shell(s), {clashes} face(s) re-oriented"
    ]

    if texspec.get("verify"):
        # A tripwire, not a build input: the PS one reuses modeltex.h verbatim,
        # so what has to hold is that this GLB is still the texture modeltex.h
        # was cut from and still maps into it the same way. Compared with plain
        # truncation because that is how modeltex.h was originally quantised
        # (this tool rounds, which is slightly better but would show up as
        # thousands of one-step differences and hide a real mismatch).
        existing = texspec["verify"].read_text()
        words = [int(w, 16) for w in
                 re.findall(r"0x([0-9a-fA-F]{4})", existing)]
        mine = []
        for r, g, b in texture.getdata():
            mine.append((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10))
        theirs = [w & 0x7FFF for w in words[: size * size]]
        same = sum(1 for a, b in zip(mine, theirs) if a == b)
        ratio = same * 100.0 / len(mine)
        report.append(
            f"  reuses {texspec['reuse']}: {ratio:.1f}% of texels reproduce it"
        )
        if ratio < 95.0:
            report.append(
                "  WARNING: modeltex.h no longer matches this GLB - the PS one's"
                " UVs and the texture it reuses may have drifted apart"
            )

    if check_only:
        return report

    # --- emit geometry --------------------------------------------------
    macro, symbol, guard = spec["macro"], spec["symbol"], spec["guard"]
    tex_note = (
        f"Texture: {texspec['reuse']} ({size}x{size}, {texspec['depth']}bpp), "
        f"shared with model_test.c."
        if not texspec["emit"] else
        f"Texture: {symbol}TextureData ({size}x{size}, {texspec['depth']}bpp)."
    )

    lines = [
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        '#include "main/model/ps_console_model.h"',
        "",
        f"/* Generated by tools/glb2console.py from",
        f" * {spec['source'].relative_to(ROOT).as_posix()} - do not edit by hand.",
        " *",
        f" * Complete {spec['label']}: {len(faces)} textured triangles.",
        f" * {tex_note}",
        " *",
        " * Coordinates are pose-tool space (X = x, Y = -y, Z = z from glTF),",
        f" * centred on the bounding box and scaled to a {TARGET_EXTENT}-unit",
        " * longest axis so the pose tool's CAM Z range matches the other",
        " * models. UVs are texel coordinates, already V-flipped. */",
        f"#define {macro}_VERTEX_COUNT {len(unique)}",
        f"#define {macro}_FACE_COUNT {len(faces)}",
        f"#define {macro}_TEX_SIZE {size}",
        "",
        f"static const PSLogoVertex {symbol}Vertices[{macro}_VERTEX_COUNT] = {{",
    ]
    lines.extend(f"\t{{ {x}, {y}, {z}, 0 }}," for x, y, z in unique)
    lines.extend([
        "};",
        "",
        f"static const PSConsoleFace {symbol}Faces[{macro}_FACE_COUNT] = {{",
    ])
    for vs, uv in faces:
        coords = ", ".join(f"{u}, {v}" for u, v in uv)
        lines.append(f"\t{{ {vs[0]}, {vs[1]}, {vs[2]}, {coords} }},")
    lines.extend(["};", "", f"#endif // {guard}", ""])
    (MODEL_DIR / f"{spec['stem']}.h").write_text(
        "\n".join(lines), encoding="utf-8", newline="\n")

    # --- emit texture ---------------------------------------------------
    if texspec["emit"]:
        indices8, palette = quantize(texture)
        report.append(f"  texture: {len(palette)} colours, {size}x{size} 8bpp")

        # 8bpp VRAM data is two texels per 16-bit word, low byte first.
        packed = [
            indices8[i] | (indices8[i + 1] << 8)
            for i in range(0, len(indices8), 2)
        ]
        clut = [to_bgr555(c) for c in palette]
        clut += [0] * (256 - len(clut))

        header_guard = f"{spec['guard'][:-2]}_TEX_H"
        (MODEL_DIR / f"{spec['stem']}_tex.h").write_text("\n".join([
            f"#ifndef {header_guard}",
            f"#define {header_guard}",
            "",
            "#include <stdint.h>",
            "",
            f"/* Generated by tools/glb2console.py - see {spec['stem']}.h.",
            " *",
            " * Kept in its own translation unit rather than as a static array in",
            " * a header so that including it from more than one place cannot",
            " * silently duplicate 64 KB of texture in a 2 MB console. */",
            f"#define {macro}_TEX_WIDTH  {size}",
            f"#define {macro}_TEX_HEIGHT {size}",
            f"#define {macro}_CLUT_COLORS 256",
            "",
            f"extern const uint16_t {symbol}TextureData[];",
            f"extern const uint16_t {symbol}ClutData[];",
            "",
            f"#endif // {header_guard}",
            "",
        ]), encoding="utf-8", newline="\n")

        body = [
            f'#include "main/model/{spec["stem"]}_tex.h"',
            "",
            f"/* Generated by tools/glb2console.py - do not edit by hand. */",
            "",
            f"const uint16_t {symbol}ClutData[{macro}_CLUT_COLORS] = {{",
        ]
        for i in range(0, len(clut), 8):
            body.append("\t" + " ".join(f"0x{v:04x}," for v in clut[i:i + 8]))
        body.extend([
            "};",
            "",
            f"const uint16_t {symbol}TextureData"
            f"[{macro}_TEX_WIDTH * {macro}_TEX_HEIGHT / 2] = {{",
        ])
        for i in range(0, len(packed), 12):
            body.append("\t" + " ".join(f"0x{v:04x}," for v in packed[i:i + 12]))
        body.extend(["};", ""])
        (MODEL_DIR / f"{spec['stem']}_tex.c").write_text(
            "\n".join(body), encoding="utf-8", newline="\n")

    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="report what would be produced, write nothing")
    args = parser.parse_args()

    for spec in MODELS:
        if not spec["source"].exists():
            sys.exit(f"missing source GLB: {spec['source']}")
        for line in convert(spec, check_only=args.check):
            print(line)


if __name__ == "__main__":
    main()
