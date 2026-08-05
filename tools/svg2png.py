#!/usr/bin/env python3
"""Rasterise the two PS1 startup logos to PNG.

No SVG rasteriser is available in this environment (no cairosvg, and
ImageMagick's SVG delegate needs rsvg-convert), so this implements just
enough of the SVG path spec to render these two specific files:

    ps_logo.svg    M, L, C, z          - the PlayStation "PS" mark
    sony_logo.svg  M, V, a, c, h, l,   - the SONY wordmark
                   m, s, v, z

Curves are flattened to line segments and every subpath is filled with PIL's
polygon fill using the even-odd rule, which is what these files expect (the
counters inside S, O, N and the PS mark are separate subpaths).

Deliberately not a general SVG renderer - no strokes, no transforms beyond
the single translate() these files use, no gradients. It only has to be
right for these two inputs, and the output is checked by eye before use.
"""

import math
import re
import sys

from PIL import Image, ImageDraw

NUM = re.compile(r'[-+]?(?:\d*\.\d+|\d+)(?:[eE][-+]?\d+)?')
CMD = re.compile(r'([MmLlHhVvCcSsQqTtAaZz])')


def tokenize(d):
    """Yield (command, [numbers]) pairs."""
    parts = CMD.split(d)
    out = []
    i = 1
    while i < len(parts):
        cmd = parts[i]
        nums = [float(n) for n in NUM.findall(parts[i + 1])]
        out.append((cmd, nums))
        i += 2
    return out


def arc_to_points(x0, y0, rx, ry, phi_deg, large_arc, sweep, x1, y1, steps=24):
    """SVG endpoint arc -> polyline. Standard endpoint-to-centre conversion."""
    if rx == 0 or ry == 0 or (x0 == x1 and y0 == y1):
        return [(x1, y1)]

    phi = math.radians(phi_deg)
    cos_p, sin_p = math.cos(phi), math.sin(phi)

    dx2, dy2 = (x0 - x1) / 2.0, (y0 - y1) / 2.0
    x1p = cos_p * dx2 + sin_p * dy2
    y1p = -sin_p * dx2 + cos_p * dy2

    rx, ry = abs(rx), abs(ry)
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1:
        s = math.sqrt(lam)
        rx, ry = rx * s, ry * s

    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    factor = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large_arc == sweep:
        factor = -factor

    cxp = factor * rx * y1p / ry
    cyp = -factor * ry * x1p / rx

    cx = cos_p * cxp - sin_p * cyp + (x0 + x1) / 2.0
    cy = sin_p * cxp + cos_p * cyp + (y0 + y1) / 2.0

    def angle(ux, uy):
        n = math.hypot(ux, uy)
        a = math.acos(max(-1.0, min(1.0, ux / n))) if n else 0.0
        return -a if uy < 0 else a

    theta1 = angle((x1p - cxp) / rx, (y1p - cyp) / ry)
    dtheta = angle((-x1p - cxp) / rx, (-y1p - cyp) / ry) - theta1

    if not sweep and dtheta > 0:
        dtheta -= 2 * math.pi
    elif sweep and dtheta < 0:
        dtheta += 2 * math.pi

    pts = []
    for i in range(1, steps + 1):
        th = theta1 + dtheta * i / steps
        px = cos_p * rx * math.cos(th) - sin_p * ry * math.sin(th) + cx
        py = sin_p * rx * math.cos(th) + cos_p * ry * math.sin(th) + cy
        pts.append((px, py))
    return pts


def cubic(p0, p1, p2, p3, steps=16):
    pts = []
    for i in range(1, steps + 1):
        t = i / steps
        mt = 1 - t
        x = (mt ** 3 * p0[0] + 3 * mt * mt * t * p1[0]
             + 3 * mt * t * t * p2[0] + t ** 3 * p3[0])
        y = (mt ** 3 * p0[1] + 3 * mt * mt * t * p1[1]
             + 3 * mt * t * t * p2[1] + t ** 3 * p3[1])
        pts.append((x, y))
    return pts


def path_to_subpaths(d):
    """Return a list of point lists, one per subpath."""
    subpaths = []
    cur = []
    x = y = 0.0
    start = (0.0, 0.0)
    prev_c2 = None

    for cmd, nums in tokenize(d):
        rel = cmd.islower()
        c = cmd.upper()
        i = 0

        if c == 'M':
            while i < len(nums):
                nx, ny = nums[i], nums[i + 1]
                if rel:
                    nx, ny = x + nx, y + ny
                if i == 0:
                    if cur:
                        subpaths.append(cur)
                    cur = [(nx, ny)]
                    start = (nx, ny)
                else:
                    cur.append((nx, ny))
                x, y = nx, ny
                i += 2
            prev_c2 = None

        elif c == 'L':
            while i < len(nums):
                nx, ny = nums[i], nums[i + 1]
                if rel:
                    nx, ny = x + nx, y + ny
                cur.append((nx, ny))
                x, y = nx, ny
                i += 2
            prev_c2 = None

        elif c == 'H':
            for n in nums:
                nx = x + n if rel else n
                cur.append((nx, y))
                x = nx
            prev_c2 = None

        elif c == 'V':
            for n in nums:
                ny = y + n if rel else n
                cur.append((x, ny))
                y = ny
            prev_c2 = None

        elif c == 'C':
            while i < len(nums):
                c1 = (nums[i], nums[i + 1])
                c2 = (nums[i + 2], nums[i + 3])
                p = (nums[i + 4], nums[i + 5])
                if rel:
                    c1 = (x + c1[0], y + c1[1])
                    c2 = (x + c2[0], y + c2[1])
                    p = (x + p[0], y + p[1])
                cur += cubic((x, y), c1, c2, p)
                prev_c2 = c2
                x, y = p
                i += 6

        elif c == 'S':
            while i < len(nums):
                c2 = (nums[i], nums[i + 1])
                p = (nums[i + 2], nums[i + 3])
                if rel:
                    c2 = (x + c2[0], y + c2[1])
                    p = (x + p[0], y + p[1])
                c1 = (2 * x - prev_c2[0], 2 * y - prev_c2[1]) if prev_c2 else (x, y)
                cur += cubic((x, y), c1, c2, p)
                prev_c2 = c2
                x, y = p
                i += 4

        elif c == 'A':
            while i < len(nums):
                rx, ry, rot = nums[i], nums[i + 1], nums[i + 2]
                laf, sw = int(nums[i + 3]), int(nums[i + 4])
                nx, ny = nums[i + 5], nums[i + 6]
                if rel:
                    nx, ny = x + nx, y + ny
                cur += arc_to_points(x, y, rx, ry, rot, laf, sw, nx, ny)
                x, y = nx, ny
                i += 7
            prev_c2 = None

        elif c == 'Z':
            if cur:
                cur.append(start)
                subpaths.append(cur)
                cur = []
            x, y = start
            prev_c2 = None

    if cur:
        subpaths.append(cur)
    return subpaths


def parse_translate(svg_text):
    m = re.search(r'transform="translate\(([-\d.]+)[ ,]+([-\d.]+)\)"', svg_text)
    return (float(m.group(1)), float(m.group(2))) if m else (0.0, 0.0)


def render(svg_path, out_path, out_w, colour=(255, 255, 255, 255), supersample=4):
    text = open(svg_path).read()

    vb = re.search(r'viewBox="([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)"', text)
    vx, vy, vw, vh = (float(g) for g in vb.groups())

    tx, ty = parse_translate(text)

    out_h = max(1, round(out_w * vh / vw))
    W, H = out_w * supersample, out_h * supersample
    scale = W / vw

    # Even-odd fill accumulated as a boolean mask: each subpath is XORed in,
    # so the counters inside S/O/N and the PS mark punch back through.
    import numpy as np

    acc = np.zeros((H, W), dtype=bool)

    for pd in re.findall(r'\sd="([^"]+)"', text):
        for sub in path_to_subpaths(pd):
            if len(sub) < 3:
                continue
            pts = [(((px + tx) - vx) * scale, ((py + ty) - vy) * scale)
                   for px, py in sub]
            layer = Image.new('L', (W, H), 0)
            ImageDraw.Draw(layer).polygon(pts, fill=255)
            acc ^= (np.array(layer) > 127)

    alpha = Image.fromarray((acc * 255).astype('uint8'), 'L')

    solid = Image.new('RGBA', (W, H), colour)
    solid.putalpha(alpha)
    solid = solid.resize((out_w, out_h), Image.LANCZOS)
    solid.save(out_path)
    print(f"{out_path}: {out_w}x{out_h}")
    return solid


if __name__ == '__main__':
    src = sys.argv[1]
    dst = sys.argv[2]
    width = int(sys.argv[3])
    render(src, dst, width)
