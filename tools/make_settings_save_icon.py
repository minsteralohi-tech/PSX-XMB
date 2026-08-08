#!/usr/bin/env python3
"""Rasterise assets/settings_save.svg for the XMB and PS1 save frame.

The checked-in SVG uses only text, one quadratic stroke and a hard offset
shadow.  Reproducing those exact primitives with Pillow keeps this generator
self-contained on Windows (where no SVG delegate is installed), while the SVG
remains the editable source of truth.  Output colours are deliberately hard
edged and limited so the combined XMB sheet still fits a PS1 4bpp CLUT.
"""

from pathlib import Path
import math
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"

TRANSPARENT = (255, 255, 255, 0)
SHADOW = (51, 51, 51, 255)
RED = (224, 0, 36, 255)
YELLOW = (243, 195, 0, 255)
BLUE = (0, 114, 206, 255)
GREEN = (0, 166, 80, 255)


def quadratic_points(p0, p1, p2, steps=80):
    points = []
    for i in range(steps + 1):
        t = i / steps
        mt = 1.0 - t
        points.append((
            mt * mt * p0[0] + 2 * mt * t * p1[0] + t * t * p2[0],
            mt * mt * p0[1] + 2 * mt * t * p1[1] + t * t * p2[1],
        ))
    return points


def render_master():
    image = Image.new("RGBA", (800, 400), TRANSPARENT)
    draw = ImageDraw.Draw(image)
    font_path = Path(r"C:\Windows\Fonts\trebucbd.ttf")
    if not font_path.exists():
        font_path = Path(r"C:\Windows\Fonts\arialbd.ttf")
    font = ImageFont.truetype(str(font_path), 200)

    labels = ((140, "X", RED), (310, "M", YELLOW), (520, "B", BLUE))
    for x, text, _ in labels:
        draw.text((x + 12, 260 + 12), text, font=font, fill=SHADOW, anchor="ls")
    for x, text, colour in labels:
        draw.text((x, 260), text, font=font, fill=colour, anchor="ls")

    curve = quadratic_points((130, 310), (380, 370), (650, 300))
    shadow_curve = [(x + 12, y + 12) for x, y in curve]
    draw.line(shadow_curve, fill=SHADOW, width=22, joint="curve")
    draw.line(curve, fill=GREEN, width=22, joint="curve")
    # SVG square linecaps.
    for points, colour in ((shadow_curve, SHADOW), (curve, GREEN)):
        r = 11
        for x, y in (points[0], points[-1]):
            draw.rectangle((x - r, y - r, x + r, y + r), fill=colour)

    # Pillow antialiases TrueType glyph edges even when the requested fill is
    # opaque. Collapse those partial-alpha edge pixels back to the nearest
    # authored SVG colour: the target hardware has one 16-colour CLUT for the
    # entire item sheet, and the desired artwork is intentionally pixel-hard.
    authored = (SHADOW, RED, YELLOW, BLUE, GREEN)
    pixels = image.load()
    for y in range(image.height):
        for x in range(image.width):
            pixel = pixels[x, y]
            if pixel[3] < 96:
                pixels[x, y] = TRANSPARENT
                continue
            pixels[x, y] = min(
                authored,
                key=lambda c: sum((pixel[channel] - c[channel]) ** 2
                                  for channel in range(3)),
            )
    return image


def nearest_fit(source, size, margin):
    box = source.getchannel("A").getbbox()
    cropped = source.crop(box)
    max_w = size[0] - margin * 2
    max_h = size[1] - margin * 2
    scale = min(max_w / cropped.width, max_h / cropped.height)
    target = (
        max(1, round(cropped.width * scale)),
        max(1, round(cropped.height * scale)),
    )
    cropped = cropped.resize(target, Image.Resampling.NEAREST)
    out = Image.new("RGBA", size, TRANSPARENT)
    out.alpha_composite(cropped, ((size[0] - target[0]) // 2,
                                  (size[1] - target[1]) // 2))
    return out


def nearest_stretch(source, size, margin):
    """Fill a tiny square save-icon frame while retaining the full design."""
    box = source.getchannel("A").getbbox()
    cropped = source.crop(box).resize(
        (size[0] - margin * 2, size[1] - margin * 2),
        Image.Resampling.NEAREST,
    )
    out = Image.new("RGBA", size, TRANSPARENT)
    out.alpha_composite(cropped, (margin, margin))
    return out


def main():
    # Fail loudly if the editable source is accidentally removed.
    svg = ASSETS / "settings_save.svg"
    if not svg.exists() or "BASLUS" in svg.read_text(encoding="utf-8"):
        raise RuntimeError("settings_save.svg is missing or unexpected")

    master = render_master()
    dashboard = nearest_fit(master, (64, 64), 2)
    # PS1 save icons are only 16x16. A proportional reduction leaves this
    # wide mark just six pixels tall, so expand it vertically inside that
    # dedicated frame; all three letters and the green sweep remain intact.
    mc_icon = nearest_stretch(master, (16, 16), 1)

    dashboard.save(ASSETS / "settings_save_icon.png")
    mc_icon.save(ASSETS / "settings_mc_icon.png")

    old = Image.open(ASSETS / "icons_item.png").convert("RGBA")
    if old.size not in ((256, 192), (256, 256)):
        raise RuntimeError(f"unexpected icons_item.png size: {old.size}")
    sheet = Image.new("RGBA", (256, 192), TRANSPARENT)
    sheet.alpha_composite(old.crop((0, 0, 256, 192)), (0, 0))
    sheet.alpha_composite(dashboard, (192, 128))  # spare item index 11
    colours = sheet.getcolors(maxcolors=17)
    if colours is None or len(colours) > 16:
        raise RuntimeError("settings icon pushes icons_item.png beyond 16 colours")
    sheet.save(ASSETS / "icons_item.png")

    print("generated settings_save_icon.png (64x64), settings_mc_icon.png "
          "(16x16), and icons_item.png (256x192, item 11)")


if __name__ == "__main__":
    main()
