# `burst_img` — the burst logo mark as an LVGL 8 image asset

A 40x40 anti-aliased "burst" mark: 12 rays radiating from an inner radius to an
outer radius, tapering from narrow at the centre to wide at the tip, with round
caps, in Claude clay `#D97757`.

Replaces 12 hand-placed `lv_line` objects. See [Tradeoffs](#tradeoffs-vs-12-lv_line-objects)
for whether that is actually a good idea (short version: yes, mostly for the taper).

| File | What it is |
| --- | --- |
| `make_burst.py` | The generator. Standard library only — no PIL, no numpy. |
| `burst_img.c` | Generated LVGL image. **Do not hand-edit.** |
| `burst_img.h` | Generated header, declares `extern const lv_img_dsc_t burst_img;` |
| `burst_preview.png` | Generated contact sheet, for eyeballing without a board. |

## Regenerating

```
cd assets
python make_burst.py
```

No arguments, no dependencies (Python 3.x, stdlib `math` + `zlib` + `struct`).
Edit the parameter block at the top of `make_burst.py` and re-run. It rewrites
all three generated files and prints a summary:

```
burst_img: 40x40, 12 spokes, #D97757
  inner r 3.20 w 1.00 -> outer r 16.60 w 4.40
  painted extent 18.80 px of 20.0 px half-canvas
  gap between ray tips: 4.29 px
  asset: 4800 bytes (1600 px x 3)
```

Always look at `burst_preview.png` after changing anything. It shows the mark at
8x over dark and light backgrounds, the alpha channel alone as greyscale (the
best way to judge edge quality without colour in the way), and 1x/4x strips for
a real-size sanity check.

## Parameters

All at the top of `make_burst.py`.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `SIZE` | `40` | Output is `SIZE` x `SIZE` px. Also the flash cost driver: `SIZE² x 3` bytes. |
| `SPOKES` | `12` | Number of rays. |
| `COLOR` | `0xD97757` | `0xRRGGBB`. Baked into every pixel — see the caveat below. |
| `ROTATION` | `0.0` | Degrees clockwise. `0` puts a ray at 12 o'clock. |
| `SUPERSAMPLE` | `4` | Render scale before box-downsampling. |
| `REF_SIZE` | `40.0` | Reference size the four geometry values below are expressed at. |
| `INNER_R` | `3.2` | Radius of the ray's narrow endpoint, px from centre. |
| `OUTER_R` | `16.6` | Radius of the ray's wide endpoint, px from centre. |
| `WIDTH_INNER` | `1.0` | Full ray width at `INNER_R`. |
| `WIDTH_OUTER` | `4.4` | Full ray width at `OUTER_R`. |

The four geometry values are scaled by `SIZE / REF_SIZE`, so **changing `SIZE`
alone regenerates a proportionally identical mark** — you do not have to rescale
the radii by hand.

Because the caps are round, the *painted* extent is `OUTER_R + WIDTH_OUTER/2`
(18.8 px at the defaults, in a 20 px half-canvas — 1.2 px of margin) and the
narrow end pokes inward to `INNER_R - WIDTH_INNER/2`. The script warns if the
painted extent would be clipped by the canvas.

### How it is rendered

Each ray is an *uneven capsule* — a 2D signed distance field for a segment whose
radius varies linearly from `r1` at one end to `r2` at the other, with round
caps and straight tangent sides. The mark is the `min()` of the 12 fields.

Anti-aliasing is not plain boolean supersampling. At each of the
`SUPERSAMPLE x SUPERSAMPLE` subsamples, coverage is derived analytically from
the signed distance (a linear ramp one subsample wide across the edge), then the
block is box-downsampled. Plain boolean 4x supersampling would give only 17
distinct alpha levels; this gives 57 in the current output, and visibly smoother
edges at 40 px.

### Rendering at the size you actually display

The header slot is 30x30. **Set `SIZE = 30` and regenerate** rather than shipping
the 40 px asset and calling `lv_img_set_zoom(img, 192)`. Native rendering is
crisper (the AA is computed for the real pixel grid instead of resampled), and
it costs 2700 bytes instead of 4800. LVGL's zoom path also spends CPU per redraw
that a native-size asset does not. The 40 px default is just the reference size.

## Byte format

`LV_IMG_CF_TRUE_COLOR_ALPHA` at `LV_COLOR_DEPTH 16` / `LV_COLOR_16_SWAP 0`:
**3 bytes per pixel, row-major, top-left first.**

```
byte 0:  RGB565 low  byte   (gggbbbbb)
byte 1:  RGB565 high byte   (rrrrrggg)
byte 2:  alpha, 0 = transparent .. 255 = opaque
```

That is a little-endian `uint16_t` of `(r5 << 11) | (g6 << 5) | b5`, followed by
`A8`. For `#D97757` every pixel's colour bytes are `0xAA, 0xDB`; only the alpha
byte varies.

Why this layout — verified against LVGL 8.4 in
`libraries/lvgl/src/misc/lv_color.h` and `src/draw/lv_img_buf.h`:

- With `LV_COLOR_16_SWAP 0`, `lv_color_t`'s bitfields are declared
  `blue:5, green:6, red:5` — LSB first — over a `uint16_t full`, so
  `full == (r5<<11)|(g6<<5)|b5`.
- LVGL reads that `uint16_t` straight out of the image map, so on a
  little-endian MCU (Xtensa / RISC-V ESP32) the **low byte comes first**.
- `LV_IMG_PX_SIZE_ALPHA_BYTE` is 3 at `LV_COLOR_DEPTH 16`, which is what
  `.data_size` is expressed in.

Colour is stored **non-premultiplied** — full-strength `#D97757` in every pixel,
including fully transparent ones — which is what LVGL expects and which keeps
the anti-aliased edges from picking up a dark fringe.

`burst_img.c` carries `#error` guards on both `LV_COLOR_DEPTH` and
`LV_COLOR_16_SWAP`. If either changes, the build fails loudly instead of
rendering a blue-and-green mark. Regenerate rather than patching by hand.

## Flash cost

| | Bytes |
| --- | --- |
| 40x40 (default) | **4800** (1600 px x 3) |
| 30x30 | 2700 (900 px x 3) |

Plus ~24 bytes for the `lv_img_dsc_t`. The map is `const`, so it lives in flash
(`.rodata`) and costs **zero RAM** — see below.

## Using it

Add `burst_img.c` to the build and include the header:

```c
#include "burst_img.h"

lv_obj_t *img = lv_img_create(parent);
lv_img_set_src(img, &burst_img);
lv_obj_align(img, LV_ALIGN_LEFT_MID, 12, 0);
```

Nothing else is required — no decoder registration, no zoom, no recolour.

### Caveat: the colour is baked in

`LV_IMG_CF_TRUE_COLOR_ALPHA` stores the RGB value in every pixel, so **a theme
change cannot recolour this asset.** `lv_obj_set_style_img_recolor()` can tint
it, but tinting a baked colour is a blend, not a swap — you cannot get a clean
arbitrary hue out of it, and mixing toward a distant hue goes muddy.

For this design that is acceptable: the accent is constant `#D97757` in both
light and dark, so there is nothing to re-theme. **If that ever stops being
true**, the options are (a) regenerate per colour — 4.8 KB each — or (b) switch
the format to `LV_IMG_CF_ALPHA_8BIT`, which stores alpha only at 1 byte/px
(1600 bytes, a third the flash) and takes its colour from the object's
`img_recolor` style, making it fully themeable. Option (b) is strictly better if
recolouring is ever needed; it is not the default here only because
`TRUE_COLOR_ALPHA` was specified.

## Tradeoffs vs 12 `lv_line` objects

**Fidelity — the actual reason to switch.** `lv_line` draws constant-width
segments. It *can* anti-alias, and it *can* do round caps
(`LV_STYLE_LINE_ROUNDED`), so those are not the differentiators. What it cannot
do at any setting is **taper** — the ray cannot be 1.0 px wide at the inner end
and 4.4 px at the tip. Everything else about the current 12-line version is
reproducible; the taper is not. If the taper does not matter to the design, the
rest of this comparison is close enough to a wash that you should keep the
lines.

**Flash.** Costs 4800 bytes (2700 at 30 px) that the lines do not. The lines'
point arrays are ~96 bytes of `.rodata` if declared `static const`. Both
approaches pull in code that this UI almost certainly already links
(`lv_draw_line` vs `lv_img` + `lv_draw_img`), so the code-size delta is roughly
zero either way. On an ESP32-S3 with multi-MB flash, 4.8 KB is noise.

**RAM.** Modest win for the image. Each `lv_obj` is roughly 50–60 bytes of
struct plus a heap-allocated style array plus allocator overhead plus a slot in
the parent's child array — call it ~80–100 bytes of heap each, so ~1 KB for 12
lines versus ~100 bytes for one `lv_img`. (Measure it on your build with
`sizeof(lv_obj_t)` rather than trusting that estimate.) The image map itself
costs no RAM at all: `lv_img_decoder_open` sees `LV_IMG_SRC_VARIABLE` +
`TRUE_COLOR_ALPHA` and hands back `dsc->data` directly — no decode buffer is
allocated and LVGL blits straight from flash.

**Redraw.** Small win for the image, and not the deciding factor. Twelve line
objects mean twelve widget draw passes, each building an anti-aliasing mask,
against one image draw that blits 1600 pixels with a per-pixel alpha blend.
Their invalidated areas all overlap the same 30x30 box and LVGL joins them, so
the invalidation cost is near-identical. Neither matters much for a header mark
that only redraws when something behind it changes. One caveat on this platform:
blitting from flash pulls the map through the instruction cache, so the image is
not quite as free as blitting from PSRAM would be.

**Maintainability.** Clear win for the image. Twelve objects means twelve sets of
coordinates, styles and parent-relative offsets to keep in sync, and `lv_line`
stores a *pointer* to your point array without copying it — a well-known
lifetime footgun if the array is ever a local. The image is one object, one
`lv_img_set_src`, and the geometry lives in a re-runnable script with a visual
preview instead of in hand-computed trig in UI code.

**Verdict.** Switch if you want the taper; that is a real design improvement
`lv_line` cannot deliver, and 4.8 KB of flash is a cheap price for it on this
board. Do not switch on performance grounds alone — the redraw difference is
marginal. The one thing you genuinely give up is runtime recolouring, which this
design does not need.
