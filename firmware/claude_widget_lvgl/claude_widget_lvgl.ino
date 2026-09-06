/*
 * Claude usage widget - Waveshare ESP32-S3-Touch-LCD-4.3B
 *
 * This sketch is the hardware and network half only. Every pixel lives in ui.c,
 * which is pure LVGL/C99 and builds unchanged against the host renderer in sim/ -
 * so layout work happens there, at a two-second loop, not at 90 seconds a flash.
 *
 * ui.c / ui.h / burst_img.* are copied into this folder by build.ps1 from ../../ui
 * and ../../assets, because the Arduino build only compiles what is in the sketch
 * directory. Do not edit the copies; they are gitignored and overwritten each build.
 *
 * Division of labour:
 *   ui.c      widgets, layout, theme. Knows nothing about WiFi, HTTP or the board.
 *   this file board bring-up, WiFi, the bridge, Home Assistant, NVS, backlight.
 *   callbacks fire on the LVGL task -> set a flag, return. loop() does the slow part.
 *
 * Requires esp32 core 3.3.11 (3.3.8's station path fails every join with
 * AUTH_EXPIRE), OPI PSRAM, 16MB flash. See README.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#include <esp_display_panel.hpp>
#include <esp_lcd_panel_rgb.h>
#include <lvgl.h>

#include "lvgl_v8_port.h"
#include "config.h"
#include "ui.h"

using namespace esp_panel::board;
using namespace esp_panel::drivers;

#define FIRMWARE_VERSION "0.7"

static Board *board = nullptr;
static Preferences prefs;

static char wifiSsid[33] = "";
static char wifiPass[65] = "";

static ui_usage_t usage;
static ui_ha_t haState;

/* Requests raised by UI callbacks, serviced in loop(). */
static volatile bool reqScan = false;
static volatile bool reqConnect = false;
static volatile bool reqRefresh = false;
static volatile bool reqSaveSettings = false;
static volatile int reqThermoDelta = 0;
static volatile int reqLightIdx = -1;
static volatile int reqLightOn = -1;
static volatile int reqLightBri = -1;
static volatile bool reqHaRefresh = false;

static char pendingSsid[33] = "";
static char pendingPass[65] = "";
static bool netPendingSave = false;

static unsigned long lastPoll = 0, lastHaPoll = 0, lastTick = 0;
static unsigned long fetchedAt = 0;
static bool screenOff = false;
static unsigned long screenOffAt = 0;
static bool haveUsage = false;

/* --------------------------------------------------------------- board --- */

static bool boardInit()
{
    board = new Board();
    board->init();

    /* Anti-drift settings for this panel under this stack. The RGB peripheral
     * streams the frame out of PSRAM continuously; at the preset's 16 MHz the DMA
     * can fall behind and leave every frame permanently shifted. Set after init(),
     * before begin(). Two frame buffers back anti-tearing mode 3 in the port. */
    auto bus = static_cast<BusRGB *>(board->getLCD()->getBus());
    bus->configRGB_BounceBufferSize(800 * 20);
    bus->configRGB_FreqHz(12 * 1000 * 1000);
    bus->configRGB_FrameBufferNumber(2);

    if (!board->begin()) {
        Serial.println("[board] Board::begin() FAILED");
        return false;
    }
    if (!psramFound())
        Serial.println("[board] WARNING: no PSRAM - check the OPI PSRAM board setting");
    if (!lvgl_port_init(board->getLCD(), board->getTouch())) {
        Serial.println("[board] lvgl_port_init FAILED");
        return false;
    }
    Serial.println("[board] init OK");
    return true;
}

static void backlight(bool on)
{
    if (!board || !board->getBacklight()) return;

    /* Not just a GPIO toggle. On this board the backlight is a CH422G expander
     * pin, so this is an I2C write to 0x20 on bus 0 (SCL 9 / SDA 8) - the same
     * bus the GT911 touch controller sits on at 0x5D. The LVGL task reads the
     * touch from inside lv_timer_handler(), which it runs under this same lock.
     *
     * Without the lock the two transactions can interleave, and the CH422G has
     * one 8-bit output register holding BOTH the backlight and LCD_RST: a
     * mangled read-modify-write lands a byte with LCD_RST low and the ST7262
     * sits in reset. That is a white screen with the MCU still running happily,
     * which reads like a hang or a boot loop but is neither.
     *
     * Wake-on-tap is exactly when the two collide: the tap that triggers this
     * call is the same tap the LVGL task is reading over I2C. The mutex is
     * recursive, so taking it here is safe from anywhere. */
    lvgl_port_lock(-1);
    if (on) board->getBacklight()->on();
    else    board->getBacklight()->off();
    lvgl_port_unlock();
}

/* ---------------------------------------------------------- diagnostics ---
 *
 * The panel can end up showing flat cycling colours while the firmware carries
 * on running: no reset, no panic, the poll loop keeps logging. That is the
 * signature of the RGB peripheral losing sync with its DMA rather than
 * anything wrong with the UI, and ESP-IDF has a documented recovery for it -
 * esp_lcd_rgb_panel_restart(), "to save the screen from a permanent shift".
 *
 * displayKick() tries that, then forces LVGL to repaint every pixel. If a kick
 * brings the UI back, the fault is the RGB DMA; if it does not, the fault is
 * below us in the panel or the CH422G, and this rules a whole class out.
 * -------------------------------------------------------------------------- */

static void logHealth(const char *why)
{
    Serial.printf("[diag] %s up=%lus heap=%u/%u psram=%u wifi=%d rssi=%d\n",
                  why, millis() / 1000UL,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize(),
                  (unsigned)ESP.getFreePsram(),
                  (int)WiFi.status(), (int)WiFi.RSSI());
}

static void displayKick(const char *why)
{
    esp_lcd_panel_handle_t panel = nullptr;
    if (board && board->getLCD()) panel = board->getLCD()->getHandle();

    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (panel) err = esp_lcd_rgb_panel_restart(panel);
    Serial.printf("[diag] displayKick(%s) restart=%s\n", why, esp_err_to_name(err));

    lvgl_port_lock(-1);
    lv_obj_invalidate(lv_scr_act());
    lvgl_port_unlock();
}

/* Single-letter commands on the serial line, so the failure can be driven from
 * a PC in seconds instead of waiting out a 30-minute sleep timeout. */
static void serialCommands()
{
    while (Serial.available()) {
        int c = Serial.read();
        switch (c) {
        case 'i': logHealth("cmd"); break;
        case 'r': displayKick("cmd"); break;
        case 's':
            screenOff = true;
            backlight(false);
            Serial.println("[power] screen off (cmd)");
            break;
        case 'w':
            screenOff = false;
            backlight(true);
            Serial.println("[power] screen on (cmd)");
            break;
        case 'b': backlight(false); Serial.println("[diag] backlight off only"); break;
        case 'B': backlight(true);  Serial.println("[diag] backlight on only"); break;
        case '?':
            Serial.println("[diag] i=info r=kick s=sleep w=wake b/B=backlight only");
            break;
        default: break;
        }
    }
}

/* ------------------------------------------------------------ settings --- */

static void loadSettings()
{
    prefs.begin("claudewgt", true);
    String s = prefs.getString("ssid", "");
    String p = prefs.getString("pass", "");
    bool dark = prefs.getBool("dark", true);
    int dim = prefs.getInt("dim", 0);
    int sleep = prefs.getInt("sleep", 0);
    prefs.end();

    snprintf(wifiSsid, sizeof(wifiSsid), "%s", s.c_str());
    snprintf(wifiPass, sizeof(wifiPass), "%s", p.c_str());
    ui_set_theme(dark);
    ui_set_dim(dim);
    ui_set_sleep_index(sleep);
    if (s.length()) Serial.printf("[wifi] stored credentials for %s\n", wifiSsid);
}

static void saveCredentials()
{
    prefs.begin("claudewgt", false);
    prefs.putString("ssid", wifiSsid);
    prefs.putString("pass", wifiPass);
    prefs.end();
    Serial.printf("[wifi] saved credentials for %s\n", wifiSsid);
}

static void saveSettings()
{
    prefs.begin("claudewgt", false);
    prefs.putBool("dark", ui_get_theme_dark());
    prefs.putInt("dim", ui_get_dim());
    prefs.putInt("sleep", ui_get_sleep_index());
    prefs.end();
}

/* ----------------------------------------------------------- callbacks --- */
/* All of these run on the LVGL task. Set a flag and return - never block here. */

static void cbPage(ui_page_t page)          { if (page == UI_PAGE_HOME) reqHaRefresh = true; }
static void cbThermo(int d)                 { reqThermoDelta += d; }
static void cbLight(int i, int on, int bri) { reqLightIdx = i; reqLightOn = on; reqLightBri = bri; }
static void cbTheme(bool)                   { reqSaveSettings = true; }
static void cbDim(int)                      { reqSaveSettings = true; }
static void cbSleep(int)                    { reqSaveSettings = true; }
static void cbWifiSetup(void)               { reqScan = true; }
static void cbRescan(void)                  { reqScan = true; }
static void cbRefresh(void)                 { reqRefresh = true; }

static void cbWifiConnect(const char *ssid, const char *pass)
{
    snprintf(pendingSsid, sizeof(pendingSsid), "%s", ssid ? ssid : "");
    snprintf(pendingPass, sizeof(pendingPass), "%s", pass ? pass : "");
    reqConnect = true;
}

/* ------------------------------------------------------------ networking -- */

static void readGauge(JsonObjectConst src, ui_gauge_t &dst)
{
    dst.used = src["used"] | 0L;
    dst.limit = src["limit"] | 0L;
    dst.pct = src["pct"] | 0.0f;
    dst.resets_in = src["resets_in"] | 0L;
    snprintf(dst.source, sizeof(dst.source), "%s", src["source"] | "");
}

static bool httpGetJson(const char *url, JsonDocument &doc)
{
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(url)) return false;
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[http] GET %s -> %d\n", url, code);
        http.end();
        return false;
    }
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) { Serial.printf("[json] %s\n", err.c_str()); return false; }
    return true;
}

static bool httpPostJson(const char *url, const char *body)
{
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(url)) return false;
    http.addHeader("Content-Type", "application/json");
    int code = http.POST((uint8_t *)body, strlen(body));
    if (code < 200 || code >= 300) Serial.printf("[http] POST -> %d\n", code);
    http.end();
    return code >= 200 && code < 300;
}

/* BRIDGE_URL ends in /usage; the HA endpoints sit beside it. */
static void bridgeUrl(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s", BRIDGE_URL);
    char *slash = strrchr(out, '/');
    if (slash) *slash = 0;
    strncat(out, path, n - strlen(out) - 1);
}

static bool fetchUsage()
{
    JsonDocument doc;
    bool ok = httpGetJson(BRIDGE_URL, doc) && (doc["ok"] | false);
    if (ok) {
        readGauge(doc["session"], usage.session);
        readGauge(doc["week"], usage.week);
        readGauge(doc["model"], usage.model);
        snprintf(usage.model_label, sizeof(usage.model_label), "%s", doc["model_label"] | "MODEL");
        usage.blocked = doc["blocked"] | false;
        usage.blocked_resets_in = doc["blocked_resets_in"] | 0L;
        usage.valid = true;
        haveUsage = true;
        fetchedAt = millis();
        Serial.printf("[usage] session %.1f%%  week %.1f%%  %s %.1f%%  (%s)\n",
                      usage.session.pct, usage.week.pct, usage.model_label,
                      usage.model.pct, usage.session.source);
    } else {
        usage.valid = haveUsage; /* keep the last good numbers on a transient failure */
    }
    lvgl_port_lock(-1);
    ui_set_usage(&usage);
    lvgl_port_unlock();
    return ok;
}

static bool fetchHA()
{
    char url[128];
    bridgeUrl("/ha/state", url, sizeof(url));
    JsonDocument doc;

    if (!httpGetJson(url, doc)) {
        snprintf(haState.message, sizeof(haState.message), "bridge unreachable");
        haState.valid = false;
    } else {
        haState.configured = doc["configured"] | false;
        if (!haState.configured || !(doc["ok"] | false)) {
            snprintf(haState.message, sizeof(haState.message), "%s", doc["error"] | "error");
            haState.valid = false;
        } else {
            JsonObjectConst t = doc["thermostat"];
            haState.has_thermostat = !t.isNull();
            snprintf(haState.thermo_name, sizeof(haState.thermo_name), "%s", t["name"] | "");
            haState.current = t["current"] | 0.0f;
            haState.has_target = !t["target"].isNull();
            haState.target = t["target"] | 0.0f;
            snprintf(haState.mode, sizeof(haState.mode), "%s", t["mode"] | "");
            snprintf(haState.action, sizeof(haState.action), "%s", t["action"] | "");
            snprintf(haState.unit, sizeof(haState.unit), "%s", t["unit"] | "");
            haState.light_count = 0;
            for (JsonObjectConst l : doc["lights"].as<JsonArrayConst>()) {
                if (haState.light_count >= UI_MAX_LIGHTS) break;
                ui_light_t &hl = haState.lights[haState.light_count++];
                snprintf(hl.name, sizeof(hl.name), "%s", l["name"] | "");
                hl.on = l["on"] | false;
                hl.brightness = l["brightness"] | 0;
            }
            haState.valid = true;
            haState.message[0] = 0;
        }
    }
    lvgl_port_lock(-1);
    ui_set_ha(&haState);
    lvgl_port_unlock();
    return haState.valid;
}

/* The bridge holds the HA token and knows the entity ids; the device only names
 * the light by index, so entity ids never travel to the panel. */
static void haCall(const char *json)
{
    char url[128];
    bridgeUrl("/ha/call", url, sizeof(url));
    Serial.printf("[ha] %s\n", json);
    httpPostJson(url, json);
    reqHaRefresh = true;
}

/* ----------------------------------------------------------------- wifi --- */

enum NetStatus { NET_OFFLINE = 0, NET_CONNECTING, NET_ONLINE };
static NetStatus netState = NET_OFFLINE;
static unsigned long netAttemptStart = 0, netLastReconnect = 0;
static bool netFailureShown = false;
static volatile int lastDisconnectReason = 0;
static const unsigned long RECONNECT_INTERVAL_MS = 15000;

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        lastDisconnectReason = info.wifi_sta_disconnected.reason;
        Serial.printf("[wifi] disconnected, reason %d\n", lastDisconnectReason);
    }
}

static const char *describeWiFiFailure()
{
    switch (lastDisconnectReason) {
        case 15: case 202: return "wrong password - check it and try again";
        case 201:          return "network not found - is it 2.4 GHz?";
        default:           return "could not connect - try again";
    }
}

static void netBegin()
{
    if (strlen(wifiSsid) == 0) return;
    lastDisconnectReason = 0;
    netFailureShown = false;
    netAttemptStart = netLastReconnect = millis();
    netState = NET_CONNECTING;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid, wifiPass);
    Serial.printf("[net] connecting to %s\n", wifiSsid);
}

static void netLoop()
{
    unsigned long now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        if (netState == NET_ONLINE) Serial.println("[net] lost connection");
        if (netState == NET_OFFLINE) return;
        netState = NET_CONNECTING;

        if (!netFailureShown && now - netAttemptStart > WIFI_CONNECT_MS) {
            netFailureShown = true;
            Serial.printf("[net] no association after %lus (reason %d)\n",
                          WIFI_CONNECT_MS / 1000UL, (int)lastDisconnectReason);
            lvgl_port_lock(-1);
            if (ui_wifi_is_active()) ui_wifi_set_message(describeWiFiFailure(), true);
            lvgl_port_unlock();
        }
        if (now - netLastReconnect > RECONNECT_INTERVAL_MS) {
            netLastReconnect = now;
            WiFi.reconnect();
        }
        return;
    }

    if (netState != NET_ONLINE) {
        netState = NET_ONLINE;
        Serial.printf("[net] online, ip=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        if (netPendingSave) { netPendingSave = false; saveCredentials(); }
        fetchUsage();
        reqHaRefresh = true;
        lvgl_port_lock(-1);
        ui_show_page(UI_PAGE_USAGE);
        lvgl_port_unlock();
        lastPoll = millis();
    }
}

static void doScan()
{
    if (netState != NET_OFFLINE) {
        WiFi.disconnect();
        netState = NET_OFFLINE;
        delay(200);
    }
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    Serial.printf("[wifi] scan found %d networks\n", n);

    static ui_network_t nets[12];
    int count = 0;
    for (int i = 0; i < n && count < 12; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        bool dup = false;
        for (int k = 0; k < count; k++)
            if (strcmp(nets[k].ssid, ssid.c_str()) == 0) dup = true;
        if (dup) continue;
        snprintf(nets[count].ssid, sizeof(nets[count].ssid), "%s", ssid.c_str());
        nets[count].rssi = WiFi.RSSI(i);
        count++;
    }
    WiFi.scanDelete();

    lvgl_port_lock(-1);
    ui_wifi_set_networks(nets, count);
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ app --- */

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.printf("\n[boot] Claude usage widget v%s\n", FIRMWARE_VERSION);

    if (!boardInit()) {
        Serial.println("[boot] board init failed, halting");
        while (true) delay(1000);
    }

    memset(&usage, 0, sizeof(usage));
    memset(&haState, 0, sizeof(haState));

    ui_callbacks_t cb = {};
    cb.page_changed = cbPage;
    cb.thermo_delta = cbThermo;
    cb.light_set = cbLight;
    cb.theme_changed = cbTheme;
    cb.dim_changed = cbDim;
    cb.sleep_changed = cbSleep;
    cb.wifi_setup_requested = cbWifiSetup;
    cb.wifi_connect = cbWifiConnect;
    cb.wifi_rescan = cbRescan;
    cb.refresh_requested = cbRefresh;

    lvgl_port_lock(-1);
    ui_init(&cb);
    lvgl_port_unlock();

    loadSettings();

    WiFi.onEvent(onWiFiEvent);
    if (strlen(wifiSsid) == 0) {
        lvgl_port_lock(-1);
        ui_show_wifi_setup();
        lvgl_port_unlock();
        reqScan = true;
    } else {
        netBegin();
    }
    lastPoll = millis();
}

void loop()
{
    unsigned long now = millis();

    if (reqScan)          { reqScan = false; doScan(); }
    if (reqSaveSettings)  { reqSaveSettings = false; saveSettings(); }

    if (reqConnect) {
        reqConnect = false;
        snprintf(wifiSsid, sizeof(wifiSsid), "%s", pendingSsid);
        snprintf(wifiPass, sizeof(wifiPass), "%s", pendingPass);
        netPendingSave = true;
        netBegin();
    }

    serialCommands();
    netLoop();

    bool online = WiFi.status() == WL_CONNECTED;
    bool onSetup;
    lvgl_port_lock(-1);
    onSetup = ui_wifi_is_active();
    lvgl_port_unlock();

    if (reqRefresh) {
        reqRefresh = false;
        if (online) fetchUsage();
        lastPoll = now;
    }

    if (online && !onSetup && now - lastPoll >= POLL_INTERVAL_MS) {
        lastPoll = now;
        fetchUsage();
    }

    if (online && !onSetup) {
        char body[192];

        if (reqThermoDelta != 0 && haState.valid && haState.has_target) {
            int delta = reqThermoDelta;
            reqThermoDelta = 0;
            haState.target += (float)delta;
            snprintf(body, sizeof(body),
                     "{\"domain\":\"climate\",\"service\":\"set_temperature\","
                     "\"data\":{\"temperature\":%.1f}}", (double)haState.target);
            haCall(body);
        }

        if (reqLightIdx >= 0 && reqLightIdx < haState.light_count) {
            int idx = reqLightIdx;
            int on = reqLightOn, bri = reqLightBri;
            reqLightIdx = -1; reqLightOn = -1; reqLightBri = -1;
            if (bri >= 0)
                snprintf(body, sizeof(body),
                         "{\"domain\":\"light\",\"service\":\"turn_on\","
                         "\"data\":{\"index\":%d,\"brightness\":%d}}", idx, bri);
            else
                snprintf(body, sizeof(body),
                         "{\"domain\":\"light\",\"service\":\"%s\",\"data\":{\"index\":%d}}",
                         on ? "turn_on" : "turn_off", idx);
            haCall(body);
        }

        unsigned long haInterval =
            (ui_get_page() == UI_PAGE_HOME) ? HA_POLL_ACTIVE_MS : HA_POLL_IDLE_MS;
        if (reqHaRefresh || now - lastHaPoll >= haInterval) {
            reqHaRefresh = false;
            lastHaPoll = now;
            fetchHA();
        }
    }

    if (now - lastTick >= 1000) {
        lastTick = now;

        ui_status_t st;
        uint32_t age = fetchedAt ? (now - fetchedAt) / 1000UL : 0;
        if (!online)                 st = UI_STATUS_NO_WIFI;
        else if (!haveUsage)         st = UI_STATUS_BRIDGE_OFFLINE;
        else if (usage.blocked)      st = UI_STATUS_LIMIT_REACHED;
        else if (age > 90)           st = UI_STATUS_STALE;
        else                         st = UI_STATUS_CONNECTED;

        lvgl_port_lock(-1);
        ui_set_network_info(wifiSsid,
                            online ? WiFi.localIP().toString().c_str() : "",
                            BRIDGE_URL, FIRMWARE_VERSION);
        ui_set_status(st, age);
        ui_tick(now);
        uint32_t idle = lv_disp_get_inactive_time(NULL);
        lvgl_port_unlock();

        uint32_t limit = ui_sleep_ms[ui_get_sleep_index()];
        if (limit && !screenOff && idle > limit) {
            screenOff = true;
            screenOffAt = now;
            backlight(false);
            Serial.println("[power] screen off");
            logHealth("slept");
        } else if (screenOff && idle < 2000) {
            screenOff = false;
            backlight(true);
            Serial.printf("[power] screen on (off for %lus)\n",
                          screenOffAt ? (now - screenOffAt) / 1000UL : 0UL);
            logHealth("woke");
            /* The RGB DMA can drift out of sync while nobody is looking at the
             * panel. Resync on the way back up - a no-op when nothing is wrong. */
            displayKick("wake");
        }
    }

    delay(20);
}
