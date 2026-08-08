#!/usr/bin/env python3
"""Keep the dashboard font shadows and inline controller glyphs consistent.

The original font atlas had hand-authored one-pixel shadows on most glyphs,
but several lowercase letters (notably r/s/t/u) never received them. Rebuild
the shadow from the white foreground for every printable ASCII cell so every
screen using printString() gets the same result.

The pad tester owns the final controller-button artwork. Its sheet is extended
with the standalone navigation D-pad supplied for dashboard prompts; font.c
draws all inline controller bytes from this shared sheet rather than carrying
a second, lower-quality copy in font.png.
"""

from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
FONT_PATH = ROOT / "assets" / "font.png"
PAD_PATH = ROOT / "assets" / "padglyphs.png"
DPAD_PATH = ROOT / "assets" / "dpad_glyph.png"

TRANSPARENT = (0, 0, 0, 0)
FOREGROUND = (255, 255, 255, 255)
SHADOW = (33, 33, 33, 127)


def rebuild_font_shadows() -> None:
	font = Image.open(FONT_PATH).convert("RGBA")
	if font.size != (96, 64):
		raise RuntimeError(f"unexpected font atlas size: {font.size}")

	pixels = font.load()
	for code in range(33, 128):
		cell = code - 32
		x0 = (cell % 16) * 6
		y0 = (cell // 16) * 9

		# Delete only the old shadow; foreground and special antialias colours
		# remain untouched. The controller-art row starts at y=54 and is not
		# part of this loop.
		for y in range(y0, y0 + 9):
			for x in range(x0, x0 + 6):
				if pixels[x, y] == SHADOW:
					pixels[x, y] = TRANSPARENT

		foreground = [
			(x, y)
			for y in range(y0, y0 + 9)
			for x in range(x0, x0 + 6)
			if pixels[x, y] == FOREGROUND
		]
		for x, y in foreground:
			sx, sy = x + 1, y + 1
			if sx < x0 + 6 and sy < y0 + 9 and pixels[sx, sy][3] == 0:
				pixels[sx, sy] = SHADOW

	font.save(FONT_PATH)


def extend_pad_sheet() -> None:
	pad = Image.open(PAD_PATH).convert("RGBA")
	if pad.height != 64 or pad.width not in (144, 168):
		raise RuntimeError(f"unexpected pad glyph atlas size: {pad.size}")

	canvas = Image.new("RGBA", (168, 64), TRANSPARENT)
	canvas.paste(pad.crop((0, 0, 144, 64)), (0, 0))

	dpad = Image.open(DPAD_PATH).convert("RGBA")
	if dpad.size != (19, 19):
		raise RuntimeError(f"unexpected navigation D-pad size: {dpad.size}")

	# Transparent RGB is irrelevant to the PS1 but counts as another palette
	# entry during conversion. Normalize it so the combined sheet stays 4bpp.
	dpad_pixels = dpad.load()
	for y in range(dpad.height):
		for x in range(dpad.width):
			if dpad_pixels[x, y][3] == 0:
				dpad_pixels[x, y] = TRANSPARENT

	canvas.paste(dpad, (144, 0))
	colors = set(canvas.get_flattened_data())
	if len(colors) > 16:
		raise RuntimeError(f"combined pad glyph atlas has {len(colors)} colors")

	canvas.save(PAD_PATH)


if __name__ == "__main__":
	rebuild_font_shadows()
	extend_pad_sheet()
