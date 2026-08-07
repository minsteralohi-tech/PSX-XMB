#!/usr/bin/env python3
"""Re-cut the SCE wordmarks for the boot intro, antialiased.

The intro's SONY / COMPUTER / ENTERTAINMENT / (TM) art started life as
one-bit masks: one transparent index, one ink index, nothing in between. At
4bpp that leaves fourteen of the sixteen palette entries unused and every
diagonal and curve as a bare staircase - which is precisely why the wordmarks
looked pixelated next to the real BIOS screen. The BIOS art is antialiased.
That, not resolution, is the difference.

This reads the masters in assets/orig_intro_*.png - high-resolution artwork,
around ten times the final size, with the shape in the alpha channel -
resamples them to the sizes measured off a real BIOS screen, and writes
assets/intro_*.png with the antialiasing baked into fifteen opaque levels
ramping from the field colour to the ink. Index 0 stays fully transparent, so
the sprite still has no background box around it.

assets/orig_intro_sony.png is itself generated, from assets/sony_logo.svg -
tools/svg2png.py renders it. The other three are supplied artwork.

Sixteen distinct colours exactly: the most 4bpp can hold, and the most
tools/convertImage.py will accept. Do not add a level.

    python3 tools/make_intro_wordmarks.py

Requires Pillow. Run from the repository root; overwrites assets/intro_*.png.
"""

import math
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

# The PlayStation screen is the other way round: white artwork on black. Its
# one wordmark uses these instead.
PS_FIELD = (0, 0, 0)
PS_INK   = (255, 255, 255)

LEVELS = 15    # non-transparent palette entries; 15 + transparent = 16

# Samples per axis inside each destination pixel. Chosen per job from the
# scale factor rather than fixed: the masters are around 10x the target, so one
# output pixel covers over a hundred source pixels and a fixed 6x6 would miss
# most of them - thin strokes would flicker in and out depending on where the
# samples happened to land.
SUBSAMP_MIN = 6
SUBSAMP_MAX = 16

# Edge contrast. Coverage is pushed away from the middle before it is
# quantised: c' = (c - 0.5) * CONTRAST + 0.5, clamped. 1.0 leaves it linear.
#
# Above 1.0 narrows the antialiased band - fewer half-lit pixels, more that are
# fully ink or fully field - which reads as sharper. Worth it where the artwork
# is set against a strong contrast and the soft band shows as a halo, which is
# what made the PlayStation wordmark look blurry next to the dashboard's own
# hard-edged bitmap font. Not worth it on the SCE wordmarks: navy on grey is a
# gentle enough step that the linear ramp already looks clean, and hardening it
# would just put the staircase back.
CONTRAST_SOFT = 1.0
CONTRAST_HARD = 2.2

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
    ("orig_intro_sony.png",     "intro_sony.png",     115, 19, 128, 24,
     FIELD, INK, CONTRAST_SOFT),
    ("orig_intro_computer.png", "intro_computer.png", 120, 11, 128, 16,
     FIELD, INK, CONTRAST_SOFT),
    ("orig_intro_enter.png",    "intro_enter.png",    124, 10, 128, 16,
     FIELD, INK, CONTRAST_SOFT),
    ("orig_intro_tm.png",       "intro_tm.png",        14,  7,  16, 16,
     FIELD, INK, CONTRAST_SOFT),
    # The PlayStation wordmark, on the black screen rather than the SCE one.
    #
    # Wider than it used to be: at 80 across, this logotype's strokes landed
    # around two pixels and most of the word was antialiasing rather than ink.
    # 96 is closer to what the BIOS screen shows and gives the letterforms
    # enough pixels to be crisp; the master's aspect is preserved.
    ("orig_intro_pstext.png",   "intro_pstext.png",    96, 21,  96, 24,
     PS_FIELD, PS_INK, CONTRAST_HARD),
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


def resample(src, dw, dh, pw, ph, field, ink, contrast):
    """Box-filter `src`'s alpha down to dw x dh, into a pw x ph RGBA image."""
    w, h = src.size
    mask = [a / 255.0 for a in src.convert("RGBA").getdata(3)]

    out = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    px  = out.load()

    xs = w / dw
    ys = h / dh
    sub = min(SUBSAMP_MAX, max(SUBSAMP_MIN, math.ceil(max(xs, ys))))
    n = sub * sub

    for y in range(dh):
        for x in range(dw):
            acc = 0.0
            for j in range(sub):
                fy = (y + (j + 0.5) / sub) * ys
                for i in range(sub):
                    fx = (x + (i + 0.5) / sub) * xs
                    acc += sample(mask, w, h, fx, fy)

            cov = acc / n
            if contrast != 1.0:
                cov = min(1.0, max(0.0, (cov - 0.5) * contrast + 0.5))

            level = round(cov * LEVELS)
            if level <= 0:
                continue

            # Opaque, not semi-transparent: the PS1 has no per-texel alpha
            # ramp, only a single semi-transparency bit, so the blend toward
            # the background has to be baked into the colour itself.
            px[x, y] = tuple(
                round(f + (i - f) * level / LEVELS) for f, i in zip(field, ink)
            ) + (255,)

    return out


def main():
    root   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets = os.path.join(root, "assets")

    for master, name, dw, dh, pw, ph, field, ink, contrast in JOBS:
        path = os.path.join(assets, master)
        if not os.path.exists(path):
            sys.exit(f"missing master: {path}")

        with Image.open(path) as src:
            # Downsampling a mask recovers antialiasing; upsampling one only
            # blurs it. A master smaller than the target is a master that
            # needs replacing, not converting - say so and leave the existing
            # asset alone rather than making it worse.
            if src.width < dw or src.height < dh:
                print(
                    f"{name}: SKIPPED - master is {src.width}x{src.height}, "
                    f"smaller than the {dw}x{dh} target. Replace {master} "
                    f"with a higher-resolution one."
                )
                continue

            out = resample(src, dw, dh, pw, ph, field, ink, contrast)

        colors = len(set(out.getdata()))
        if colors > 16:
            sys.exit(f"{name}: {colors} colours, 4bpp holds 16")

        out.save(os.path.join(assets, name))
        print(f"{name}: {dw}x{dh} artwork in {pw}x{ph}, {colors} colours")


if __name__ == "__main__":
    main()
