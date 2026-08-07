#!/usr/bin/env python3
"""Re-cut the SCE wordmarks for the boot intro, antialiased.

The intro's SONY / COMPUTER / ENTERTAINMENT / (TM) art started life as
one-bit masks: one transparent index, one ink index, nothing in between. At
4bpp that leaves fourteen of the sixteen palette entries unused and every
diagonal and curve as a bare staircase - which is precisely why the wordmarks
looked pixelated next to the real BIOS screen. The BIOS art is antialiased.
That, not resolution, is the difference.

This reads the one-bit masters in assets/orig_intro_*.png, resamples them to
the sizes measured off a real BIOS screen, and writes assets/intro_*.png with
the antialiasing baked into fifteen opaque grey levels ramping from the SCE
screen's white down to the ink. Index 0 stays fully transparent, so the sprite
still has no background box around it.

Sixteen distinct colours exactly: the most 4bpp can hold, and the most
tools/convertImage.py will accept. Do not add a level.

    python3 tools/make_intro_wordmarks.py

Requires Pillow. Run from the repository root; overwrites assets/intro_*.png.
"""

import os
import sys

from PIL import Image

# The field the antialiasing blends toward, and the ink it blends from.
#
# FIELD is the SCE screen's background colour, and the ramp is baked against
# it, so these sprites belong on that exact field and nowhere else. Both are
# sampled off the BIOS reference: a mid grey, not white, with navy text rather
# than black. Change these two and re-run - the intro's own SCE_FIELD in
# src/main/intro_ps1.c has to be changed to match, or the sprites will sit in
# visible boxes.
FIELD = (168, 168, 168)
INK   = ( 18,  45,  80)

LEVELS  = 15   # non-transparent palette entries; 15 + transparent = 16
SUBSAMP = 6    # samples per axis inside each destination pixel

# master, output, artwork w/h, padded texture w/h.
#
# Artwork sizes are measured off a real BIOS screen. The old ones came from
# the CSS remake and were wrong in a way that showed: SONY was 160 wide, wider
# than the diamond, where the real one is narrower than it; and COMPUTER was
# 16 tall against ENTERTAINMENT's 11, so the two lines did not match each
# other and collided on screen.
#
# Padded sizes satisfy both upload constraints at once - width a multiple of
# 16, height a multiple of 8. See the note on the size constants in
# src/main/intro_ps1.c for why a short DMA transfer is fatal rather than
# merely wrong.
JOBS = [
    ("orig_intro_sony.png",     "intro_sony.png",     115, 19, 128, 24),
    ("orig_intro_computer.png", "intro_computer.png", 120, 11, 128, 16),
    ("orig_intro_enter.png",    "intro_enter.png",    124, 10, 128, 16),
    ("orig_intro_tm.png",       "intro_tm.png",        14,  7,  16, 16),
]


def sample(mask, w, h, fx, fy):
    """Bilinear sample of a 0..1 mask, clamped to transparent outside."""
    fx -= 0.5
    fy -= 0.5
    x0 = int(fx // 1)
    y0 = int(fy // 1)
    tx = fx - x0
    ty = fy - y0

    acc = 0.0
    for j in (0, 1):
        for i in (0, 1):
            sx, sy = x0 + i, y0 + j
            v = mask[sy * w + sx] if (0 <= sx < w and 0 <= sy < h) else 0.0
            acc += v * (tx if i else 1 - tx) * (ty if j else 1 - ty)
    return acc


def resample(src, dw, dh, pw, ph):
    """Box-filter `src`'s alpha down to dw x dh, into a pw x ph RGBA image."""
    w, h = src.size
    mask = [a / 255.0 for a in src.convert("RGBA").getdata(3)]

    out = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    px  = out.load()

    xs = w / dw
    ys = h / dh
    n  = SUBSAMP * SUBSAMP

    for y in range(dh):
        for x in range(dw):
            acc = 0.0
            for j in range(SUBSAMP):
                fy = (y + (j + 0.5) / SUBSAMP) * ys
                for i in range(SUBSAMP):
                    fx = (x + (i + 0.5) / SUBSAMP) * xs
                    acc += sample(mask, w, h, fx, fy)

            level = round(acc / n * LEVELS)
            if level <= 0:
                continue

            # Opaque, not semi-transparent: the PS1 has no per-texel alpha
            # ramp, only a single semi-transparency bit, so the blend toward
            # the background has to be baked into the colour itself.
            px[x, y] = tuple(
                round(f + (i - f) * level / LEVELS) for f, i in zip(FIELD, INK)
            ) + (255,)

    return out


def main():
    root   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets = os.path.join(root, "assets")

    for master, name, dw, dh, pw, ph in JOBS:
        path = os.path.join(assets, master)
        if not os.path.exists(path):
            sys.exit(f"missing master: {path}")

        with Image.open(path) as src:
            out = resample(src, dw, dh, pw, ph)

        colors = len(set(out.getdata()))
        if colors > 16:
            sys.exit(f"{name}: {colors} colours, 4bpp holds 16")

        out.save(os.path.join(assets, name))
        print(f"{name}: {dw}x{dh} artwork in {pw}x{ph}, {colors} colours")


if __name__ == "__main__":
    main()
