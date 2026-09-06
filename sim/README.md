# sim/ — headless LVGL host renderer

Renders `ui/ui.c` on the development machine and writes an 800x480 PNG, so UI
work does not need a flash-and-squint cycle on the ESP32.

There is no display, no window system, no SDL and no real clock. LVGL draws
into a plain `malloc()`ed framebuffer, the tick is a `uint32_t` stepped by hand,
and the result is written straight out as a PNG. A given scenario renders
byte-identically on every run, which makes the shots diffable.

## Build and run

There is no gcc/make on the Windows side, so everything compiles inside WSL.

**From PowerShell (the easy path):**

```powershell
cd C:\path\to\claude_desk_panel\sim
.\run.ps1                 # build + regenerate every screenshot into shots\
.\run.ps1 -Clean          # wipe build\ first
.\run.ps1 -SimArgs '--page home --theme light --out shots/home-light.png'
```

**From PowerShell, driving WSL directly:**

```powershell
wsl -d Ubuntu -- bash -lc "cd /mnt/c/path/to/claude_desk_panel/sim && make -j8 && make shots"
```

**Inside WSL:**

```bash
cd /mnt/c/path/to/claude_desk_panel/sim
make -j8            # build ./sim
make shots          # regenerate every scenario into shots/
make info           # show which UI source and LVGL tree are in use
make clean          # remove build/ and ./sim
make distclean      # also remove shots/
./sim --help
```

Nothing under `Documents/Arduino/libraries/` is modified — LVGL and `lv_conf.h`
are consumed exactly as the firmware sees them, via `-I` and
`-DLV_CONF_INCLUDE_SIMPLE`. If that tree ever moves:

```bash
make ARDUINO_LIBS=/some/other/libraries
```

## Flags

| Flag | Values | Meaning |
| --- | --- | --- |
| `--page` | `usage` \| `home` \| `settings` | Which page to load. Default `usage`. |
| `--theme` | `dark` \| `light` | Colour scheme, applied both before and after `ui_init()`. Default `dark`. |
| `--state` | `ok` \| `stale` \| `nowifi` \| `bridge` \| `limit` | Connection/data state. Drives `ui_set_status()` and reshapes the fake usage and Home Assistant payloads. Default `ok`. |
| `--wifi` | (no value) | Show the Wi-Fi setup screen with a fake scan list instead of a page. |
| `--dim N` | `0`–`70` | Software dim overlay percentage. Clamped. Default `0`. |
| `--out PATH` | path | PNG to write. Parent directories are created. Default `shots/out.png`. |
| `--help` | | Usage text. |

### What each `--state` feeds in

| State | `ui_set_status()` | Usage | Home Assistant |
| --- | --- | --- | --- |
| `ok` | `CONNECTED`, 12s | 62% session / 41% week / 79% model, sources `api`, `api-scaled`, `calibrated` | valid, Living Room 21.5→22.0 heating, 3 lights |
| `stale` | `STALE`, 930s | same | valid |
| `nowifi` | `NO_WIFI`, 3600s | same; SSID and IP blanked | invalid, with a message |
| `bridge` | `BRIDGE_OFFLINE`, 244s | same | invalid, with a message |
| `limit` | `LIMIT_REACHED`, 8s | `blocked = true`, session pinned at 100%, resets in 45m | valid |

## Files

| File | Purpose |
| --- | --- |
| `main.c` | The harness: display driver, simulated clock, fake data, CLI, settle loop, framebuffer → PNG. |
| `png_write.c` / `.h` | Dependency-free PNG encoder. |
| `ui_stub.c` | **Temporary.** Minimal `ui.h` implementation used only when `../ui/ui.c` does not exist. Safe to delete now that the real UI is in place. |
| `Makefile` | Compiles all 192 LVGL `.c` files + `../ui/*.c` + the harness. |
| `run.ps1` | Windows wrapper around the WSL build. |
| `shots/` | Generated PNGs. |
| `build/` | Object files, mirroring the source tree. |

## How it works

**Display.** One `lv_disp_drv_t` at 800x480 with a full-screen draw buffer and
`full_refresh = 1`, so LVGL hands over whole frames rather than a patchwork of
dirty rectangles. `flush_cb` blits into a separate persistent framebuffer and
calls `lv_disp_flush_ready()` immediately. Both buffers are plain `malloc()` —
deliberately *not* LVGL's heap, which `lv_conf.h` caps at 128 KB for the ESP32
and which a 768 KB framebuffer would instantly exhaust.

**Clock.** `LV_TICK_CUSTOM` is `0` in `lv_conf.h`, so LVGL expects
`lv_tick_inc()` to be called externally. The settle loop steps it by 30 ms
(matching `LV_DISP_DEF_REFR_PERIOD`) and passes the same counter to `ui_tick()`.
Nothing reads a real clock, so runs are reproducible.

**Settling.** After the data is pushed in, the loop pumps
`lv_tick_inc` + `ui_tick` + `lv_timer_handler` until six consecutive iterations
produce no flush, capped at 80 iterations (~2.4 s of simulated time). The
screenshot is taken after that, so theme transitions and arc animations have
finished.

**PNG.** RGB565 → RGB888 via `lv_color_to32()` (which does the 5/6/5 expansion
with LVGL's own rounding), then a hand-rolled encoder. A PNG's IDAT is a zlib
stream, and zlib allows "stored" DEFLATE blocks — a 5-byte header plus literal
bytes. So a valid PNG needs no compressor at all, only CRC-32 for the chunks and
Adler-32 for the zlib trailer. The files come out ~1.1 MB instead of ~30 KB,
which is irrelevant for screenshots.

## Gotchas hit along the way

- **`lv_conf.h` caps LVGL's heap at 128 KB** (`LV_MEM_SIZE`), sized for the
  ESP32. That is fine for the widget tree but nowhere near a framebuffer, hence
  the plain `malloc()`. If you add a heavy page and the sim dies inside
  `lv_mem_alloc`, that is a *real* signal that it will not fit on the device
  either — do not "fix" it by raising `LV_MEM_SIZE` here.
- **`LV_ASSERT_HANDLER` is `while(1);`.** A failed LVGL assertion (a NULL object,
  a malloc failure) does not crash — it hangs silently and forever. If a
  scenario stops producing output, it is almost certainly an assert, not a slow
  render. Run it under `timeout 60 ./sim ...` and attach gdb to the spin.
- **Only montserrat 14/20/28/48 are compiled in.** Referencing any other size
  fails at link time, not compile time, with an undefined `lv_font_montserrat_NN`.
- **`--state limit` never goes quiet.** The "LIMIT REACHED" label animates
  continuously, so the settle loop hits its 80-iteration cap and prints a
  warning. That is expected and the shot is still correct; the warning is only
  there to catch *unintended* runaway animations.
- **Build as C, not C++.** LVGL 8 is C99 and uses designated initialisers and
  implicit `void*` conversions that a C++ compiler rejects.
- **Windows/WSL line endings.** `Makefile` and the sources must stay LF. A CRLF
  `Makefile` fails in WSL with a confusing `missing separator` error.
- **Vendored LVGL is compiled with `-w`.** It is noisy under `-Wextra` and is not
  ours to patch. `main.c`, `png_write.c` and `../ui/*.c` still get
  `-Wall -Wextra`.
- **`make` picks the UI source automatically.** `../ui/*.c` if present, otherwise
  `ui_stub.c` — never both, which would be duplicate symbols. `make info` says
  which one is in play.

## Verifying a PNG

`make shots` only proves the encoder did not crash. To prove the bytes are a
real PNG, decode them with something that is not our code:

```bash
cd shots && python3 -c "
import struct, zlib, binascii, sys
d = open(sys.argv[1],'rb').read()
assert d[:8] == b'\x89PNG\r\n\x1a\n'
off, idat = 8, b''
while off < len(d):
    ln, = struct.unpack('>I', d[off:off+4]); typ = d[off+4:off+8]
    data = d[off+8:off+8+ln]
    crc, = struct.unpack('>I', d[off+8+ln:off+12+ln])
    assert crc == binascii.crc32(typ+data) & 0xffffffff, typ
    if typ == b'IDAT': idat += data
    off += 12 + ln
raw = zlib.decompress(idat)   # also validates the adler32 trailer
print('ok', len(raw), 'bytes of scanlines')
" usage-dark.png
```

That checks every chunk CRC, and `zlib.decompress` validates the stored-block
`LEN`/`NLEN` pairs and the Adler-32 trailer. All twelve shots pass.
