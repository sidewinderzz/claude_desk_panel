/*
 * ui_stub.c - TEMPORARY placeholder implementation of ui.h.
 *
 * This is NOT the real UI and must never be shipped. It exists only so the
 * simulator harness can be compiled, linked, run and verified end-to-end
 * before ui/ui.c lands. The Makefile prefers ../ui/ui.c whenever that file
 * exists and only falls back to this stub when it does not.
 *
 * Delete this file once ui/ui.c is in place and the fallback stops firing.
 */

#include "../ui/ui.h"

#include <stdio.h>
#include <string.h>

const uint32_t ui_sleep_ms[5] = { 0, 30000, 60000, 300000, 900000 };

/* ------------------------------------------------------------------ state --- */

static ui_callbacks_t s_cb;
static bool           s_dark        = true;
static int            s_dim         = 0;
static int            s_sleep_index = 0;
static ui_page_t      s_page        = UI_PAGE_USAGE;
static bool           s_wifi_active = false;

static lv_obj_t *s_root;
static lv_obj_t *s_card;
static lv_obj_t *s_title;
static lv_obj_t *s_body;
static lv_obj_t *s_footer;
static lv_obj_t *s_dim_overlay;
static lv_obj_t *s_wifi_screen;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_wifi_msg;

static ui_usage_t  s_usage;
static ui_ha_t     s_ha;
static ui_status_t s_status = UI_STATUS_STARTING;
static uint32_t    s_synced_age;
static char        s_ssid[33];
static char        s_ip[40];
static char        s_bridge[80];
static char        s_fw[24];

/* ----------------------------------------------------------------- helpers -- */

static const char *page_name(ui_page_t p)
{
    switch (p) {
        case UI_PAGE_USAGE:    return "USAGE";
        case UI_PAGE_HOME:     return "HOME";
        case UI_PAGE_SETTINGS: return "SETTINGS";
        default:               return "?";
    }
}

static const char *status_name(ui_status_t s)
{
    switch (s) {
        case UI_STATUS_STARTING:       return "starting";
        case UI_STATUS_CONNECTED:      return "connected";
        case UI_STATUS_STALE:          return "stale";
        case UI_STATUS_NO_WIFI:        return "no wifi";
        case UI_STATUS_BRIDGE_OFFLINE: return "bridge offline";
        case UI_STATUS_LIMIT_REACHED:  return "limit reached";
        default:                       return "?";
    }
}

static void apply_theme(void)
{
    if (!s_root) return;

    lv_obj_set_style_bg_color(s_root, s_dark ? lv_color_hex(0x101418) : lv_color_hex(0xf2f4f7), 0);
    lv_obj_set_style_bg_color(s_card, s_dark ? lv_color_hex(0x1d242c) : lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_color(s_card, s_dark ? lv_color_hex(0x3a4652) : lv_color_hex(0xc8d0d8), 0);

    lv_color_t fg = s_dark ? lv_color_hex(0xf4f7fa) : lv_color_hex(0x161b21);
    lv_obj_set_style_text_color(s_title, fg, 0);
    lv_obj_set_style_text_color(s_body, fg, 0);
    lv_obj_set_style_text_color(s_footer, s_dark ? lv_color_hex(0x8d9aa8) : lv_color_hex(0x5c6773), 0);
}

static void apply_dim(void)
{
    if (!s_dim_overlay) return;
    lv_obj_set_style_bg_opa(s_dim_overlay, (lv_opa_t)((s_dim * 255) / 100), 0);
}

static void refresh_text(void)
{
    char buf[512];

    if (!s_root) return;

    lv_label_set_text_fmt(s_title, "%s  -  stub UI", page_name(s_page));

    if (s_usage.valid) {
        snprintf(buf, sizeof buf,
                 "session  %5.1f%%  (%ld/%ld)  [%s]\n"
                 "week     %5.1f%%  (%ld/%ld)  [%s]\n"
                 "%-8s %5.1f%%  (%ld/%ld)  [%s]\n"
                 "%s",
                 (double)s_usage.session.pct, s_usage.session.used, s_usage.session.limit, s_usage.session.source,
                 (double)s_usage.week.pct,    s_usage.week.used,    s_usage.week.limit,    s_usage.week.source,
                 s_usage.model_label[0] ? s_usage.model_label : "model",
                 (double)s_usage.model.pct,   s_usage.model.used,   s_usage.model.limit,   s_usage.model.source,
                 s_usage.blocked ? "BLOCKED" : "");
    }
    else {
        snprintf(buf, sizeof buf, "session  --\nweek     --\nmodel    --");
    }
    lv_label_set_text(s_body, buf);

    snprintf(buf, sizeof buf,
             "%s | synced %us ago | ha:%s | %s %s | dim %d%% | sleep #%d | fw %s",
             status_name(s_status), (unsigned)s_synced_age,
             s_ha.valid ? s_ha.thermo_name : (s_ha.message[0] ? s_ha.message : "n/a"),
             s_ssid[0] ? s_ssid : "-", s_ip[0] ? s_ip : "-",
             s_dim, s_sleep_index, s_fw[0] ? s_fw : "-");
    lv_label_set_text(s_footer, buf);
}

/* -------------------------------------------------------------------- api --- */

void ui_init(const ui_callbacks_t *cb)
{
    if (cb) s_cb = *cb;
    else    memset(&s_cb, 0, sizeof s_cb);

    s_root = lv_scr_act();
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);

    /* A plain rectangle, so a blank render is obviously a failure. */
    s_card = lv_obj_create(s_root);
    lv_obj_set_size(s_card, 720, 380);
    lv_obj_align(s_card, LV_ALIGN_CENTER, 0, -12);
    lv_obj_set_style_radius(s_card, 16, 0);
    lv_obj_set_style_border_width(s_card, 2, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_28, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 8, 8);

    s_body = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_body, &lv_font_montserrat_20, 0);
    lv_obj_align(s_body, LV_ALIGN_LEFT_MID, 8, 0);

    s_footer = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_footer, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_footer, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_footer, 760);
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_MID, 0, -12);

    /* Software dim overlay, matching what the real UI is expected to do. */
    s_dim_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_dim_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_dim_overlay, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_dim_overlay, 0, 0);
    lv_obj_set_style_radius(s_dim_overlay, 0, 0);
    lv_obj_clear_flag(s_dim_overlay, LV_OBJ_FLAG_CLICKABLE);

    apply_theme();
    apply_dim();
    refresh_text();
}

void ui_set_theme(bool dark)
{
    s_dark = dark;
    apply_theme();
}

void ui_set_dim(int percent)
{
    if (percent < 0)  percent = 0;
    if (percent > 70) percent = 70;
    s_dim = percent;
    apply_dim();
    refresh_text();
}

void ui_set_sleep_index(int index)
{
    s_sleep_index = index;
    refresh_text();
}

bool ui_get_theme_dark(void) { return s_dark; }
int  ui_get_dim(void)        { return s_dim; }
int  ui_get_sleep_index(void){ return s_sleep_index; }

void ui_show_page(ui_page_t page)
{
    if (page < 0 || page >= UI_PAGE_COUNT) return;
    s_wifi_active = false;
    if (s_root) lv_scr_load(s_root);
    if (s_page != page) {
        s_page = page;
        if (s_cb.page_changed) s_cb.page_changed(page);
    }
    refresh_text();
}

ui_page_t ui_get_page(void) { return s_page; }

void ui_set_usage(const ui_usage_t *usage)
{
    if (!usage) return;
    s_usage = *usage;
    refresh_text();
}

void ui_set_ha(const ui_ha_t *ha)
{
    if (!ha) return;
    s_ha = *ha;
    refresh_text();
}

void ui_set_status(ui_status_t status, uint32_t synced_age_s)
{
    s_status     = status;
    s_synced_age = synced_age_s;
    refresh_text();
}

void ui_set_network_info(const char *ssid, const char *ip, const char *bridge_url,
                         const char *firmware_version)
{
    snprintf(s_ssid,   sizeof s_ssid,   "%s", ssid ? ssid : "");
    snprintf(s_ip,     sizeof s_ip,     "%s", ip ? ip : "");
    snprintf(s_bridge, sizeof s_bridge, "%s", bridge_url ? bridge_url : "");
    snprintf(s_fw,     sizeof s_fw,     "%s", firmware_version ? firmware_version : "");
    refresh_text();
}

void ui_tick(uint32_t now_ms)
{
    (void)now_ms;   /* the stub has no countdowns */
}

void ui_show_wifi_setup(void)
{
    if (!s_wifi_screen) {
        s_wifi_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_wifi_screen, lv_color_hex(0x0d1117), 0);

        lv_obj_t *t = lv_label_create(s_wifi_screen);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(0xf4f7fa), 0);
        lv_label_set_text(t, "Wi-Fi Setup (stub)");
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);

        s_wifi_msg = lv_label_create(s_wifi_screen);
        lv_obj_set_style_text_font(s_wifi_msg, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_wifi_msg, lv_color_hex(0x9fb0c0), 0);
        lv_label_set_text(s_wifi_msg, "");
        lv_obj_align(s_wifi_msg, LV_ALIGN_TOP_MID, 0, 62);

        s_wifi_list = lv_label_create(s_wifi_screen);
        lv_obj_set_style_text_font(s_wifi_list, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_wifi_list, lv_color_hex(0xe6edf3), 0);
        lv_label_set_text(s_wifi_list, "(no scan yet)");
        lv_obj_align(s_wifi_list, LV_ALIGN_CENTER, 0, 20);
    }

    s_wifi_active = true;
    lv_scr_load(s_wifi_screen);
    if (s_cb.wifi_setup_requested) s_cb.wifi_setup_requested();
}

void ui_wifi_set_networks(const ui_network_t *nets, int count)
{
    char buf[1024];
    size_t off = 0;

    if (!s_wifi_list) return;

    buf[0] = '\0';
    for (int i = 0; i < count && off + 64 < sizeof buf; i++) {
        off += (size_t)snprintf(buf + off, sizeof buf - off, "%-24s %4d dBm\n",
                                nets[i].ssid, (int)nets[i].rssi);
    }
    lv_label_set_text(s_wifi_list, buf[0] ? buf : "(no networks)");
}

void ui_wifi_set_message(const char *message, bool is_error)
{
    if (!s_wifi_msg) return;
    lv_obj_set_style_text_color(s_wifi_msg,
                                is_error ? lv_color_hex(0xff6b6b) : lv_color_hex(0x9fb0c0), 0);
    lv_label_set_text(s_wifi_msg, message ? message : "");
}

bool ui_wifi_is_active(void) { return s_wifi_active; }
