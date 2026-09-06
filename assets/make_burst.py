#!/usr/bin/env python3
"""
make_burst.py -- generate the Claude "burst" logo mark as an LVGL 8 C image asset.

Dependency free: standard library only (math + zlib). No PIL, no numpy.

Outputs, next to this script:
    burst_img.c        LVGL 8 image, LV_IMG_CF_TRUE_COLOR_ALPHA (RGB565 LE + A8)
    burst_img.h        extern const lv_img_dsc_t burst_img;
    burst_preview.png  contact sheet so the mark can be eyeballed without a board

Target: LVGL 8.4, LV_COLOR_DEPTH 16, LV_COLOR_16_SWAP 0 (little-endian MCU).

Run:
    python make_burst.py
"""

import math
import os
import struct
import zlib

# ---------------------------------------------------------------------------
# Parameters -- everything you would want to tweak lives here.
# ---------------------------------------------------------------------------

SIZE = 30  # output is SIZE x SIZE pixels
SPOKES = 12  # number of rays
COLOR = 0xD97757  # Claude clay, 0xRRGGBB
ROTATION = 0.0  # degrees, clockwise; 0 puts a ray at 12 o'clock
SUPERSAMPLE = 4  # render at SIZE*SUPERSAMPLE, then box-downsample

# Ray geometry, in pixels at REF_SIZE. All four are rescaled by SIZE/REF_SIZE,
# so changing SIZE alone regenerates a proportionally identical mark.
REF_SIZE = 40.0
INNER_R = 2.0  # radius of the ray's inner (narrow) endpoint, px from centre
OUTER_R = 17.2  # radius of the ray's outer (wide) endpoint, px from centre
WIDTH_INNER = 0.7  # ray width at INNER_R (full width, px)
WIDTH_OUTER = 2.1  # ray width at OUTER_R (full width, px)

C_NAME = "burst_img"  # symbol name used in the .c/.h

# Ray ends are round caps, so the painted extent is OUTER_R + WIDTH_OUTER/2
# and the narrow end pokes in to INNER_R - WIDTH_INNER/2.

_K = SIZE / REF_SIZE
INNER_R *= _K
OUTER_R *= _K
WIDTH_INNER *= _K
WIDTH_OUTER *= _K

HERE = os.path.dirname(os.path.abspath(__file__))


# ---------------------------------------------------------------------------
# Geometry
# ---------------------------------------------------------------------------


def sd_uneven_capsule(px, py, r1, r2, h):
    """Signed distance to a capsule running from (0,0) with radius r1 to (0,h)
    with radius r2. Negative inside. Straight tapered sides, round caps.

    Standard 2D "uneven capsule" distance (Inigo Quilez formulation)."""
    px = abs(px)
    b = (r1 - r2) / h
    a2 = 1.0 - b * b
    if a2 <= 0.0:
        # Degenerate: one cap swallows the other. Fall back to the larger disc.
        return math.hypot(px, py if r1 >= r2 else py - h) - max(r1, r2)
    a = math.sqrt(a2)
    k = -b * px + a * py
    if k < 0.0:
        return math.hypot(px, py) - r1
    if k > a * h:
        return math.hypot(px, py - h) - r2
    return a * px + b * py - r1


def burst_sdf(x, y, spokes):
    """Signed distance to the union of all rays. `spokes` is a precomputed list
    of (cos, sin) unit vectors along each ray."""
    r1 = WIDTH_INNER * 0.5
    r2 = WIDTH_OUTER * 0.5
    h = OUTER_R - INNER_R
    best = 1e9
    for cx, sy in spokes:
        # Local frame: +v runs outward along the ray, +u is perpendicular.
        # Ray body starts at INNER_R along the direction.
        v = x * cx + y * sy - INNER_R
        u = -x * sy + y * cx
        d = sd_uneven_capsule(u, v, r1, r2, h)
        if d < best:
            best = d
    return best


def render_alpha():
    """Render the mark and return a SIZE*SIZE list of 0..255 alpha values.

    Coverage is computed analytically from the signed distance at each
    supersample (a half-pixel-wide linear ramp across the edge), then the
    SUPERSAMPLE x SUPERSAMPLE block is box-downsampled. Distance-based coverage
    plus the box filter gives much smoother edges than boolean supersampling,
    which would only ever produce SUPERSAMPLE^2 + 1 distinct alpha levels.
    """
    ss = SUPERSAMPLE
    n = SIZE * ss
    step = 1.0 / ss  # size of one subsample in output-pixel units
    centre = SIZE * 0.5

    rot = math.radians(ROTATION)
    spokes = []
    for i in range(SPOKES):
        # -90deg so index 0 points up; +rot clockwise on screen (y grows down).
        ang = rot + i * 360.0 / SPOKES
        a = math.radians(ang) - math.pi / 2.0
        spokes.append((math.cos(a), math.sin(a)))

    # Accumulate coverage per output pixel.
    acc = [0.0] * (SIZE * SIZE)
    inv = 1.0 / (ss * ss)
    # Coverage ramp: 1 at d = -step/2, 0 at d = +step/2.
    for sy in range(n):
        y = (sy + 0.5) * step - centre
        oy = sy // ss
        row_base = oy * SIZE
        for sx in range(n):
            x = (sx + 0.5) * step - centre
            d = burst_sdf(x, y, spokes)
            if d <= -step * 0.5:
                c = 1.0
            elif d >= step * 0.5:
                continue
            else:
                c = 0.5 - d / step
            acc[row_base + (sx // ss)] += c

    return [max(0, min(255, int(round(a * inv * 255.0)))) for a in acc]


# ---------------------------------------------------------------------------
# LVGL C output
# ---------------------------------------------------------------------------


def rgb565(color):
    r = (color >> 16) & 0xFF
    g = (color >> 8) & 0xFF
    b = color & 0xFF
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def pack_true_color_alpha(alpha):
    """LV_IMG_CF_TRUE_COLOR_ALPHA @ LV_COLOR_DEPTH 16 / LV_COLOR_16_SWAP 0.

    lv_color_t is a union over a uint16_t `full` whose bitfields are declared
    blue:5, green:6, red:5 (LSB first), so `full` == (r5<<11)|(g6<<5)|b5.
    LVGL memcpy's that uint16_t straight out of the image map, so on a
    little-endian MCU (Xtensa/RISC-V ESP32) the low byte comes first.
    Then one alpha byte. 3 bytes per pixel.
    """
    v = rgb565(COLOR)
    lo = v & 0xFF
    hi = (v >> 8) & 0xFF
    out = bytearray()
    for a in alpha:
        out.append(lo)
        out.append(hi)
        out.append(a)
    return bytes(out)


C_TEMPLATE = """/* Generated by assets/make_burst.py -- DO NOT EDIT BY HAND.
 *
 * {name}: {size}x{size} burst mark, {spokes} tapered rays, colour #{color:06X}.
 * Format: LV_IMG_CF_TRUE_COLOR_ALPHA, 3 bytes/px (RGB565 little-endian + A8).
 * Requires LV_COLOR_DEPTH 16 and LV_COLOR_16_SWAP 0 -- the pixel bytes are
 * baked for that layout, so the guard below is not optional.
 */

#include "{name}.h"

#if LV_COLOR_DEPTH != 16
#error "{name} is baked for LV_COLOR_DEPTH 16. Regenerate with assets/make_burst.py."
#endif
#if LV_COLOR_16_SWAP != 0
#error "{name} is baked for LV_COLOR_16_SWAP 0. Regenerate with assets/make_burst.py."
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_CONST
#endif

#ifndef LV_ATTRIBUTE_IMG_{upper}
#define LV_ATTRIBUTE_IMG_{upper}
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_{upper}
uint8_t {name}_map[] = {{
{body}}};

const lv_img_dsc_t {name} = {{
    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = {size},
    .header.h = {size},
    .data_size = {size} * {size} * LV_IMG_PX_SIZE_ALPHA_BYTE,
    .data = {name}_map,
}};
"""

H_TEMPLATE = """/* Generated by assets/make_burst.py -- DO NOT EDIT BY HAND. */
#ifndef {upper}_H
#define {upper}_H

#ifdef __cplusplus
extern "C" {{
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
#include "lvgl.h"
#elif defined(LV_BUILD_TEST)
#include "../lvgl.h"
#else
#include <lvgl.h>
#endif

/* {size}x{size}, LV_IMG_CF_TRUE_COLOR_ALPHA, {bytes} bytes of flash.
 *
 *   lv_obj_t *img = lv_img_create(parent);
 *   lv_img_set_src(img, &{name});
 */
extern const lv_img_dsc_t {name};

#ifdef __cplusplus
}} /* extern "C" */
#endif

#endif /* {upper}_H */
"""


def write_c(data):
    lines = []
    per_row = 12  # 4 pixels per source line
    for i in range(0, len(data), per_row):
        chunk = data[i : i + per_row]
        lines.append("    " + " ".join("0x%02X," % b for b in chunk))
    body = "\n".join(lines) + "\n"

    c = C_TEMPLATE.format(
        name=C_NAME,
        upper=C_NAME.upper(),
        size=SIZE,
        spokes=SPOKES,
        color=COLOR,
        body=body,
    )
    h = H_TEMPLATE.format(
        name=C_NAME, upper=C_NAME.upper(), size=SIZE, bytes=len(data)
    )
    with open(os.path.join(HERE, C_NAME + ".c"), "w", newline="\n") as f:
        f.write(c)
    with open(os.path.join(HERE, C_NAME + ".h"), "w", newline="\n") as f:
        f.write(h)


# ---------------------------------------------------------------------------
# Minimal PNG encoder (stdlib zlib only) + preview contact sheet
# ---------------------------------------------------------------------------


def write_png(path, width, height, rgb_rows):
    """rgb_rows: list of `height` bytes objects, each width*3 bytes, RGB8."""
    raw = bytearray()
    for row in rgb_rows:
        raw.append(0)  # filter type 0 (None)
        raw.extend(row)

    def chunk(tag, payload):
        out = struct.pack(">I", len(payload)) + tag + payload
        return out + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)


def composite(alpha, bg, zoom):
    """Composite the mark over a solid background at integer `zoom`, returning
    (w, h, rows). Nearest-neighbour zoom so anti-aliasing is inspected as-is."""
    fr = (COLOR >> 16) & 0xFF
    fg = (COLOR >> 8) & 0xFF
    fb = COLOR & 0xFF
    br, bgc, bb = bg
    w = h = SIZE * zoom
    rows = []
    for y in range(h):
        sy = y // zoom
        row = bytearray()
        for x in range(w):
            a = alpha[sy * SIZE + (x // zoom)]
            row.append((fr * a + br * (255 - a) + 127) // 255)
            row.append((fg * a + bgc * (255 - a) + 127) // 255)
            row.append((fb * a + bb * (255 - a) + 127) // 255)
        rows.append(bytes(row))
    return w, h, rows


def write_preview(alpha, path):
    """Contact sheet: 1x and 8x, over a dark and a light background, plus an
    8x alpha-only ramp so edge quality is judged without colour in the way."""
    dark = (0x1F, 0x1E, 0x1D)
    light = (0xF5, 0xF2, 0xEE)
    gap = 12
    sheet_bg = (0x80, 0x80, 0x80)

    tiles = []
    tiles.append(composite(alpha, dark, 8))
    tiles.append(composite(alpha, light, 8))
    # Alpha as greyscale, 8x.
    w = h = SIZE * 8
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            a = alpha[(y // 8) * SIZE + (x // 8)]
            row += bytes((a, a, a))
        rows.append(bytes(row))
    tiles.append((w, h, rows))

    # 1x strip: dark, light, repeated so real size is visible.
    small = [composite(alpha, dark, 1), composite(alpha, light, 1),
             composite(alpha, dark, 4), composite(alpha, light, 4)]

    big_h = max(t[1] for t in tiles)
    total_w = sum(t[0] for t in tiles) + gap * (len(tiles) + 1)
    small_h = max(t[1] for t in small)
    total_h = gap + big_h + gap + small_h + gap

    out = []
    blank = bytes(sheet_bg) * total_w
    for y in range(total_h):
        row = bytearray(blank)
        if gap <= y < gap + big_h:
            ty = y - gap
            x0 = gap
            for tw, th, trows in tiles:
                if ty < th:
                    row[x0 * 3 : (x0 + tw) * 3] = trows[ty]
                x0 += tw + gap
        elif gap + big_h + gap <= y < gap + big_h + gap + small_h:
            ty = y - (gap + big_h + gap)
            x0 = gap
            for tw, th, trows in small:
                if ty < th:
                    row[x0 * 3 : (x0 + tw) * 3] = trows[ty]
                x0 += tw + gap
        out.append(bytes(row))

    write_png(path, total_w, total_h, out)


# ---------------------------------------------------------------------------


def main():
    tip = OUTER_R + WIDTH_OUTER * 0.5
    if tip > SIZE * 0.5:
        print(
            "WARNING: painted extent %.2f px exceeds half the canvas (%.1f px);"
            " rays will be clipped." % (tip, SIZE * 0.5)
        )

    alpha = render_alpha()
    data = pack_true_color_alpha(alpha)
    write_c(data)
    write_preview(alpha, os.path.join(HERE, "burst_preview.png"))

    gap = 2.0 * math.pi * OUTER_R / SPOKES - WIDTH_OUTER
    print("%s: %dx%d, %d spokes, #%06X" % (C_NAME, SIZE, SIZE, SPOKES, COLOR))
    print("  inner r %.2f w %.2f -> outer r %.2f w %.2f" % (INNER_R, WIDTH_INNER, OUTER_R, WIDTH_OUTER))
    print("  painted extent %.2f px of %.1f px half-canvas" % (tip, SIZE * 0.5))
    print("  gap between ray tips: %.2f px" % gap)
    print("  asset: %d bytes (%d px x 3)" % (len(data), SIZE * SIZE))
    print("  wrote burst_img.c, burst_img.h, burst_preview.png")


if __name__ == "__main__":
    main()
