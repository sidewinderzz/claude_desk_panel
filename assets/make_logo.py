#!/usr/bin/env python3
"""
Rasterise the Claude mark from its SVG path into an LVGL 8 image asset.

Emits LV_IMG_CF_ALPHA_8BIT: one byte of coverage per pixel, no colour. LVGL draws
it using the widget's `img_recolor` style, so the mark follows the theme accent
instead of having a colour baked in, and it costs a third of what TRUE_COLOR_ALPHA
would (1 byte per pixel instead of 3).

Stdlib only - the SVG path is parsed and scan-converted here rather than pulling in
a renderer. The source path is straight segments plus a single cubic, so the parser
handles M/L/C/Z (absolute) and flattens the cubic with de Casteljau.

    python make_logo.py [path/to/claude-ai.svg]

Writes claude_logo.c, claude_logo.h, claude_logo_preview.png.
"""

import os
import re
import sys
import zlib

SIZE = 32          # output is SIZE x SIZE px
SUPERSAMPLE = 8    # rasterise at SIZE*SUPERSAMPLE then box-downsample
MARGIN = 0.0       # extra padding inside the square, in output px
BEZIER_STEPS = 24  # segments per cubic
NAME = "claude_logo"

DEFAULT_SVG = os.path.join(os.path.expanduser("~"), "Downloads", "claude-ai.svg")


# ----------------------------------------------------------------- svg path ---

def extract_path(svg_text):
    m = re.search(r'\sd="([^"]+)"', svg_text)
    if not m:
        raise SystemExit("no path data (d=...) found in the SVG")
    return m.group(1)


def tokenize(d):
    """Yield ('CMD', [numbers...]) for absolute M/L/C/Z."""
    tokens = re.findall(r"[MmLlCcZzHhVv]|-?\d*\.?\d+(?:[eE][-+]?\d+)?", d)
    i = 0
    cmd = None
    while i < len(tokens):
        t = tokens[i]
        if re.match(r"[A-Za-z]", t):
            cmd = t
            i += 1
            if cmd in "Zz":
                yield ("Z", [])
                continue
        if cmd is None:
            raise SystemExit("path data started without a command")
        n = {"M": 2, "L": 2, "C": 6, "H": 1, "V": 1}[cmd.upper()]
        try:
            args = [float(tokens[i + k]) for k in range(n)]
        except (IndexError, ValueError):
            raise SystemExit("truncated path data near token %d" % i)
        i += n
        yield (cmd, args)
        # A repeated M implies L for subsequent coordinate pairs, per the SVG spec.
        if cmd == "M":
            cmd = "L"
        elif cmd == "m":
            cmd = "l"


def cubic(p0, p1, p2, p3, steps):
    out = []
    for s in range(1, steps + 1):
        t = s / steps
        u = 1.0 - t
        x = (u * u * u * p0[0] + 3 * u * u * t * p1[0]
             + 3 * u * t * t * p2[0] + t * t * t * p3[0])
        y = (u * u * u * p0[1] + 3 * u * u * t * p1[1]
             + 3 * u * t * t * p2[1] + t * t * t * p3[1])
        out.append((x, y))
    return out


def path_to_contours(d):
    """Flatten the path into a list of closed contours (lists of points)."""
    contours, cur = [], []
    pos = (0.0, 0.0)
    start = (0.0, 0.0)
    for cmd, a in tokenize(d):
        up = cmd.upper()
        rel = cmd.islower()
        if up == "Z":
            if len(cur) > 2:
                contours.append(cur)
            cur = []
            pos = start
            continue
        if up == "M":
            if len(cur) > 2:
                contours.append(cur)
            pos = (pos[0] + a[0], pos[1] + a[1]) if rel else (a[0], a[1])
            start = pos
            cur = [pos]
        elif up == "L":
            pos = (pos[0] + a[0], pos[1] + a[1]) if rel else (a[0], a[1])
            cur.append(pos)
        elif up == "H":
            pos = (pos[0] + a[0], pos[1]) if rel else (a[0], pos[1])
            cur.append(pos)
        elif up == "V":
            pos = (pos[0], pos[1] + a[0]) if rel else (pos[0], a[0])
            cur.append(pos)
        elif up == "C":
            if rel:
                p1 = (pos[0] + a[0], pos[1] + a[1])
                p2 = (pos[0] + a[2], pos[1] + a[3])
                p3 = (pos[0] + a[4], pos[1] + a[5])
            else:
                p1, p2, p3 = (a[0], a[1]), (a[2], a[3]), (a[4], a[5])
            cur.extend(cubic(pos, p1, p2, p3, BEZIER_STEPS))
            pos = p3
    if len(cur) > 2:
        contours.append(cur)
    return contours


# ------------------------------------------------------------- rasteriser ---

def rasterise(contours, size, ss, margin):
    """Scan-convert to an alpha map, nonzero winding, box-downsampled."""
    xs = [p[0] for c in contours for p in c]
    ys = [p[1] for c in contours for p in c]
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    w, h = maxx - minx, maxy - miny

    # Fit the mark's bounding box into the square, preserving aspect and centring.
    avail = size - 2 * margin
    scale = avail / max(w, h)
    ox = margin + (avail - w * scale) / 2.0 - minx * scale
    oy = margin + (avail - h * scale) / 2.0 - miny * scale

    edges = []  # (y0, y1, x_at_y0, dx/dy, winding)
    for c in contours:
        pts = [(p[0] * scale + ox, p[1] * scale + oy) for p in c]
        pts.append(pts[0])
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            if y0 == y1:
                continue
            wind = 1 if y1 > y0 else -1
            if y0 > y1:
                x0, y0, x1, y1 = x1, y1, x0, y0
            edges.append((y0, y1, x0, (x1 - x0) / (y1 - y0), wind))

    hi = size * ss
    cov = [0] * (hi * hi)
    for sy in range(hi):
        yc = (sy + 0.5) / ss
        xs_hits = []
        for (y0, y1, x0, slope, wind) in edges:
            if y0 <= yc < y1:
                xs_hits.append((x0 + (yc - y0) * slope, wind))
        if not xs_hits:
            continue
        xs_hits.sort()
        acc = 0
        row = sy * hi
        for i in range(len(xs_hits) - 1):
            acc += xs_hits[i][1]
            if acc == 0:
                continue
            xa = int(xs_hits[i][0] * ss + 0.5)
            xb = int(xs_hits[i + 1][0] * ss + 0.5)
            if xb <= 0 or xa >= hi:
                continue
            for px in range(max(0, xa), min(hi, xb)):
                cov[row + px] = 1

    # Box-downsample the supersampled coverage into 0..255 alpha.
    alpha = bytearray(size * size)
    n = ss * ss
    for y in range(size):
        for x in range(size):
            total = 0
            for sy in range(y * ss, (y + 1) * ss):
                base = sy * hi + x * ss
                total += sum(cov[base:base + ss])
            alpha[y * size + x] = (total * 255 + n // 2) // n
    return alpha


# ------------------------------------------------------------------ output ---

def write_png(path, size, alpha, rgb, zoom=6):
    """Preview: the mark on dark, on light, and as a raw alpha ramp.

    Nearest-neighbour zoomed so the edge quality is actually inspectable; a 32 px
    tile on a screenshot tells you nothing about the anti-aliasing.
    """
    tiles = [(0x1F, 0x1E, 0x1D), (0xF0, 0xEE, 0xE6), None]
    tile = size * zoom
    gap = 10
    w = tile * len(tiles) + gap * (len(tiles) + 1)
    h = tile + gap * 2
    rows = []
    for y in range(h):
        row = bytearray([0x80] * (w * 3))
        ty = (y - gap) // zoom
        if 0 <= ty < size and gap <= y < gap + tile:
            for t, bg in enumerate(tiles):
                x0 = gap + t * (tile + gap)
                for xz in range(tile):
                    x = xz // zoom
                    a = alpha[ty * size + x]
                    if bg is None:
                        c = (a, a, a)
                    else:
                        c = tuple((rgb[i] * a + bg[i] * (255 - a)) // 255 for i in range(3))
                    o = (x0 + xz) * 3
                    row[o:o + 3] = bytes(c)
        rows.append(bytes(row))

    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        return (len(data).to_bytes(4, "big") + tag + data
                + (zlib.crc32(tag + data) & 0xFFFFFFFF).to_bytes(4, "big"))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", w.to_bytes(4, "big") + h.to_bytes(4, "big")
                   + bytes([8, 2, 0, 0, 0]))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def write_c(path, name, size, alpha):
    lines = []
    for i in range(0, len(alpha), 16):
        lines.append("    " + " ".join("0x%02x," % b for b in alpha[i:i + 16]))
    body = "\n".join(lines)
    src = f'''/*
 * {name} - the Claude mark, {size}x{size}, LV_IMG_CF_ALPHA_8BIT.
 *
 * Generated by assets/make_logo.py from the source SVG. Do not edit by hand.
 *
 * One byte of coverage per pixel and no colour: LVGL paints it with the widget's
 * `img_recolor` style, so it follows the theme rather than baking the accent in.
 *
 *     lv_obj_t *img = lv_img_create(parent);
 *     lv_img_set_src(img, &{name});
 *     lv_obj_set_style_img_recolor(img, C_ACCENT, 0);
 *     lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
 */

#include "{name}.h"

#ifndef LV_ATTRIBUTE_IMG_{name.upper()}
#define LV_ATTRIBUTE_IMG_{name.upper()}
#endif

static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST
    LV_ATTRIBUTE_IMG_{name.upper()} uint8_t {name}_map[] = {{
{body}
}};

const lv_img_dsc_t {name} = {{
    .header.cf = LV_IMG_CF_ALPHA_8BIT,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = {size},
    .header.h = {size},
    .data_size = {len(alpha)},
    .data = {name}_map,
}};
'''
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)


def write_h(path, name):
    guard = name.upper() + "_H"
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f'''#ifndef {guard}
#define {guard}

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {{
#endif

extern const lv_img_dsc_t {name};

#ifdef __cplusplus
}}
#endif

#endif /* {guard} */
''')


def main():
    svg_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SVG
    if not os.path.exists(svg_path):
        raise SystemExit("SVG not found: %s" % svg_path)
    with open(svg_path, "r", encoding="utf-8") as f:
        text = f.read()

    fill = re.search(r'fill="#([0-9a-fA-F]{6})"', text)
    rgb = tuple(int(fill.group(1)[i:i + 2], 16) for i in (0, 2, 4)) if fill else (0xD9, 0x77, 0x57)

    contours = path_to_contours(extract_path(text))
    pts = sum(len(c) for c in contours)
    print("  %d contour(s), %d points, source fill #%02x%02x%02x" % (len(contours), pts, *rgb))

    alpha = rasterise(contours, SIZE, SUPERSAMPLE, MARGIN)
    here = os.path.dirname(os.path.abspath(__file__))
    write_c(os.path.join(here, NAME + ".c"), NAME, SIZE, alpha)
    write_h(os.path.join(here, NAME + ".h"), NAME)
    write_png(os.path.join(here, NAME + "_preview.png"), SIZE, alpha, rgb)

    nonzero = sum(1 for a in alpha if a)
    solid = sum(1 for a in alpha if a == 255)
    print("  %dx%d, %d bytes, %d px painted (%d fully opaque)"
          % (SIZE, SIZE, len(alpha), nonzero, solid))
    print("  wrote %s.c, %s.h, %s_preview.png" % (NAME, NAME, NAME))


if __name__ == "__main__":
    main()
