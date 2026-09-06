/*
 * ui.h - hardware-independent UI for the Claude usage widget.
 *
 * Pure LVGL 8 and C99. No Arduino, no WiFi, no FreeRTOS, no ESP-IDF, no board
 * headers. Everything the UI needs is pushed in through ui_set_*(); everything the
 * UI wants done is handed back through ui_callbacks_t. That is what lets the same
 * translation unit build for the ESP32 firmware and for the host renderer in sim/.
 *
 * Threading: the caller owns the LVGL lock. On the device that means wrapping every
 * ui_* call from loop() in lvgl_port_lock()/unlock(). Callbacks fire from inside
 * LVGL (which already holds the lock) and must not block - set a flag and return.
 *
 * Designed for a 800x480 panel but laid out with flex and percentages, so it adapts
 * to whatever lv_disp the caller has registered.
 */

#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ types --- */

typedef enum {
    UI_PAGE_USAGE = 0,
    UI_PAGE_CLOCK = 1,
    UI_PAGE_HOME = 2,
    UI_PAGE_SETTINGS = 3,
    UI_PAGE_COUNT
} ui_page_t;

typedef enum {
    UI_STATUS_STARTING = 0,
    UI_STATUS_CONNECTED,     /* fresh data */
    UI_STATUS_STALE,         /* connected, but last good fetch is old */
    UI_STATUS_NO_WIFI,
    UI_STATUS_BRIDGE_OFFLINE,
    UI_STATUS_LIMIT_REACHED
} ui_status_t;

/* One gauge as the bridge reports it. `source` is "api", "api-scaled",
 * "calibrated", "rejection" or "fallback" - anything starting "api" is a real
 * number and the UI shows only a countdown; otherwise it labels itself an estimate. */
typedef struct {
    float pct;
    long used;
    long limit;
    long resets_in;      /* seconds, as of fetched_at */
    char source[16];
} ui_gauge_t;

typedef struct {
    bool valid;              /* false -> gauges render as "--" */
    bool blocked;
    long blocked_resets_in;
    ui_gauge_t session;
    ui_gauge_t week;
    ui_gauge_t model;
    char model_label[24];    /* e.g. "Fable" -> card title "WEEKLY   FABLE" */
} ui_usage_t;

#define UI_MAX_LIGHTS 3

typedef struct {
    char name[32];
    bool on;
    int brightness;          /* 0..255 */
} ui_light_t;

typedef struct {
    bool configured;         /* bridge has an ha_config.json */
    bool valid;              /* at least one good fetch */
    bool has_thermostat;     /* a climate entity is configured and readable */
    char message[80];        /* shown instead of values when !valid */
    char thermo_name[32];
    float current;
    float target;
    bool has_target;
    char mode[16];
    char action[16];
    char unit[4];
    int light_count;
    ui_light_t lights[UI_MAX_LIGHTS];
} ui_ha_t;

/* Weather glyph families. The bridge maps WMO codes onto these, so the device
 * carries no weather table; ui_wx_icon_from_name() parses the bridge's strings. */
typedef enum {
    UI_WX_NONE = 0,
    UI_WX_SUN,
    UI_WX_MOON,
    UI_WX_PARTLY,            /* sun behind cloud */
    UI_WX_PARTLY_NIGHT,      /* moon behind cloud */
    UI_WX_CLOUD,
    UI_WX_FOG,
    UI_WX_RAIN,
    UI_WX_SNOW,
    UI_WX_STORM
} ui_wx_icon_t;

#define UI_FORECAST_DAYS 5

typedef struct {
    char day[8];             /* "Sat" */
    ui_wx_icon_t icon;
    int hi, lo;              /* already in the display unit */
    int precip;              /* chance of precipitation, 0..100 */
} ui_forecast_t;

typedef struct {
    bool configured;         /* the bridge knows a location */
    bool valid;              /* at least one good fetch */
    char message[80];        /* shown instead of values when !valid */
    char location[32];
    char unit[4];            /* "F" / "C" */
    char wind_unit[8];       /* "mph" / "km/h" */
    int temp, feels, humidity, wind;
    ui_wx_icon_t icon;
    char text[32];           /* "Partly cloudy" */
    int day_count;
    ui_forecast_t days[UI_FORECAST_DAYS];
} ui_weather_t;

/* Local wall-clock time, already broken down. The UI never reads a clock itself. */
typedef struct {
    bool valid;              /* false until the clock has been set -> "--:--" */
    int hour, minute, second;/* 24-hour */
    int weekday;             /* 0 = Sunday */
    int day, month, year;    /* month 1..12 */
} ui_clock_t;

typedef struct {
    char ssid[33];
    int32_t rssi;
} ui_network_t;

/* Everything the UI asks the application to do. Any of these may be NULL. */
typedef struct {
    void (*page_changed)(ui_page_t page);
    void (*thermo_delta)(int degrees);              /* +1 / -1 */
    void (*light_set)(int index, int on, int brightness); /* brightness <0 = no change */
    void (*theme_changed)(bool dark);
    void (*dim_changed)(int percent);               /* 0..70, on release */
    void (*sleep_changed)(int index);               /* index into ui_sleep_ms[] */
    void (*clock_24h_changed)(bool on);
    void (*wifi_setup_requested)(void);
    void (*wifi_connect)(const char *ssid, const char *password);
    void (*wifi_rescan)(void);
    void (*refresh_requested)(void);
} ui_callbacks_t;

/* Screen-off delays matching the settings dropdown. */
extern const uint32_t ui_sleep_ms[5];

/* ------------------------------------------------------------------- api ---- */

/* Build styles, the root screen and the wifi screen. Call once, after lv_init()
 * and after a display driver is registered. `cb` is copied. */
void ui_init(const ui_callbacks_t *cb);

/* Restore persisted preferences before or after ui_init(); safe either way. */
void ui_set_theme(bool dark);
void ui_set_dim(int percent);          /* 0..70 software dim overlay */
void ui_set_sleep_index(int index);
void ui_set_clock_24h(bool on);

bool ui_get_theme_dark(void);
int  ui_get_dim(void);
int  ui_get_sleep_index(void);
bool ui_get_clock_24h(void);

void ui_show_page(ui_page_t page);
ui_page_t ui_get_page(void);

/* Data in. Each is cheap to call repeatedly - nothing is written to a widget
 * unless the rendered value actually changed. */
void ui_set_usage(const ui_usage_t *usage);
void ui_set_ha(const ui_ha_t *ha);
void ui_set_weather(const ui_weather_t *weather);
void ui_set_clock(const ui_clock_t *clock);
void ui_set_status(ui_status_t status, uint32_t synced_age_s);
void ui_set_network_info(const char *ssid, const char *ip, const char *bridge_url,
                         const char *firmware_version);

/* "sun", "moon", "partly", "partly-night", "cloud", "fog", "rain", "snow", "storm". */
ui_wx_icon_t ui_wx_icon_from_name(const char *name);

/* Countdowns. `now_ms` is any monotonic millisecond clock; the UI diffs it against
 * the value passed to ui_set_usage() rather than calling a clock itself. */
void ui_tick(uint32_t now_ms);

/* Wi-Fi setup screen. */
void ui_show_wifi_setup(void);
void ui_wifi_set_networks(const ui_network_t *nets, int count);
void ui_wifi_set_message(const char *message, bool is_error);
bool ui_wifi_is_active(void);          /* true while the setup screen is loaded */

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
