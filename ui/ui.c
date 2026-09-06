/*
 * ui.c - hardware-independent UI for the Claude usage widget. See ui.h.
 *
 * Layout is LVGL 8 flex throughout. There are exactly three places that are not
 * flex, each for a structural reason, and each is marked ABSOLUTE: below:
 *   - the percentage label centred ON the arc (flex cannot stack on z)
 *   - the burst mark's line points (lv_line takes pixel coordinates by definition)
 *   - the dim sheet on lv_layer_top() (a full-screen overlay is not in any flow)
 * Everything else is flow, percentage or LV_SIZE_CONTENT, so the same file lays
 * out correctly at 800x480 on the panel and in the host renderer.
 *
 * Redraw discipline: on an RGB panel every invalidation is a chance to tear, so
 * nothing here writes to a widget unless the rendered value actually changed - see
 * set_label() / set_bg(). Dynamic labels are given fixed widths so that a text
 * change never reflows a parent and cascades into sibling repaints.
 */

#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --------------------------------------------------------------- palette --- */

typedef struct {
    lv_color_t bg, surface, line, text, muted;
} palette_t;

/* lv_color_hex() is not a constant expression, so the palettes are filled in at
 * init rather than declared static const. */
static palette_t pal_dark, pal_light, P;

static void palettes_init(void)
{
    pal_dark.bg      = lv_color_hex(0x1F1E1D);
    pal_dark.surface = lv_color_hex(0x262624);
    pal_dark.line    = lv_color_hex(0x3A3A37);
    pal_dark.text    = lv_color_hex(0xF0EEE6);
    pal_dark.muted   = lv_color_hex(0x8A8780);

    pal_light.bg      = lv_color_hex(0xF0EEE6);
    pal_light.surface = lv_color_hex(0xFAF9F5);
    pal_light.line    = lv_color_hex(0xD9D6CC);
    pal_light.text    = lv_color_hex(0x191919);
    pal_light.muted   = lv_color_hex(0x6E6B62);
}

#define C_ACCENT lv_color_hex(0xD97757)
#define C_WARN   lv_color_hex(0xC2703F)
#define C_HOT    lv_color_hex(0xA63D2F)
#define C_GOOD   lv_color_hex(0x7D9A6B)

const uint32_t ui_sleep_ms[5] = {0, 5 * 60000UL, 15 * 60000UL, 30 * 60000UL, 60 * 60000UL};

/* ---------------------------------------------------------------- state ---- */

static ui_callbacks_t CB;

static bool s_dark = true;
static int  s_dim = 0;
static int  s_sleep_idx = 0;
static ui_page_t s_page = UI_PAGE_USAGE;

static ui_usage_t s_usage;
static ui_ha_t    s_ha;
static uint32_t   s_now_ms = 0;
static uint32_t   s_usage_at = 0;   /* s_now_ms when ui_set_usage() last ran */
static bool       s_built = false;

static char s_info_ssid[33], s_info_ip[24], s_info_bridge[96], s_info_fw[12];

/* styles */
static lv_style_t st_screen, st_surface, st_card, st_line, st_text, st_muted,
                  st_btn, st_btn_accent;

/* widgets */
static lv_obj_t *scr_root, *scr_wifi;
static lv_obj_t *page_area, *pages[UI_PAGE_COUNT];
static lv_obj_t *page_dot[UI_PAGE_COUNT];
static lv_obj_t *lbl_title, *logo_obj;
static lv_obj_t *lbl_status, *dot_status;

static lv_obj_t *arc_session, *lbl_pct, *lbl_caption;
static lv_obj_t *bar_week, *lbl_week_pct, *lbl_week_sub;
static lv_obj_t *bar_model, *lbl_model_pct, *lbl_model_sub, *lbl_model_title;

static lv_obj_t *lbl_thermo_name, *lbl_thermo_cur, *lbl_thermo_target, *lbl_thermo_mode,
                *lbl_ha_msg, *thermo_body, *lights_body;
static lv_obj_t *light_row[UI_MAX_LIGHTS], *light_name[UI_MAX_LIGHTS],
                *light_sw[UI_MAX_LIGHTS], *light_slider[UI_MAX_LIGHTS];

static lv_obj_t *sw_dark, *slider_dim, *dd_sleep, *lbl_info;
static lv_obj_t *dimmer;

static lv_obj_t *wifi_list, *ta_pass, *lbl_wifi_msg;
static char s_wifi_ssid[33];

/* ------------------------------------------------------------- utilities --- */

static void copy_str(char *dst, size_t n, const char *src)
{
    if (n == 0) return;
    snprintf(dst, n, "%s", src ? src : "");
}

/* Only write when the rendered text differs: every set invalidates and flushes. */
static void set_label(lv_obj_t *lbl, const char *text)
{
    if (lbl == NULL) return;
    const char *cur = lv_label_get_text(lbl);
    if (cur == NULL || strcmp(cur, text) != 0) lv_label_set_text(lbl, text);
}

static void set_bg(lv_obj_t *obj, lv_color_t c, lv_style_selector_t sel)
{
    if (obj == NULL) return;
    if (lv_obj_get_style_bg_color(obj, sel).full != c.full)
        lv_obj_set_style_bg_color(obj, c, sel);
}

static lv_color_t severity(float pct)
{
    if (pct >= 90.0f) return C_HOT;
    if (pct >= 70.0f) return C_WARN;
    return C_ACCENT;
}

/* Temperature as a bounded integer. Printing a float with %.0f can emit up to 310
 * characters for an absurd value, which is a real (if unlikely) truncation. */
static int temp_i(float t)
{
    if (t > 999.0f) t = 999.0f;
    if (t < -999.0f) t = -999.0f;
    return (int)lroundf(t);
}

/* Integer arithmetic rather than %.1f of a double. Two reasons: it keeps softfloat
 * out of a path called several times a second on an MCU, and the compiler can bound
 * the formatted width - given a double it must assume %f could emit 300+ characters,
 * which it reports as a possible truncation no matter how the value is clamped. */
static void fmt_tokens(long v, char *out, size_t n)
{
    if (v < 0) v = 0;
    if (v > 999999999999L) v = 999999999999L; /* display ceiling, 999.9B */

    if (v >= 1000000000L)
        snprintf(out, n, "%ld.%ldB", v / 1000000000L, (v / 100000000L) % 10L);
    else if (v >= 1000000L)
        snprintf(out, n, "%ld.%ldM", v / 1000000L, (v / 100000L) % 10L);
    else if (v >= 1000L)
        snprintf(out, n, "%ldK", v / 1000L);
    else
        snprintf(out, n, "%ld", v);
}

static void fmt_duration(long seconds, char *out, size_t n)
{
    if (seconds <= 0) { snprintf(out, n, "now"); return; }
    long d = seconds / 86400, h = (seconds % 86400) / 3600, m = (seconds % 3600) / 60;
    if (d > 0)      snprintf(out, n, "%ldd %ldh", d, h);
    else if (h > 0) snprintf(out, n, "%ldh %ldm", h, m);
    else if (m > 0) snprintf(out, n, "%ldm", m);
    else            snprintf(out, n, "<1m");
}

/* Seconds left on a countdown captured at the last ui_set_usage(). */
static long live_resets_in(long at_fetch)
{
    if (at_fetch <= 0) return 0;
    long elapsed = (long)((s_now_ms - s_usage_at) / 1000U);
    long left = at_fetch - elapsed;
    return left > 0 ? left : 0;
}

/* --------------------------------------------------------------- styles ---- */

static void styles_write(void)
{
    lv_style_set_bg_color(&st_screen, P.bg);
    lv_style_set_bg_opa(&st_screen, LV_OPA_COVER);

    lv_style_set_bg_color(&st_surface, P.surface);
    lv_style_set_bg_opa(&st_surface, LV_OPA_COVER);
    lv_style_set_border_width(&st_surface, 0);
    lv_style_set_radius(&st_surface, 0);

    lv_style_set_bg_color(&st_card, P.surface);
    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    lv_style_set_border_color(&st_card, P.line);
    lv_style_set_border_width(&st_card, 1);
    lv_style_set_radius(&st_card, 12);

    lv_style_set_bg_color(&st_line, P.line);
    lv_style_set_bg_opa(&st_line, LV_OPA_COVER);

    lv_style_set_text_color(&st_text, P.text);
    lv_style_set_text_color(&st_muted, P.muted);

    lv_style_set_bg_color(&st_btn, P.surface);
    lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
    lv_style_set_border_color(&st_btn, P.line);
    lv_style_set_border_width(&st_btn, 1);
    lv_style_set_radius(&st_btn, 8);
    lv_style_set_text_color(&st_btn, P.text);
    lv_style_set_shadow_width(&st_btn, 0);

    lv_style_set_bg_color(&st_btn_accent, C_ACCENT);
    lv_style_set_bg_opa(&st_btn_accent, LV_OPA_COVER);
    lv_style_set_border_width(&st_btn_accent, 0);
    lv_style_set_radius(&st_btn_accent, 8);
    lv_style_set_text_color(&st_btn_accent, lv_color_hex(0x1F1E1D));
    lv_style_set_shadow_width(&st_btn_accent, 0);
}

static void styles_init(void)
{
    lv_style_init(&st_screen);   lv_style_init(&st_surface);
    lv_style_init(&st_card);     lv_style_init(&st_line);
    lv_style_init(&st_text);     lv_style_init(&st_muted);
    lv_style_init(&st_btn);      lv_style_init(&st_btn_accent);
    styles_write();
}

/* Colours held as local styles rather than shared ones have to be re-poked. */
static void theme_refresh_locals(void)
{
    if (!s_built) return;
    lv_obj_set_style_arc_color(arc_session, P.line, LV_PART_MAIN);
    for (int i = 0; i < UI_PAGE_COUNT; i++)
        set_bg(page_dot[i], i == (int)s_page ? C_ACCENT : P.line, 0);
}

/* ------------------------------------------------------- widget factories --- */

/* Track height and the knob's overhang past it. The default theme sizes the knob
 * from its own padding and draws it outside the track, which a LV_SIZE_CONTENT
 * parent then clips top and bottom - that was the cut-off slider. Pinning both
 * makes the overhang a known number the layout can leave room for. */
#define UI_SLIDER_TRACK 16
#define UI_SLIDER_KNOB_PAD 6
/* Knob = track + 2*pad, drawn centred on the track, so it reaches this far past
 * every edge: vertically always, horizontally at the two ends of travel. */
#define UI_SLIDER_KNOB (UI_SLIDER_TRACK + 2 * UI_SLIDER_KNOB_PAD)
#define UI_SLIDER_OVERHANG UI_SLIDER_KNOB_PAD
#define UI_SLIDER_REACH (UI_SLIDER_KNOB / 2)


/* A bare flow container: no default padding, border or scrolling. */
static lv_obj_t *mk_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    /* Transparent to input. lv_obj_create() sets CLICKABLE by default and the hit
     * test stops at the first clickable object, so without this a swipe starting on
     * a card is swallowed there and never reaches the screen's gesture handler.
     * Real controls (buttons, switches, sliders) are created elsewhere and keep it. */
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *mk_flex(lv_obj_t *parent, lv_flex_flow_t flow, lv_coord_t pad, lv_coord_t gap)
{
    lv_obj_t *o = mk_box(parent);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_style_pad_all(o, pad, 0);
    lv_obj_set_style_pad_row(o, gap, 0);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, lv_style_t *color,
                          const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_add_style(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *mk_card(lv_obj_t *parent)
{
    lv_obj_t *c = mk_flex(parent, LV_FLEX_FLOW_COLUMN, 18, 6);
    lv_obj_add_style(c, &st_card, 0);
    return c;
}

static lv_obj_t *mk_button(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, const char *text,
                           bool accent, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, accent ? &st_btn_accent : &st_btn, 0);
    lv_obj_set_size(b, w, h);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

/* A settings/list row: label on the left, control on the right. */
static lv_obj_t *mk_row(lv_obj_t *parent, const char *text, lv_obj_t **out_label)
{
    lv_obj_t *row = mk_flex(parent, LV_FLEX_FLOW_ROW, 0, 8);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = mk_label(row, &lv_font_montserrat_14, &st_text, text);
    lv_obj_set_flex_grow(l, 1);
    if (out_label) *out_label = l;
    return row;
}

static lv_obj_t *mk_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_add_style(bar, &st_line, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 7, LV_PART_INDICATOR);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, 14);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    return bar;
}

static void style_switch(lv_obj_t *sw)
{
    lv_obj_set_size(sw, 60, 32);
    lv_obj_set_style_bg_color(sw, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(sw, 10);
}

static void style_slider(lv_obj_t *s)
{
    /* The track is a shared style, not a local colour, so it follows the theme.
     * Without this the slider keeps LVGL's default blue in both themes. */
    lv_obj_add_style(s, &st_line, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, C_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, UI_SLIDER_KNOB_PAD, LV_PART_KNOB);
    lv_obj_set_height(s, UI_SLIDER_TRACK);
}

/* Every slider goes in a wrapper that reserves the knob's reach. Without the
 * horizontal half the knob is sliced in two at 0% and 100%; without the vertical
 * it is sliced top and bottom. The card clips whatever leaves its bounds. */
static lv_obj_t *mk_slider(lv_obj_t *parent, lv_coord_t width)
{
    lv_obj_t *wrap = mk_flex(parent, LV_FLEX_FLOW_COLUMN, 0, 0);
    lv_obj_set_width(wrap, width);
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(wrap, UI_SLIDER_REACH, 0);
    lv_obj_set_style_pad_ver(wrap, UI_SLIDER_OVERHANG, 0);

    lv_obj_t *s = lv_slider_create(wrap);
    lv_obj_set_width(s, LV_PCT(100));
    style_slider(s);
    /* 16px of track is a small thing to catch with a finger on a 4.3in panel. */
    lv_obj_set_ext_click_area(s, 12);
    return s;
}

/* ----------------------------------------------------------- burst mark ---- */

/* Two implementations. The real Claude mark is an SVG path with irregular ray
 * lengths and an organic taper - lv_line cannot approximate it at all (it draws
 * constant-width segments; it does anti-alias and round its caps, but width is
 * fixed). assets/make_logo.py scan-converts the path to an LV_IMG_CF_ALPHA_8BIT
 * coverage map: 1024 bytes, blitted straight from flash with no decode buffer, and
 * tinted at draw time via img_recolor, so it follows the theme rather than having a
 * colour baked in. Define UI_BURST_LINES to fall back to twelve plain spokes. */
#if !defined(UI_BURST_LINES) && defined(UI_HAVE_LOGO_IMG)

#include "claude_logo.h"

static lv_obj_t *mk_burst(lv_obj_t *parent, int size)
{
    (void)size; /* the asset is generated at its final size; see assets/README.md */
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, &claude_logo);
    lv_obj_set_style_img_recolor(img, C_ACCENT, 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

#else

#define BURST_SPOKES 12
static lv_point_t burst_pts[BURST_SPOKES][2];

/* ABSOLUTE: lv_line takes pixel coordinates by definition. Confined to a fixed-size
 * wrapper that itself participates in the header's flex row. */
static lv_obj_t *mk_burst(lv_obj_t *parent, int size)
{
    lv_obj_t *wrap = mk_box(parent);
    lv_obj_set_size(wrap, size, size);
    const int cx = size / 2, cy = size / 2, r = size / 2, inner = r / 3;
    for (int i = 0; i < BURST_SPOKES; i++) {
        double rad = (i * (360.0 / BURST_SPOKES)) * M_PI / 180.0;
        double c = cos(rad), s = sin(rad);
        burst_pts[i][0].x = (lv_coord_t)lround(cx + c * inner);
        burst_pts[i][0].y = (lv_coord_t)lround(cy + s * inner);
        burst_pts[i][1].x = (lv_coord_t)lround(cx + c * r);
        burst_pts[i][1].y = (lv_coord_t)lround(cy + s * r);
        lv_obj_t *line = lv_line_create(wrap);
        lv_line_set_points(line, burst_pts[i], 2);
        lv_obj_set_style_line_width(line, 3, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_style_line_color(line, C_ACCENT, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
    return wrap;
}

#endif /* burst implementation */

/* ------------------------------------------------------------- callbacks --- */

static void on_nav(lv_event_t *e)
{
    ui_page_t p = (ui_page_t)(intptr_t)lv_event_get_user_data(e);
    ui_show_page(p);
    if (CB.page_changed) CB.page_changed(p);
}

static void on_thermo(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    /* Reflect the intent immediately; the confirmed value arrives with the next
     * ui_set_ha(). Without this the button feels dead for a whole poll interval. */
    if (s_ha.has_target) {
        char b[24];
        s_ha.target += (float)delta;
        snprintf(b, sizeof(b), "%d%s", temp_i(s_ha.target), s_ha.unit);
        set_label(lbl_thermo_target, b);
    }
    if (CB.thermo_delta) CB.thermo_delta(delta);
}

/* Optimistic control. The round trip is device -> bridge -> Home Assistant -> HA
 * applies it -> next poll reads it back, so for a second or two the poll still
 * carries the OLD value and would drag the switch back under the user's finger.
 * The requested value is shown at once and held until the server agrees, or until
 * the window expires and the server is believed instead. */
#define UI_OPTIMISTIC_MS 6000

typedef struct {
    bool active;
    uint32_t since;
    bool want_on;
    int want_brightness;   /* <0: only on/off was requested */
} pending_light_t;

static pending_light_t s_pending[UI_MAX_LIGHTS];

static void hold_light(int idx, bool on, int brightness)
{
    if (idx < 0 || idx >= UI_MAX_LIGHTS) return;
    s_pending[idx].active = true;
    s_pending[idx].since = s_now_ms;
    s_pending[idx].want_on = on;
    s_pending[idx].want_brightness = brightness;
    if (idx < s_ha.light_count) {
        s_ha.lights[idx].on = on;
        if (brightness >= 0) s_ha.lights[idx].brightness = brightness;
    }
}

static void on_light_switch(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    hold_light(idx, on, -1);
    if (CB.light_set) CB.light_set(idx, on ? 1 : 0, -1);
}

/* RELEASED only: one call per gesture rather than one per pixel of drag. */
static void on_light_slider(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int bri = (int)lv_slider_get_value(lv_event_get_target(e));
    hold_light(idx, bri > 0, bri);
    if (CB.light_set) CB.light_set(idx, bri > 0 ? 1 : 0, bri);
}

static void on_dark(lv_event_t *e)
{
    bool dark = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ui_set_theme(dark);
    if (CB.theme_changed) CB.theme_changed(dark);
}

/* RELEASED only: the dim sheet is full-screen, so applying it per VALUE_CHANGED
 * repaints 800x480 for every pixel of the drag. */
static void on_dim(lv_event_t *e)
{
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    ui_set_dim(v);
    if (CB.dim_changed) CB.dim_changed(v);
}

static void on_sleep(lv_event_t *e)
{
    int idx = (int)lv_dropdown_get_selected(lv_event_get_target(e));
    s_sleep_idx = idx;
    if (CB.sleep_changed) CB.sleep_changed(idx);
}

static void on_wifi_setup_btn(lv_event_t *e)
{
    (void)e;
    ui_show_wifi_setup();
    if (CB.wifi_setup_requested) CB.wifi_setup_requested();
}

static void on_refresh_btn(lv_event_t *e)
{
    (void)e;
    if (CB.refresh_requested) CB.refresh_requested();
}

/* ---------------------------------------------------------------- header --- */

static void build_header(lv_obj_t *parent)
{
    lv_obj_t *header = mk_flex(parent, LV_FLEX_FLOW_ROW, 0, 0);
    lv_obj_add_style(header, &st_surface, 0);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 66);
    lv_obj_set_style_pad_left(header, 24, 0);
    lv_obj_set_style_pad_right(header, 24, 0);
    lv_obj_set_style_pad_column(header, 12, 0);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    logo_obj = mk_burst(header, 30);

    /* One header serves all three pages, so the title is rewritten and the mark
     * shown or hidden per page - see ui_show_page(). */
    lbl_title = mk_label(header, &lv_font_montserrat_28, &st_text, "Claude Usage");
    lv_obj_set_flex_grow(lbl_title, 1);

    /* Fixed width so a status change never reflows the row. */
    lbl_status = mk_label(header, &lv_font_montserrat_14, &st_muted, "starting");
    lv_obj_set_width(lbl_status, 260);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_RIGHT, 0);

    dot_status = mk_box(header);
    lv_obj_set_size(dot_status, 12, 12);
    lv_obj_set_style_radius(dot_status, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot_status, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot_status, P.muted, 0);

    lv_obj_t *rule = mk_box(parent);
    lv_obj_add_style(rule, &st_line, 0);
    lv_obj_set_width(rule, LV_PCT(100));
    lv_obj_set_height(rule, 1);
}

/* ------------------------------------------------------------------- nav --- */

/* Swipe replaces the nav bar. The dots stay tappable on purpose: a flick that the
 * panel misses would otherwise leave no way at all to change page, and 26px of dots
 * is cheaper than the 43px bar they replace. */
static void on_gesture(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    int page = (int)s_page;
    if (dir == LV_DIR_LEFT && page < UI_PAGE_COUNT - 1) page++;
    else if (dir == LV_DIR_RIGHT && page > 0) page--;
    else return;

    ui_show_page((ui_page_t)page);
    if (CB.page_changed) CB.page_changed((ui_page_t)page);
}

static void build_dots(lv_obj_t *parent)
{
    lv_obj_t *bar = mk_flex(parent, LV_FLEX_FLOW_ROW, 0, 10);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, 26);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        /* The hit area is larger than the dot so it is tappable at 7px. */
        lv_obj_t *hit = lv_btn_create(bar);
        lv_obj_remove_style_all(hit);
        lv_obj_set_size(hit, 44, 26);
        lv_obj_add_event_cb(hit, on_nav, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *dot = mk_box(hit);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_center(dot);
        page_dot[i] = dot;
    }
}

/* ------------------------------------------------------------ usage page --- */

static lv_obj_t *mk_page(void)
{
    lv_obj_t *p = mk_flex(page_area, LV_FLEX_FLOW_ROW, 18, 24);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_align(p, LV_ALIGN_TOP_LEFT, 0, 0);
    return p;
}

static void build_usage_page(void)
{
    lv_obj_t *p = mk_page();
    pages[UI_PAGE_USAGE] = p;

    /* left: title, ring, caption */
    lv_obj_t *ring_col = mk_flex(p, LV_FLEX_FLOW_COLUMN, 0, 8);
    lv_obj_set_width(ring_col, LV_PCT(52));
    lv_obj_set_height(ring_col, LV_PCT(100));
    lv_obj_set_flex_align(ring_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *t = mk_label(ring_col, &lv_font_montserrat_14, &st_muted, "5-HOUR SESSION");
    lv_obj_set_style_text_letter_space(t, 1, 0);

    /* ABSOLUTE: the percentage is centred ON the ring; flex cannot stack on z, so
     * the pair live in a fixed-size wrapper with no layout of its own. */
    lv_obj_t *arc_wrap = mk_box(ring_col);
    lv_obj_set_size(arc_wrap, 240, 240);

    arc_session = lv_arc_create(arc_wrap);
    lv_obj_remove_style(arc_session, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc_session, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc_session, 240, 240);
    lv_obj_center(arc_session);
    lv_arc_set_rotation(arc_session, 135);
    lv_arc_set_bg_angles(arc_session, 0, 270);
    lv_arc_set_range(arc_session, 0, 100);
    lv_arc_set_value(arc_session, 0);
    lv_obj_set_style_arc_width(arc_session, 28, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_session, 28, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_session, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_session, P.line, LV_PART_MAIN);

    lbl_pct = mk_label(arc_wrap, &lv_font_montserrat_48, &st_text, "--");
    lv_obj_center(lbl_pct);

    lbl_caption = mk_label(ring_col, &lv_font_montserrat_14, &st_muted, "");
    lv_obj_set_width(lbl_caption, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_caption, LV_TEXT_ALIGN_CENTER, 0);

    /* right: two cards sharing the column */
    lv_obj_t *cards = mk_flex(p, LV_FLEX_FLOW_COLUMN, 0, 18);
    lv_obj_set_flex_grow(cards, 1);
    lv_obj_set_height(cards, LV_PCT(100));

    lv_obj_t *card_a = mk_card(cards);
    lv_obj_set_width(card_a, LV_PCT(100));
    lv_obj_set_flex_grow(card_a, 1);
    lv_obj_t *ta = mk_label(card_a, &lv_font_montserrat_14, &st_muted, "WEEKLY   ALL MODELS");
    lv_obj_set_style_text_letter_space(ta, 1, 0);
    lbl_week_pct = mk_label(card_a, &lv_font_montserrat_28, &st_text, "--");
    bar_week = mk_bar(card_a);
    lbl_week_sub = mk_label(card_a, &lv_font_montserrat_14, &st_muted, "");
    lv_obj_set_width(lbl_week_sub, LV_PCT(100));

    lv_obj_t *card_b = mk_card(cards);
    lv_obj_set_width(card_b, LV_PCT(100));
    lv_obj_set_flex_grow(card_b, 1);
    lbl_model_title = mk_label(card_b, &lv_font_montserrat_14, &st_muted, "WEEKLY   MODEL");
    lv_obj_set_style_text_letter_space(lbl_model_title, 1, 0);
    lbl_model_pct = mk_label(card_b, &lv_font_montserrat_28, &st_text, "--");
    bar_model = mk_bar(card_b);
    lbl_model_sub = mk_label(card_b, &lv_font_montserrat_14, &st_muted, "");
    lv_obj_set_width(lbl_model_sub, LV_PCT(100));
}

/* ------------------------------------------------------------- home page --- */

static void build_home_page(void)
{
    lv_obj_t *p = mk_page();
    pages[UI_PAGE_HOME] = p;

    /* thermostat */
    lv_obj_t *card = mk_card(p);
    lv_obj_set_width(card, LV_PCT(50));
    lv_obj_set_height(card, LV_PCT(100));

    lv_obj_t *head = mk_flex(card, LV_FLEX_FLOW_ROW, 0, 8);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_t *ht = mk_label(head, &lv_font_montserrat_14, &st_muted, "THERMOSTAT");
    lv_obj_set_style_text_letter_space(ht, 1, 0);
    lv_obj_set_flex_grow(ht, 1);
    lbl_thermo_name = mk_label(head, &lv_font_montserrat_14, &st_muted, "");

    thermo_body = mk_flex(card, LV_FLEX_FLOW_COLUMN, 0, 4);
    lv_obj_set_width(thermo_body, LV_PCT(100));
    lv_obj_set_flex_grow(thermo_body, 1);

    lbl_thermo_cur = mk_label(thermo_body, &lv_font_montserrat_48, &st_text, "--");
    mk_label(thermo_body, &lv_font_montserrat_14, &st_muted, "current");

    lv_obj_t *spacer = mk_box(thermo_body);
    lv_obj_set_size(spacer, 1, 8);

    mk_label(thermo_body, &lv_font_montserrat_14, &st_muted, "target");
    lv_obj_t *tgt = mk_flex(thermo_body, LV_FLEX_FLOW_ROW, 0, 12);
    lv_obj_set_width(tgt, LV_PCT(100));
    lv_obj_set_height(tgt, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(tgt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    mk_button(tgt, 64, 56, LV_SYMBOL_MINUS, false, on_thermo, (void *)(intptr_t)-1);
    lbl_thermo_target = mk_label(tgt, &lv_font_montserrat_48, &st_text, "--");
    lv_obj_set_flex_grow(lbl_thermo_target, 1);
    lv_obj_set_style_text_align(lbl_thermo_target, LV_TEXT_ALIGN_CENTER, 0);
    mk_button(tgt, 64, 56, LV_SYMBOL_PLUS, false, on_thermo, (void *)(intptr_t)1);

    lbl_thermo_mode = mk_label(card, &lv_font_montserrat_14, &st_muted, "");
    lv_obj_set_width(lbl_thermo_mode, LV_PCT(100));

    /* lights */
    lv_obj_t *lc = mk_card(p);
    lv_obj_set_flex_grow(lc, 1);
    lv_obj_set_height(lc, LV_PCT(100));
    lv_obj_t *lt = mk_label(lc, &lv_font_montserrat_14, &st_muted, "OFFICE LIGHTS");
    lv_obj_set_style_text_letter_space(lt, 1, 0);

    lights_body = mk_flex(lc, LV_FLEX_FLOW_COLUMN, 0, 24);
    lv_obj_set_width(lights_body, LV_PCT(100));
    lv_obj_set_flex_grow(lights_body, 1);

    for (int i = 0; i < UI_MAX_LIGHTS; i++) {
        lv_obj_t *row = mk_flex(lights_body, LV_FLEX_FLOW_COLUMN, 0, 10);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        light_row[i] = row;

        lv_obj_t *top = mk_flex(row, LV_FLEX_FLOW_ROW, 0, 8);
        lv_obj_set_width(top, LV_PCT(100));
        lv_obj_set_height(top, LV_SIZE_CONTENT);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        light_name[i] = mk_label(top, &lv_font_montserrat_14, &st_text, "");
        lv_obj_set_flex_grow(light_name[i], 1);

        light_sw[i] = lv_switch_create(top);
        style_switch(light_sw[i]);
        lv_obj_add_event_cb(light_sw[i], on_light_switch, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)i);

        light_slider[i] = mk_slider(row, LV_PCT(100));
        lv_slider_set_range(light_slider[i], 0, 255);
        lv_obj_add_event_cb(light_slider[i], on_light_slider, LV_EVENT_RELEASED,
                            (void *)(intptr_t)i);
    }

    lbl_ha_msg = mk_label(lights_body, &lv_font_montserrat_14, &st_muted, "");
    lv_obj_set_width(lbl_ha_msg, LV_PCT(100));
    lv_label_set_long_mode(lbl_ha_msg, LV_LABEL_LONG_WRAP);
}

/* --------------------------------------------------------- settings page --- */

static void build_settings_page(void)
{
    lv_obj_t *p = mk_page();
    pages[UI_PAGE_SETTINGS] = p;

    lv_obj_t *card = mk_card(p);
    lv_obj_set_width(card, LV_PCT(50));
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_t *dt = mk_label(card, &lv_font_montserrat_14, &st_muted, "DISPLAY");
    lv_obj_set_style_text_letter_space(dt, 1, 0);

    lv_obj_t *dark_row = mk_row(card, "Dark mode", NULL);
    sw_dark = lv_switch_create(dark_row);
    style_switch(sw_dark);
    if (s_dark) lv_obj_add_state(sw_dark, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_dark, on_dark, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *dim_row = mk_row(card, "Dim screen", NULL);
    slider_dim = mk_slider(dim_row, 180 + 2 * UI_SLIDER_REACH);
    lv_slider_set_range(slider_dim, 0, 70);
    lv_slider_set_value(slider_dim, s_dim, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider_dim, on_dim, LV_EVENT_RELEASED, NULL);

    lv_obj_t *sleep_row = mk_row(card, "Screen off after", NULL);
    dd_sleep = lv_dropdown_create(sleep_row);
    lv_dropdown_set_options(dd_sleep, "Never\n5 minutes\n15 minutes\n30 minutes\n1 hour");
    lv_dropdown_set_selected(dd_sleep, s_sleep_idx);
    lv_obj_set_width(dd_sleep, 170);
    lv_obj_add_style(dd_sleep, &st_btn, 0);
    lv_obj_add_event_cb(dd_sleep, on_sleep, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *note = mk_label(card, &lv_font_montserrat_14, &st_muted,
                              "Backlight on this board is on/off only, so dim is done in "
                              "software. Tap the screen to wake.");
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

    lv_obj_t *nc = mk_card(p);
    lv_obj_set_flex_grow(nc, 1);
    lv_obj_set_height(nc, LV_PCT(100));
    lv_obj_set_style_pad_row(nc, 12, 0);
    lv_obj_t *nt = mk_label(nc, &lv_font_montserrat_14, &st_muted, "NETWORK");
    lv_obj_set_style_text_letter_space(nt, 1, 0);

    lv_obj_t *wb = mk_button(nc, LV_PCT(100), 48, "Wi-Fi setup", true, on_wifi_setup_btn, NULL);
    lv_obj_set_width(wb, LV_PCT(100));
    lv_obj_t *rb = mk_button(nc, LV_PCT(100), 40, "Refresh now", false, on_refresh_btn, NULL);
    lv_obj_set_width(rb, LV_PCT(100));

    lbl_info = mk_label(nc, &lv_font_montserrat_14, &st_muted, "");
    lv_obj_set_width(lbl_info, LV_PCT(100));
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
}

/* ------------------------------------------------------------ wifi setup --- */

static void on_network_picked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *txt = lv_list_get_btn_text(wifi_list, btn);
    if (!txt) return;
    /* The row reads "SSID   (-42 dBm)"; take everything before the padding. */
    const char *sep = strstr(txt, "   (");
    size_t n = sep ? (size_t)(sep - txt) : strlen(txt);
    if (n >= sizeof(s_wifi_ssid)) n = sizeof(s_wifi_ssid) - 1;
    memcpy(s_wifi_ssid, txt, n);
    s_wifi_ssid[n] = '\0';

    char msg[96];
    snprintf(msg, sizeof(msg), "network: %s - now type the password", s_wifi_ssid);
    ui_wifi_set_message(msg, false);
}

static void on_keyboard(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        if (s_wifi_ssid[0] == '\0') {
            ui_wifi_set_message("pick a network first", true);
            return;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "connecting to %s...", s_wifi_ssid);
        ui_wifi_set_message(msg, false);
        if (CB.wifi_connect) CB.wifi_connect(s_wifi_ssid, lv_textarea_get_text(ta_pass));
    } else if (code == LV_EVENT_CANCEL) {
        lv_scr_load(scr_root);
    }
}

static void on_rescan(lv_event_t *e)
{
    (void)e;
    ui_wifi_set_message("scanning...", false);
    if (CB.wifi_rescan) CB.wifi_rescan();
}

static void on_show_password(lv_event_t *e)
{
    bool show = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    lv_textarea_set_password_mode(ta_pass, !show);
}

static void on_wifi_back(lv_event_t *e)
{
    (void)e;
    lv_scr_load(scr_root);
}

static void build_wifi_screen(void)
{
    scr_wifi = lv_obj_create(NULL);
    lv_obj_add_style(scr_wifi, &st_screen, 0);
    lv_obj_clear_flag(scr_wifi, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(scr_wifi, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr_wifi, 0, 0);
    lv_obj_set_style_pad_row(scr_wifi, 0, 0);

    lv_obj_t *top = mk_flex(scr_wifi, LV_FLEX_FLOW_ROW, 20, 10);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *title = mk_label(top, &lv_font_montserrat_28, &st_text, "Wi-Fi setup");
    lv_obj_set_flex_grow(title, 1);
    mk_button(top, 90, 38, "back", false, on_wifi_back, NULL);
    mk_button(top, 110, 38, "rescan", false, on_rescan, NULL);

    lbl_wifi_msg = mk_label(scr_wifi, &lv_font_montserrat_14, &st_muted, "scanning...");
    lv_obj_set_style_pad_left(lbl_wifi_msg, 20, 0);
    lv_obj_set_width(lbl_wifi_msg, LV_PCT(100));

    lv_obj_t *mid = mk_flex(scr_wifi, LV_FLEX_FLOW_ROW, 20, 20);
    lv_obj_set_width(mid, LV_PCT(100));
    lv_obj_set_flex_grow(mid, 1);

    wifi_list = lv_list_create(mid);
    lv_obj_set_width(wifi_list, LV_PCT(50));
    lv_obj_set_height(wifi_list, LV_PCT(100));
    lv_obj_add_style(wifi_list, &st_card, 0);
    lv_obj_set_style_pad_all(wifi_list, 4, 0);

    lv_obj_t *right = mk_flex(mid, LV_FLEX_FLOW_COLUMN, 0, 12);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));

    ta_pass = lv_textarea_create(right);
    lv_obj_set_width(ta_pass, LV_PCT(100));
    lv_obj_set_height(ta_pass, 48);
    lv_textarea_set_placeholder_text(ta_pass, "password - tap here, then type");
    lv_textarea_set_one_line(ta_pass, true);
    lv_textarea_set_password_mode(ta_pass, true);
    lv_obj_set_style_border_color(ta_pass, C_ACCENT, 0);

    lv_obj_t *cb_show = lv_checkbox_create(right);
    lv_checkbox_set_text(cb_show, "show password");
    lv_obj_add_style(cb_show, &st_muted, 0);
    lv_obj_set_style_text_font(cb_show, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(cb_show, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(cb_show, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(cb_show, on_show_password, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *kb = lv_keyboard_create(scr_wifi);
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_height(kb, 236);
    lv_keyboard_set_textarea(kb, ta_pass);
    lv_obj_add_event_cb(kb, on_keyboard, LV_EVENT_ALL, NULL);
}

/* ------------------------------------------------------------------ init --- */

void ui_init(const ui_callbacks_t *cb)
{
    if (cb) CB = *cb; else memset(&CB, 0, sizeof(CB));

    palettes_init();
    P = s_dark ? pal_dark : pal_light;
    styles_init();

    scr_root = lv_obj_create(NULL);
    lv_obj_add_style(scr_root, &st_screen, 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(scr_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr_root, 0, 0);
    lv_obj_set_style_pad_row(scr_root, 0, 0);

    build_header(scr_root);

    /* The page area takes whatever is left between header and nav. Its children are
     * full-size and shown one at a time, so it carries no layout of its own. */
    page_area = mk_box(scr_root);
    lv_obj_set_width(page_area, LV_PCT(100));
    lv_obj_set_flex_grow(page_area, 1);

    build_usage_page();
    build_home_page();
    build_settings_page();
    build_dots(scr_root);
    lv_obj_add_event_cb(scr_root, on_gesture, LV_EVENT_GESTURE, NULL);
    build_wifi_screen();

    /* ABSOLUTE: a full-screen sheet on the top layer, not part of any flow. Not
     * clickable, so touches fall through to the UI underneath. */
    dimmer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(dimmer);
    lv_obj_set_size(dimmer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(dimmer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dimmer, LV_OPA_0, 0);
    lv_obj_clear_flag(dimmer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dimmer, LV_OBJ_FLAG_HIDDEN);

    s_built = true;
    ui_set_dim(s_dim);
    ui_show_page(UI_PAGE_USAGE);
    theme_refresh_locals();
    lv_scr_load(scr_root);
}

/* -------------------------------------------------------------- settings --- */

void ui_set_theme(bool dark)
{
    s_dark = dark;
    if (!s_built) return;
    P = dark ? pal_dark : pal_light;
    styles_write();
    lv_obj_report_style_change(&st_screen);
    lv_obj_report_style_change(&st_surface);
    lv_obj_report_style_change(&st_card);
    lv_obj_report_style_change(&st_line);
    lv_obj_report_style_change(&st_text);
    lv_obj_report_style_change(&st_muted);
    lv_obj_report_style_change(&st_btn);
    lv_obj_report_style_change(&st_btn_accent);
    theme_refresh_locals();
    if (sw_dark) {
        if (dark) lv_obj_add_state(sw_dark, LV_STATE_CHECKED);
        else      lv_obj_clear_state(sw_dark, LV_STATE_CHECKED);
    }
}

void ui_set_dim(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 70) percent = 70;
    s_dim = percent;
    if (!s_built) return;
    if (slider_dim && lv_slider_get_value(slider_dim) != percent)
        lv_slider_set_value(slider_dim, percent, LV_ANIM_OFF);
    if (percent <= 0) {
        lv_obj_add_flag(dimmer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(dimmer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(dimmer, (lv_opa_t)(255 * percent / 100), 0);
    }
}

void ui_set_sleep_index(int index)
{
    if (index < 0) index = 0;
    if (index > 4) index = 4;
    s_sleep_idx = index;
    if (s_built && dd_sleep) lv_dropdown_set_selected(dd_sleep, index);
}

bool ui_get_theme_dark(void) { return s_dark; }
int  ui_get_dim(void)        { return s_dim; }
int  ui_get_sleep_index(void){ return s_sleep_idx; }

void ui_show_page(ui_page_t page)
{
    if (!s_built || page >= UI_PAGE_COUNT) return;
    s_page = page;

    static const char *titles[UI_PAGE_COUNT] = {"Claude Usage", "Office", "Settings"};
    set_label(lbl_title, titles[page]);
    /* The mark belongs to the Claude page only. */
    if (logo_obj) {
        if (page == UI_PAGE_USAGE) lv_obj_clear_flag(logo_obj, LV_OBJ_FLAG_HIDDEN);
        else                       lv_obj_add_flag(logo_obj, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        if (i == (int)page) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else                lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        set_bg(page_dot[i], i == (int)page ? C_ACCENT : P.line, 0);
    }
}

ui_page_t ui_get_page(void) { return s_page; }

/* ------------------------------------------------------------------ data --- */

void ui_set_usage(const ui_usage_t *u)
{
    if (u) s_usage = *u;
    s_usage_at = s_now_ms;
    if (!s_built) return;

    float pct = s_usage.valid ? s_usage.session.pct : 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    lv_color_t col = s_usage.blocked ? C_HOT : severity(s_usage.session.pct);
    lv_arc_set_value(arc_session, (int)lroundf(pct));
    if (lv_obj_get_style_arc_color(arc_session, LV_PART_INDICATOR).full != col.full)
        lv_obj_set_style_arc_color(arc_session, col, LV_PART_INDICATOR);

    char buf[48];
    if (s_usage.valid) snprintf(buf, sizeof(buf), "%d%%", (int)lroundf(s_usage.session.pct));
    else               snprintf(buf, sizeof(buf), "--");
    set_label(lbl_pct, buf);

    struct { lv_obj_t *bar, *pct_lbl; const ui_gauge_t *g; } cards[2] = {
        {bar_week,  lbl_week_pct,  &s_usage.week},
        {bar_model, lbl_model_pct, &s_usage.model},
    };
    for (int i = 0; i < 2; i++) {
        float p = cards[i].g->pct > 100.0f ? 100.0f : cards[i].g->pct;
        lv_bar_set_value(cards[i].bar, (int)lroundf(p), LV_ANIM_OFF);
        set_bg(cards[i].bar, severity(cards[i].g->pct), LV_PART_INDICATOR);
        if (s_usage.valid) snprintf(buf, sizeof(buf), "%d%%", (int)lroundf(cards[i].g->pct));
        else               snprintf(buf, sizeof(buf), "--");
        set_label(cards[i].pct_lbl, buf);
    }

    if (s_usage.valid && s_usage.model_label[0]) {
        char title[48];
        snprintf(title, sizeof(title), "WEEKLY   %s", s_usage.model_label);
        for (char *c = title; *c; c++)
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        set_label(lbl_model_title, title);
    }
}

void ui_set_ha(const ui_ha_t *h)
{
    if (h) {
        ui_ha_t in = *h;
        for (int i = 0; i < UI_MAX_LIGHTS && i < in.light_count; i++) {
            if (!s_pending[i].active) continue;
            bool agreed = in.lights[i].on == s_pending[i].want_on &&
                          (s_pending[i].want_brightness < 0 ||
                           in.lights[i].brightness == s_pending[i].want_brightness);
            if (agreed || (uint32_t)(s_now_ms - s_pending[i].since) > UI_OPTIMISTIC_MS) {
                /* Confirmed, or we have guessed for long enough - the light may have
                 * been changed elsewhere, or the command may simply have failed. */
                s_pending[i].active = false;
            } else {
                in.lights[i].on = s_pending[i].want_on;
                if (s_pending[i].want_brightness >= 0)
                    in.lights[i].brightness = s_pending[i].want_brightness;
            }
        }
        s_ha = in;
    }
    if (!s_built) return;

    char b[128];
    bool show_values = s_ha.configured && s_ha.valid;

    if (!show_values) {
        if (!s_ha.configured)
            set_label(lbl_ha_msg, "Home Assistant not configured - add bridge/ha_config.json");
        else {
            snprintf(b, sizeof(b), "Home Assistant: %s",
                     s_ha.message[0] ? s_ha.message : "waiting for first reading");
            set_label(lbl_ha_msg, b);
        }
    } else {
        set_label(lbl_ha_msg, "");
    }

    /* With no climate entity there is nothing to show, and rendering 0 degrees would
     * look like a reading rather than an absence. Hide the controls and say so. */
    if (show_values && !s_ha.has_thermostat) {
        lv_obj_add_flag(thermo_body, LV_OBJ_FLAG_HIDDEN);
        set_label(lbl_thermo_name, "");
        set_label(lbl_thermo_mode, "no thermostat in Home Assistant");
    } else if (show_values) {
        lv_obj_clear_flag(thermo_body, LV_OBJ_FLAG_HIDDEN);
        set_label(lbl_thermo_name, s_ha.thermo_name);
        snprintf(b, sizeof(b), "%d%s", temp_i(s_ha.current), s_ha.unit);
        set_label(lbl_thermo_cur, b);
        if (s_ha.has_target) snprintf(b, sizeof(b), "%d%s", temp_i(s_ha.target), s_ha.unit);
        else                 snprintf(b, sizeof(b), "--");
        set_label(lbl_thermo_target, b);
        if (s_ha.action[0]) snprintf(b, sizeof(b), "%s  -  %s", s_ha.mode, s_ha.action);
        else                snprintf(b, sizeof(b), "%s", s_ha.mode);
        set_label(lbl_thermo_mode, b);
    }

    for (int i = 0; i < UI_MAX_LIGHTS; i++) {
        if (!show_values || i >= s_ha.light_count) {
            lv_obj_add_flag(light_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(light_row[i], LV_OBJ_FLAG_HIDDEN);
        set_label(light_name[i], s_ha.lights[i].name);
        bool checked = lv_obj_has_state(light_sw[i], LV_STATE_CHECKED);
        if (checked != s_ha.lights[i].on) {
            if (s_ha.lights[i].on) lv_obj_add_state(light_sw[i], LV_STATE_CHECKED);
            else                   lv_obj_clear_state(light_sw[i], LV_STATE_CHECKED);
        }
        if (lv_slider_get_value(light_slider[i]) != s_ha.lights[i].brightness)
            lv_slider_set_value(light_slider[i], s_ha.lights[i].brightness, LV_ANIM_OFF);
    }
}

void ui_set_status(ui_status_t status, uint32_t synced_age_s)
{
    if (!s_built) return;
    const char *text = "starting";
    lv_color_t dot = P.muted;
    char buf[64], d[24];

    switch (status) {
        case UI_STATUS_CONNECTED:      text = "connected";      dot = C_GOOD; break;
        case UI_STATUS_NO_WIFI:        text = "no wi-fi";       dot = C_HOT;  break;
        case UI_STATUS_BRIDGE_OFFLINE: text = "bridge offline"; dot = C_HOT;  break;
        case UI_STATUS_LIMIT_REACHED:  text = "limit reached";  dot = C_HOT;  break;
        case UI_STATUS_STALE:
            fmt_duration((long)synced_age_s, d, sizeof(d));
            snprintf(buf, sizeof(buf), "connected - synced %s ago", d);
            text = buf; dot = C_WARN; break;
        default: break;
    }
    set_label(lbl_status, text);
    set_bg(dot_status, dot, 0);
}

void ui_set_network_info(const char *ssid, const char *ip, const char *bridge_url,
                         const char *firmware_version)
{
    copy_str(s_info_ssid, sizeof(s_info_ssid), ssid);
    copy_str(s_info_ip, sizeof(s_info_ip), ip);
    copy_str(s_info_bridge, sizeof(s_info_bridge), bridge_url);
    copy_str(s_info_fw, sizeof(s_info_fw), firmware_version);
}

static void sub_text(const ui_gauge_t *g, char *out, size_t n)
{
    char d[24];
    fmt_duration(live_resets_in(g->resets_in), d, sizeof(d));
    if (strncmp(g->source, "api", 3) == 0) {
        snprintf(out, n, "resets in %s", d);
    } else {
        /* An estimate says so, so a guess never reads as fact. */
        char used[24], limit[24];
        fmt_tokens(g->used, used, sizeof(used));
        fmt_tokens(g->limit, limit, sizeof(limit));
        snprintf(out, n, "%s / %s  est.   resets in %s", used, limit, d);
    }
}

void ui_tick(uint32_t now_ms)
{
    s_now_ms = now_ms;
    if (!s_built) return;

    char buf[256], d[24];

    if (!s_usage.valid) {
        set_label(lbl_caption, "");
    } else if (s_usage.blocked) {
        fmt_duration(live_resets_in(s_usage.blocked_resets_in), d, sizeof(d));
        snprintf(buf, sizeof(buf), "LIMIT REACHED   %s", d);
        set_label(lbl_caption, buf);
        lv_obj_set_style_text_color(lbl_caption, C_HOT, 0);
    } else {
        fmt_duration(live_resets_in(s_usage.session.resets_in), d, sizeof(d));
        snprintf(buf, sizeof(buf), "resets in %s", d);
        set_label(lbl_caption, buf);
        lv_obj_remove_local_style_prop(lbl_caption, LV_STYLE_TEXT_COLOR, 0);
    }

    if (s_usage.valid) {
        sub_text(&s_usage.week, buf, sizeof(buf));
        set_label(lbl_week_sub, buf);
        sub_text(&s_usage.model, buf, sizeof(buf));
        set_label(lbl_model_sub, buf);
    }

    if (s_page == UI_PAGE_SETTINGS) {
        snprintf(buf, sizeof(buf), "%s\n%s\n\nbridge  %s\nfirmware  v%s",
                 s_info_ssid[0] ? s_info_ssid : "-",
                 s_info_ip[0] ? s_info_ip : "not connected",
                 s_info_bridge, s_info_fw);
        set_label(lbl_info, buf);
    }
}

/* ------------------------------------------------------------ wifi setup --- */

void ui_show_wifi_setup(void)
{
    if (!s_built) return;
    s_wifi_ssid[0] = '\0';
    lv_textarea_set_text(ta_pass, "");
    lv_scr_load(scr_wifi);
}

void ui_wifi_set_networks(const ui_network_t *nets, int count)
{
    if (!s_built) return;
    lv_obj_clean(wifi_list);
    for (int i = 0; i < count; i++) {
        char label[64];
        snprintf(label, sizeof(label), "%s   (%d dBm)", nets[i].ssid, (int)nets[i].rssi);
        lv_obj_t *btn = lv_list_add_btn(wifi_list, LV_SYMBOL_WIFI, label);
        lv_obj_add_style(btn, &st_text, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_0, 0);
        lv_obj_add_event_cb(btn, on_network_picked, LV_EVENT_CLICKED, NULL);
    }
    ui_wifi_set_message(count ? "pick a network, then type the password"
                              : "no networks found - tap rescan", false);
}

void ui_wifi_set_message(const char *message, bool is_error)
{
    if (!s_built) return;
    set_label(lbl_wifi_msg, message ? message : "");
    if (is_error) lv_obj_set_style_text_color(lbl_wifi_msg, C_HOT, 0);
    else          lv_obj_remove_local_style_prop(lbl_wifi_msg, LV_STYLE_TEXT_COLOR, 0);
}

bool ui_wifi_is_active(void)
{
    return s_built && lv_scr_act() == scr_wifi;
}
