# Claude Desk Panel

A desk display for a **Waveshare ESP32-S3-Touch-LCD-4.3B** (800×480 RGB, GT911 touch):
how much of your Claude usage window you've burned through, plus Home Assistant
controls. Three pages, swipe between them.

```
┌──────────────────────────────────────────────────────────┐
│ ✳ Claude Usage                          connected  ●     │
├──────────────────────────────────────────────────────────┤
│         5-HOUR SESSION        ┌──────────────────────┐   │
│            ╭─────────╮        │ WEEKLY  ALL MODELS   │   │
│          ╭─┤   62%   ├─╮      │ 41%                  │   │
│          │ ╰─────────╯ │      │ ▓▓▓▓▓▓▓░░░░░░░░░░░░  │   │
│          ╰─           ─╯      │ resets in 3d 0h      │   │
│         resets in 1h 22m      └──────────────────────┘   │
│                               ┌──────────────────────┐   │
│                               │ WEEKLY  FABLE        │   │
│                               │ 79%  ▓▓▓▓▓▓▓▓▓▓▓▓░░  │   │
│                               └──────────────────────┘   │
│                    ●  ○  ○                               │
└──────────────────────────────────────────────────────────┘
   Claude Usage           Office              Settings
```

| | |
| --- | --- |
| `bridge/` | Python, stdlib only. Serves usage JSON on the LAN and proxies Home Assistant. |
| `ui/` | The entire UI. Pure LVGL 8 / C99 — no Arduino, no ESP-IDF. |
| `sim/` | Headless renderer: builds `ui.c` with gcc, writes 800×480 PNGs. |
| `firmware/` | Board bring-up, Wi-Fi, HTTP, NVS. |
| `assets/` | Rasterises the Claude mark into an LVGL alpha map. |
| `design/` | Design-canvas artboards tracing the firmware layout. |

The panel holds no credentials but your Wi-Fi password, which you type on it.

---

## The interesting part: the UI is hardware-independent

`ui/ui.c` has no Arduino, ESP-IDF or board headers. Data goes in through
`ui_set_usage()` / `ui_set_ha()` / `ui_set_status()`; actions come back through a
callback struct. The firmware supplies one implementation, `sim/` supplies another.

So the same file that runs on the panel also builds on a PC and renders to PNG:

```bash
cd sim && make -j && make shots      # 12 scenarios into sim/shots/
./sim --page home --theme light --out shot.png
```

A layout change is a two-second loop instead of a 35-second flash, and every state —
light theme, limit reached, bridge offline, Wi-Fi setup — can be rendered without
putting the device into it. Several real bugs were caught this way: sliders whose
knobs were clipped by the parent, tracks that stayed LVGL-blue in both themes, and
two `snprintf` truncations.

Needs gcc. On Windows, WSL works: `sudo apt install build-essential`.

---

## Where the usage numbers come from

**With a token — real numbers.** Every response from `api.anthropic.com` carries the
account's rate-limit state in headers (`anthropic-ratelimit-unified-5h-utilization`
and friends). A one-token Haiku request costs ~nothing and returns exactly what
`/usage` shows. This is [Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)'s
approach. Mint one with `claude setup-token`, drop it in `bridge/token.txt`.

**Without one — an estimate.** The bridge sums weighted token usage from
`~/.claude/projects/**/*.jsonl` over the same windows the panel uses: a 5-hour
session starting at the first message after a gap, and weekly windows since a fixed
reset (default Wednesday 13:00 local). Anthropic doesn't publish the budgets, so
calibrate once against the real panel:

```bash
python bridge/claude_usage_bridge.py --calibrate "session=1,week=20,model=30"
```

Every gauge shows its own provenance — `api`, `api-scaled`, `calibrated`,
`rejection` or `fallback` — so an estimate never reads as a fact.

---

## Home Assistant

Copy `bridge/ha_config.example.json` to `ha_config.json`, add your URL and a
long-lived token, then:

```bash
python bridge/claude_usage_bridge.py --ha-discover   # lists climate.* and light.*
```

Put one climate entity in `thermostat` and up to three lights in `lights`.

**The panel never names an entity.** It sends a light *index*; the bridge resolves it
against that config and rejects anything else with a 403. A device sitting on a desk,
reachable by anyone on the LAN, cannot reach an entity you did not list — verified
against out-of-scope lights and a door lock. The HA token stays on the PC.

---

## Setup

**1. Bridge** — stdlib only, no `pip install`:

```bash
python bridge/claude_usage_bridge.py
```

Allow it through the firewall for **private networks**, or the panel can't reach it.
`--once` prints the payload, `--dump` shows windows and sources.

**2. Firmware** — copy `config.h.example` to `config.h` and set `BRIDGE_URL` to your
PC. Wi-Fi is configured on the device itself; the `WIFI_*` defines are fallbacks.

**3. Libraries — the versions matter:**

| Library | Version | Why pinned |
| --- | --- | --- |
| esp32 Arduino core | **3.3.11** | On 3.3.8 every Wi-Fi join fails `AUTH_EXPIRE` |
| ESP32_Display_Panel | 1.0.4 | v0.x has no preset for the boxed 4.3B; the registry copy is a stale fork with no Waveshare support |
| ESP32_IO_Expander | 1.1.1 | |
| esp-lib-utils | 0.2.3 | The registry's 0.3.0 violates Display_Panel's own range |
| lvgl | 8.4.0 | LVGL 9 is an API break |
| ArduinoJson | 7.x | |

Four config headers live in the Arduino `libraries/` root, not the sketch:
`lv_conf.h`, `esp_panel_board_supported_conf.h`, `esp_panel_drivers_conf.h`,
`esp_utils_conf.h`. Editing any of them needs a clean build.

**4. Build:**

```powershell
.\build.ps1              # compile + flash
.\build.ps1 -Clean       # after touching a header in libraries/
```

All cores, fixed build path: a warm rebuild is ~35 s. Kill any serial monitor first —
it holds the port. Board: ESP32S3 Dev Module, 16MB flash, **OPI PSRAM**, USB CDC on
boot. Download mode: hold BOOT, tap RESET, release BOOT.

---

## Hardware notes that cost real time

- **The esp32 core version is not optional.** On 3.3.8 every association fails with
  `AUTH_EXPIRE` (reason 2) — at full signal, correct password, on any router. 3.3.11
  joins first try. Days can go into blaming the router for this.
- **The CH422G I/O expander ACKs every I²C address `0x20`–`0x3F`.** Any touch library
  that identifies chips by probing will bind to the expander instead of the GT911 and
  read nonsense out of it. Use the board preset, or address the GT911 at `0x5D`
  directly. A device answering 32 consecutive addresses is the tell.
- **RGB timings are stack-specific and don't transfer.** 12 MHz pixel clock with a
  `width*20` bounce buffer stops the panel drifting under ESP32_Display_Panel — and
  produces a blank white screen under Arduino_GFX, which wants 16 MHz and `width*10`.
- **PSRAM must be OPI.** The 800×480 framebuffer is 768 KB; without it you get
  `no mem for frame buffer`, which reads like a wiring fault.
- **Sliders:** LVGL draws the knob *outside* the track on all four sides. A
  `LV_SIZE_CONTENT` parent clips it — vertically always, horizontally at 0% and 100%.

`firmware/wifi_test/` is a bare radio test (open AP + scan, no display) for splitting
radio problems from everything else. `firmware/claude_usage_widget/` is an earlier
Arduino_GFX build kept as a fallback.

---

## Credentials

`ha_config.json`, `token.txt` and `config.h` are gitignored, with `.example` files
alongside. Nothing in this repo is a secret; check before you commit.

## Licence

MIT.
