/*
 * main.c - headless LVGL 8.4 host harness for the Claude usage widget.
 *
 * Renders ui/ui.c into an 800x480 in-memory framebuffer and writes a PNG.
 * No display, no window system, no real clock: the LVGL tick is a plain
 * uint32_t we step by hand, so a run is deterministic and reproducible.
 *
 * Everything the firmware would supply (Wi-Fi, the bridge, Home Assistant,
 * NVS preferences) is faked here. Callbacks just printf what the UI asked for,
 * which doubles as a smoke test that the UI wires its events up at all.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <lvgl.h>

#include "../ui/ui.h"
#include "png_write.h"

/* ------------------------------------------------------------- constants --- */

#define SIM_HOR_RES 800
#define SIM_VER_RES 480

/* Milliseconds of simulated time per settle iteration. Matches
 * LV_DISP_DEF_REFR_PERIOD (30) so every pass does refresh work. */
#define SIM_TICK_STEP_MS 30

/* Give up on settling after this many iterations even if something animates
 * forever (a spinner, a scrolling label). ~2.4s of simulated time. */
#define SIM_MAX_ITERS 80

/* Consecutive iterations with no flush before we call the frame settled. */
#define SIM_QUIET_ITERS 6

/* ------------------------------------------------------------------ state --- */

static lv_color_t *g_framebuf;          /* SIM_HOR_RES * SIM_VER_RES pixels    */
static lv_color_t *g_drawbuf;           /* LVGL draw buffer, full screen       */
static uint32_t    g_flush_count;       /* flushes since last reset            */
static uint32_t    g_now_ms;            /* the simulated monotonic clock       */

/* ------------------------------------------------------------ display drv --- */

static void sim_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    int32_t aw = area->x2 - area->x1 + 1;

    for (int32_t y = area->y1; y <= area->y2; y++) {
        if (y < 0 || y >= SIM_VER_RES) continue;
        for (int32_t x = area->x1; x <= area->x2; x++) {
            if (x < 0 || x >= SIM_HOR_RES) continue;
            g_framebuf[(size_t)y * SIM_HOR_RES + x] =
                px[(size_t)(y - area->y1) * aw + (x - area->x1)];
        }
    }

    g_flush_count++;
    lv_disp_flush_ready(drv);
}

static lv_disp_t *sim_display_init(void)
{
    /* Both buffers are plain malloc(): they must NOT come out of the LVGL
     * heap (LV_MEM_SIZE is 128 KB in lv_conf.h, sized for the ESP32). */
    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t      disp_drv;

    size_t px_count = (size_t)SIM_HOR_RES * SIM_VER_RES;

    g_framebuf = (lv_color_t *)malloc(px_count * sizeof(lv_color_t));
    g_drawbuf  = (lv_color_t *)malloc(px_count * sizeof(lv_color_t));
    if (!g_framebuf || !g_drawbuf) {
        fprintf(stderr, "sim: out of host memory for framebuffers\n");
        exit(1);
    }
    memset(g_framebuf, 0, px_count * sizeof(lv_color_t));

    lv_disp_draw_buf_init(&draw_buf, g_drawbuf, NULL, px_count);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = SIM_HOR_RES;
    disp_drv.ver_res  = SIM_VER_RES;
    disp_drv.flush_cb = sim_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    /* Full-screen buffer: ask LVGL for whole frames so a screenshot is never a
     * patchwork of stale regions. */
    disp_drv.full_refresh = 1;

    return lv_disp_drv_register(&disp_drv);
}

/* --------------------------------------------------------------- callbacks -- */

static void cb_page_changed(ui_page_t page)      { printf("[cb] page_changed(%d)\n", (int)page); }
static void cb_thermo_delta(int degrees)         { printf("[cb] thermo_delta(%+d)\n", degrees); }
static void cb_light_set(int i, int on, int bri) { printf("[cb] light_set(index=%d, on=%d, brightness=%d)\n", i, on, bri); }
static void cb_theme_changed(bool dark)          { printf("[cb] theme_changed(dark=%s)\n", dark ? "true" : "false"); }
static void cb_dim_changed(int percent)          { printf("[cb] dim_changed(%d%%)\n", percent); }
static void cb_sleep_changed(int index)          { printf("[cb] sleep_changed(index=%d)\n", index); }
static void cb_wifi_setup_requested(void)        { printf("[cb] wifi_setup_requested()\n"); }
static void cb_wifi_rescan(void)                 { printf("[cb] wifi_rescan()\n"); }
static void cb_refresh_requested(void)           { printf("[cb] refresh_requested()\n"); }

static void cb_wifi_connect(const char *ssid, const char *password)
{
    /* Deliberately does not echo the password. */
    printf("[cb] wifi_connect(ssid=\"%s\", password=<%d chars>)\n",
           ssid ? ssid : "", password ? (int)strlen(password) : 0);
}

static const ui_callbacks_t g_callbacks = {
    .page_changed         = cb_page_changed,
    .thermo_delta         = cb_thermo_delta,
    .light_set            = cb_light_set,
    .theme_changed        = cb_theme_changed,
    .dim_changed          = cb_dim_changed,
    .sleep_changed        = cb_sleep_changed,
    .wifi_setup_requested = cb_wifi_setup_requested,
    .wifi_connect         = cb_wifi_connect,
    .wifi_rescan          = cb_wifi_rescan,
    .refresh_requested    = cb_refresh_requested,
};

/* -------------------------------------------------------------- fake data --- */

typedef enum {
    ST_OK = 0,
    ST_STALE,
    ST_NOWIFI,
    ST_BRIDGE,
    ST_LIMIT
} sim_state_t;

static void gauge_set(ui_gauge_t *g, float pct, long used, long limit,
                      long resets_in, const char *source)
{
    memset(g, 0, sizeof *g);
    g->pct       = pct;
    g->used      = used;
    g->limit     = limit;
    g->resets_in = resets_in;
    snprintf(g->source, sizeof g->source, "%s", source);
}

static void fill_usage(ui_usage_t *u, sim_state_t state)
{
    memset(u, 0, sizeof *u);
    u->valid = true;
    snprintf(u->model_label, sizeof u->model_label, "Fable");

    if (state == ST_LIMIT) {
        u->blocked           = true;
        u->blocked_resets_in = 2700;                       /* 45 min */
        gauge_set(&u->session, 100.0f, 300, 300,      0, "api");
        gauge_set(&u->week,     87.5f, 175, 200, 191400, "api-scaled");
        gauge_set(&u->model,    96.0f, 192, 200, 191400, "calibrated");
        return;
    }

    gauge_set(&u->session, 62.4f, 187, 300,   4920, "api");
    gauge_set(&u->week,    41.0f,  82, 200, 259200, "api-scaled");
    gauge_set(&u->model,   78.5f, 157, 200, 259200, "calibrated");
}

static void fill_ha(ui_ha_t *h, sim_state_t state)
{
    memset(h, 0, sizeof *h);
    h->configured = true;

    if (state == ST_BRIDGE || state == ST_NOWIFI) {
        h->valid = false;
        snprintf(h->message, sizeof h->message, "%s",
                 state == ST_NOWIFI ? "No Wi-Fi - Home Assistant unreachable"
                                    : "Bridge offline - last sync 4 min ago");
        return;
    }

    h->valid          = true;
    h->has_thermostat = true;
    h->current    = 21.5f;
    h->target     = 22.0f;
    h->has_target = true;
    snprintf(h->thermo_name, sizeof h->thermo_name, "Living Room");
    snprintf(h->mode,        sizeof h->mode,        "heat");
    snprintf(h->action,      sizeof h->action,      "heating");
    snprintf(h->unit,        sizeof h->unit,        "C");

    h->light_count = 3;
    snprintf(h->lights[0].name, sizeof h->lights[0].name, "Desk Lamp");
    h->lights[0].on = true;  h->lights[0].brightness = 204;
    snprintf(h->lights[1].name, sizeof h->lights[1].name, "Kitchen");
    h->lights[1].on = false; h->lights[1].brightness = 0;
    snprintf(h->lights[2].name, sizeof h->lights[2].name, "Hallway");
    h->lights[2].on = true;  h->lights[2].brightness = 96;
}

static void apply_status(sim_state_t state)
{
    switch (state) {
        case ST_STALE:  ui_set_status(UI_STATUS_STALE, 930);          break;
        case ST_NOWIFI: ui_set_status(UI_STATUS_NO_WIFI, 3600);       break;
        case ST_BRIDGE: ui_set_status(UI_STATUS_BRIDGE_OFFLINE, 244); break;
        case ST_LIMIT:  ui_set_status(UI_STATUS_LIMIT_REACHED, 8);    break;
        case ST_OK:
        default:        ui_set_status(UI_STATUS_CONNECTED, 12);       break;
    }
}

static void feed_wifi_networks(void)
{
    static const ui_network_t nets[] = {
        { "Tanagra",       -47 },
        { "Tanagra-5G",    -58 },
        { "NETGEAR71",     -66 },
        { "xfinitywifi",   -74 },
        { "Sunroom Guest", -81 },
        { "SETUP-4A2C",    -88 },
    };
    ui_wifi_set_networks(nets, (int)(sizeof nets / sizeof nets[0]));
    ui_wifi_set_message("Select a network", false);
}

/* ---------------------------------------------------------------- settling -- */

/*
 * Step the simulated clock and pump LVGL until nothing redraws any more.
 * Deliberately never touches wall-clock time, so two runs of the same scenario
 * produce byte-identical PNGs.
 */
static void settle(void)
{
    int quiet = 0;

    for (int i = 0; i < SIM_MAX_ITERS; i++) {
        g_flush_count = 0;

        lv_tick_inc(SIM_TICK_STEP_MS);
        g_now_ms += SIM_TICK_STEP_MS;

        ui_tick(g_now_ms);
        lv_timer_handler();

        if (g_flush_count == 0) {
            if (++quiet >= SIM_QUIET_ITERS) return;
        }
        else {
            quiet = 0;
        }
    }
    fprintf(stderr, "sim: warning - never went quiet after %d iterations "
                    "(an animation is probably still running)\n", SIM_MAX_ITERS);
}

/* ------------------------------------------------------------------ output -- */

static int write_screenshot(const char *path)
{
    size_t px_count = (size_t)SIM_HOR_RES * SIM_VER_RES;
    uint8_t *rgb = (uint8_t *)malloc(px_count * 3u);
    int rc;

    if (!rgb) return -1;

    /* lv_color_t here is RGB565 (LV_COLOR_DEPTH 16, LV_COLOR_16_SWAP 0).
     * lv_color_to32() does the 5/6/5 -> 8/8/8 expansion with proper rounding,
     * so we do not hand-roll the bit replication. */
    for (size_t i = 0; i < px_count; i++) {
        lv_color32_t c32;
        c32.full = lv_color_to32(g_framebuf[i]);
        rgb[i * 3 + 0] = c32.ch.red;
        rgb[i * 3 + 1] = c32.ch.green;
        rgb[i * 3 + 2] = c32.ch.blue;
    }

    rc = png_write_rgb(path, rgb, SIM_HOR_RES, SIM_VER_RES);
    free(rgb);
    return rc;
}

/* mkdir -p on the directory part of `path`. Best effort; EEXIST is normal. */
static void ensure_parent_dirs(const char *path)
{
    char buf[1024];
    size_t n = strlen(path);

    if (n >= sizeof buf) return;
    memcpy(buf, path, n + 1);

    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            mkdir(buf, 0775);
            buf[i] = '/';
        }
    }
}

/* -------------------------------------------------------------------- cli --- */

static void usage_text(const char *argv0)
{
    printf(
        "usage: %s [options]\n"
        "\n"
        "  --page {usage|home|settings}   which page to show      (default: usage)\n"
        "  --theme {dark|light}           colour scheme           (default: dark)\n"
        "  --state {ok|stale|nowifi|bridge|limit}\n"
        "                                 connection / data state (default: ok)\n"
        "  --wifi                         show the Wi-Fi setup screen instead\n"
        "  --dim N                        software dim overlay, 0..70 (default: 0)\n"
        "  --out PATH                     PNG to write     (default: shots/out.png)\n"
        "  --help                         this text\n",
        argv0);
}

static int parse_page(const char *s, ui_page_t *out)
{
    if (!strcmp(s, "usage"))    { *out = UI_PAGE_USAGE;    return 0; }
    if (!strcmp(s, "home"))     { *out = UI_PAGE_HOME;     return 0; }
    if (!strcmp(s, "settings")) { *out = UI_PAGE_SETTINGS; return 0; }
    return -1;
}

static int parse_state(const char *s, sim_state_t *out)
{
    if (!strcmp(s, "ok"))     { *out = ST_OK;     return 0; }
    if (!strcmp(s, "stale"))  { *out = ST_STALE;  return 0; }
    if (!strcmp(s, "nowifi")) { *out = ST_NOWIFI; return 0; }
    if (!strcmp(s, "bridge")) { *out = ST_BRIDGE; return 0; }
    if (!strcmp(s, "limit"))  { *out = ST_LIMIT;  return 0; }
    return -1;
}

int main(int argc, char **argv)
{
    ui_page_t   page    = UI_PAGE_USAGE;
    bool        dark    = true;
    sim_state_t state   = ST_OK;
    bool        do_wifi = false;
    int         dim     = 0;
    const char *out     = "shots/out.png";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int takes_value = strcmp(a, "--wifi") && strcmp(a, "--help") && strcmp(a, "-h");

        if (takes_value && i + 1 >= argc) {
            fprintf(stderr, "sim: %s needs a value\n", a);
            return 2;
        }

        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage_text(argv[0]);
            return 0;
        }
        else if (!strcmp(a, "--wifi")) {
            do_wifi = true;
        }
        else if (!strcmp(a, "--page")) {
            if (parse_page(argv[++i], &page) != 0) {
                fprintf(stderr, "sim: unknown page '%s'\n", argv[i]);
                return 2;
            }
        }
        else if (!strcmp(a, "--theme")) {
            const char *v = argv[++i];
            if (!strcmp(v, "dark"))       dark = true;
            else if (!strcmp(v, "light")) dark = false;
            else { fprintf(stderr, "sim: unknown theme '%s'\n", v); return 2; }
        }
        else if (!strcmp(a, "--state")) {
            if (parse_state(argv[++i], &state) != 0) {
                fprintf(stderr, "sim: unknown state '%s'\n", argv[i]);
                return 2;
            }
        }
        else if (!strcmp(a, "--dim")) {
            dim = atoi(argv[++i]);
            if (dim < 0)  dim = 0;
            if (dim > 70) dim = 70;
        }
        else if (!strcmp(a, "--out")) {
            out = argv[++i];
        }
        else {
            fprintf(stderr, "sim: unknown option '%s' (try --help)\n", a);
            return 2;
        }
    }

    printf("sim: page=%d theme=%s state=%d wifi=%d dim=%d -> %s\n",
           (int)page, dark ? "dark" : "light", (int)state, (int)do_wifi, dim, out);

    lv_init();
    sim_display_init();

    /* Preferences the firmware would restore from NVS, applied before ui_init()
     * so the very first render is already in the right theme. */
    ui_set_theme(dark);
    ui_set_dim(dim);
    ui_set_sleep_index(2);

    ui_init(&g_callbacks);

    /* Re-assert after init: ui.h documents these as safe either way, and doing
     * both is how the firmware behaves on a cold boot. */
    ui_set_theme(dark);
    ui_set_dim(dim);

    {
        ui_usage_t u;
        ui_ha_t    h;
        fill_usage(&u, state);
        fill_ha(&h, state);
        ui_set_usage(&u);
        ui_set_ha(&h);
    }

    ui_set_network_info(state == ST_NOWIFI ? "" : "Tanagra",
                        state == ST_NOWIFI ? "0.0.0.0" : "192.168.1.47",
                        "http://192.168.1.10:8787",
                        "1.4.2");
    apply_status(state);

    if (do_wifi) {
        ui_show_wifi_setup();
        feed_wifi_networks();
    }
    else {
        ui_show_page(page);
    }

    settle();

    if (do_wifi && !ui_wifi_is_active())
        fprintf(stderr, "sim: warning - asked for the wifi screen but "
                        "ui_wifi_is_active() is false\n");
    if (!do_wifi && ui_get_page() != page)
        fprintf(stderr, "sim: warning - asked for page %d but ui_get_page() is %d\n",
                (int)page, (int)ui_get_page());

    ensure_parent_dirs(out);
    if (write_screenshot(out) != 0) {
        fprintf(stderr, "sim: failed to write %s\n", out);
        return 1;
    }

    printf("sim: wrote %s (%dx%d)\n", out, SIM_HOR_RES, SIM_VER_RES);
    return 0;
}
