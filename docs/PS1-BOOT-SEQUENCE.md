# PS1 boot sequence — extracted from the HTML remake

Source: `PS1_Startup_Remake-1.0.0` (HTML/CSS/jQuery). This is the exact
timeline read out of `js/startup.js`, with geometry from `css/sce.css` and
`css/ps.css`, converted to the dashboard's 320x240 screen and 60Hz frames.

## Timeline

`startup.js` drives everything with `setTimeout`, so the sequence is exact
rather than inferred. Times are from the moment the animation starts.

| ms | frame @60Hz | Event |
|---:|---:|---|
| 0 | 0 | Background animates black → **white** over 998ms |
| 1000 | 60 | SCE diamond (`sce_logo_background`) fades in over 100ms |
| 1150 | 69 | Two triangles appear and animate for 850ms — shrinking from 25%×50% to 10%×20% while converging on the centre |
| 2000 | 120 | SONY wordmark, "COMPUTER / ENTERTAINMENT", and ™ fade in over 300ms |
| 7000 | 420 | Whole SCE screen fades out over 100ms |
| 7030 | 422 | Background animates white → **black** over 200ms |
| 7250 | 435 | PS logo fades in over 450ms |
| 7700 | 462 | Credits text fades in over 50ms |
| 7750 | 465 | "PlayStation™" wordmark fades in over 400ms |
| 15500 | 930 | Everything fades out over 150ms |
| 16650 | 999 | Sequence ends |

**Total: ~16.65 seconds.** Two distinct screens: the white SCE logo screen
(0–7s) and the black PlayStation screen (7.25–15.5s).

## Geometry, converted to 320x240

The CSS lays everything out in percentages of a square `#screen` (`100vmin`),
so on a 320x240 display the reference square is **240x240, centred** — i.e.
x from 40 to 280. All the percentage positions below are relative to that
square, not to the full width.

**SCE screen (white background):**

| Element | CSS | 320x240 equivalent |
|---|---|---|
| Diamond | 50% × 50%, centred | 120×120 at (100, 60) |
| Diamond shape | `polygon(50% 0, 100% 50%, 50% 100%, 0 50%)` | rhombus — 4 points, one quad |
| Diamond fill | `linear-gradient(90deg, #E01705 0%, #DF9300 50%, #E01705 100%)` | red → amber → red, horizontal |
| Triangle start | 25% × 50% | 60×120 |
| Triangle end | 10% × 20% | 24×48 |
| SONY wordmark | top 5%, width 50% | 120 wide at y≈12 |
| Bottom text | bottom 5.5%, 7.8vmin / 5.27vmin | ~19px / ~13px tall |

**PlayStation screen (black background):**

| Element | CSS | 320x240 equivalent |
|---|---|---|
| PS logo | top 16%, width 40% | 96 wide at y≈38 |
| "PlayStation™" | top 47%, 7vmin | y≈113, ~17px tall |
| Credits | top 61%, 3vmin | y≈146, ~7px tall |

## What maps cleanly to this hardware, and what does not

**Cleanly:**

- The diamond and both triangles are flat 4-point polygons with a horizontal
  colour gradient — exactly what `gouraudQuad()` already draws. The triangle
  convergence is pure interpolation of four corner positions per frame.
- Both logos are now real assets, rasterised from the project's own SVGs (see
  below) and quantised to hard-edged 4bpp: `intro_sony.png` (160×28, 2
  colours) and `intro_pslogo.png` (90×82, 5 colours). Together they cost
  about 6 KB of RAM.

**Does not:**

- **Fades.** The GPU's semi-transparent blend is a fixed 50% mix, so a fade
  has five usable steps (50%, 25%, 12.5%...) rather than a smooth ramp. The
  black→white background transition at the start is the worst case, because
  it fades *up* to white rather than down to black, and the same trick does
  not work in that direction — the honest options are a hard cut, a coarse
  5-step ramp, or a small dither texture.
- **Fonts.** "COMPUTER ENTERTAINMENT" is Helvetica Neue Extended and
  "PlayStation™" is Zrnic. The dashboard's bitmap font is neither. Either
  those two words get rasterised into the same kind of asset the logos use,
  or they get drawn in the existing font and simply look different.

## Correction: the SCE wordmarks

The geometry table above is what the CSS remake implies, and for the two
wordmarks it is wrong. Measured against a real BIOS screen:

| Element | This table said | Actually |
|---|---|---|
| Diamond | 120x120 at (100, 60) | 127x135, centred at (160, 117) |
| SONY | 120 wide at y≈12 | 115x19 at y=21 — *narrower* than the diamond |
| COMPUTER | ~19px tall | 120x11 at y=194 |
| ENTERTAINMENT | ~13px tall | 124x10 at y=207 |

The old SONY was 160 wide, wider than the diamond; the real one is narrower.
COMPUTER was 16 tall against ENTERTAINMENT's 11, so the two lines did not
match each other and collided on screen.

They were also two-colour art — one transparent index, one ink index — which
wastes fourteen of a 4bpp palette's sixteen entries and leaves every curve a
bare staircase. **That, not resolution, is why they looked pixelated next to
the BIOS.** They are now generated antialiased by
`tools/make_intro_wordmarks.py` from the one-bit masters in
`assets/orig_intro_*.png`, with fifteen opaque grey levels ramping from the
screen's white down to the ink and index 0 left transparent. The PS1 has no
per-texel alpha, only a single semi-transparency bit, so the blend toward the
background has to be baked into the colours — which means these sprites are
correct on a white field and nowhere else.

## Assets already produced

`tools/svg2png.py` renders the two SVGs without any external rasteriser
(none is available here — no cairosvg, and ImageMagick's SVG delegate needs
rsvg-convert). It implements just enough of the SVG path spec for these two
files: `M, L, H, V, C, S, A, Z`, with arcs converted via the standard
endpoint-to-centre formula, curves flattened to line segments, and even-odd
fill so the counters inside S/O/N and the PS mark punch through correctly.

Both outputs were checked by eye and then verified through the project's own
`tools/convertImage.py` at 4bpp.

## Remaining work

1. VRAM slots for the two logo textures, and their `addBinaryFile` entries.
2. `intro_ps1.c`: the state machine above, driving polygons and textures.
3. A decision on the two non-bitmap-font strings.
4. The startup sound (`snd/ps1_startup.wav`) is 16-bit PCM and would need
   converting to VAG and fitting in SPU RAM alongside the existing BGM and
   SFX — worth treating as a separate step from the visuals.
