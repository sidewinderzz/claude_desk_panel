/*
 * Claude usage widget - Waveshare ESP32-S3-Touch-LCD-4.3B (800x480 RGB, GT911 touch)
 *
 * Polls claude_usage_bridge.py on the LAN and shows how much of the 5-hour session
 * window and the two 7-day windows have been used.
 *
 * Board settings (Arduino IDE):
 *   Board            ESP32S3 Dev Module
 *   Flash Size       16MB (128Mb)
 *   PSRAM            OPI PSRAM
 *   Partition Scheme 16M Flash (3MB APP/9.9MB FATFS)
 *   USB CDC On Boot  Enabled
 *
 * Tap the screen to refresh immediately.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>

#include "config.h"
#include "theme.h"

// ---------------------------------------------------------------------------
// Board wiring - Waveshare ESP32-S3-Touch-LCD-4.3B
// ---------------------------------------------------------------------------

#define LCD_DE 5
#define LCD_VSYNC 3
#define LCD_HSYNC 46
#define LCD_PCLK 7

// Red / green / blue data lines, least significant bit first.
#define LCD_R0 1
#define LCD_R1 2
#define LCD_R2 42
#define LCD_R3 41
#define LCD_R4 40
#define LCD_G0 39
#define LCD_G1 0
#define LCD_G2 45
#define LCD_G3 48
#define LCD_G4 47
#define LCD_G5 21
#define LCD_B0 14
#define LCD_B1 38
#define LCD_B2 18
#define LCD_B3 17
#define LCD_B4 10

#define I2C_SDA 8
#define I2C_SCL 9
#define TP_INT 4

// The 4.3B has almost no GPIO left, so a CH422G expander carries the reset and
// backlight lines. Each of its "registers" is a separate I2C address that takes a
// single data byte.
#define CH422G_ADDR_MODE 0x24
#define CH422G_ADDR_OUT 0x38
#define CH422G_MODE_OUTPUT 0x01

#define EXIO_TP_RST (1 << 1)
#define EXIO_LCD_BL (1 << 2)
#define EXIO_LCD_RST (1 << 3)
#define EXIO_SD_CS (1 << 4)

#define SCREEN_W 800
#define SCREEN_H 480

// ---------------------------------------------------------------------------
// Display / touch
// ---------------------------------------------------------------------------

Arduino_ESP32RGBPanel *panel = new Arduino_ESP32RGBPanel(
    LCD_DE, LCD_VSYNC, LCD_HSYNC, LCD_PCLK,
    LCD_R0, LCD_R1, LCD_R2, LCD_R3, LCD_R4,
    LCD_G0, LCD_G1, LCD_G2, LCD_G3, LCD_G4, LCD_G5,
    LCD_B0, LCD_B1, LCD_B2, LCD_B3, LCD_B4,
    0 /* hsync_polarity */, 16 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 16 /* hsync_back_porch */,
    0 /* vsync_polarity */, 16 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 16 /* vsync_back_porch */,
    // 16 MHz / width*10 bounce buffer. These are the values proven on this board
    // with this driver - do not lower the pixel clock to 12 MHz or double the
    // bounce buffer here. Those are the documented anti-drift settings for the
    // esp_panel + LVGL stack, but under Arduino_GFX the same combination produces
    // a blank white panel: the peripheral comes up and the backlight lights, but
    // no valid data ever reaches the glass. Verified on hardware 2026-09-05.
    1 /* pclk_active_neg */, 16000000 /* prefer_speed */, false /* useBigEndian */,
    0 /* de_idle_high */, 0 /* pclk_idle_high */, SCREEN_W * 10 /* bounce buffer px */);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    SCREEN_W, SCREEN_H, panel, 0 /* rotation */, true /* auto_flush */);

bool touchReady = false;

uint8_t expanderState = 0;

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

struct Gauge
{
  long used = 0;
  long limit = 0;
  float pct = 0.0f;
  long resetsIn = 0; // seconds, as of fetchedAt
  char source[16] = "";
};

struct Usage
{
  bool valid = false;
  Gauge session;
  Gauge week;
  Gauge opus;
  bool blocked = false;
  long blockedResetsIn = 0;
};

Usage usage;

enum Status
{
  ST_BOOT,
  ST_WIFI_CONNECTING,
  ST_WIFI_FAILED,
  ST_BRIDGE_UNREACHABLE,
  ST_OK
};

Status status = ST_BOOT;
char statusDetail[48] = "starting";

unsigned long lastPoll = 0;
unsigned long lastTick = 0;
unsigned long fetchedAt = 0;
unsigned long lastTouch = 0;
bool chromeDrawn = false;
// Set when a tap asks for the setup screen from inside a blocking wait.
bool setupRequested = false;

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

#define HEADER_H 66

#define GAUGE_CX 232
#define GAUGE_CY 268
#define GAUGE_R_OUT 136
#define GAUGE_R_IN 106
#define GAUGE_SWEEP_START 225.0f // 0 deg is 12 o'clock, clockwise; gap at the bottom
#define GAUGE_SWEEP_TOTAL 270.0f

#define CARD_X 448
#define CARD_W 324
#define CARD_H 170
#define CARD_A_Y 96
#define CARD_B_Y 286

// ---------------------------------------------------------------------------
// CH422G expander
// ---------------------------------------------------------------------------

static bool expanderWrite(uint8_t addr, uint8_t value)
{
  Wire.beginTransmission(addr);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static void expanderApply()
{
  expanderWrite(CH422G_ADDR_OUT, expanderState);
}

static void expanderSet(uint8_t bit, bool high)
{
  if (high)
    expanderState |= bit;
  else
    expanderState &= ~bit;
  expanderApply();
}

// Bring the panel and the touch controller out of reset. The GT911 samples its INT
// pin as the reset is released to choose its I2C address; holding INT low selects
// 0x5D, which is what the Waveshare board expects.
static void resetPeripherals()
{
  if (!expanderWrite(CH422G_ADDR_MODE, CH422G_MODE_OUTPUT))
    Serial.println("[exp] CH422G did not ACK - check the I2C bus");

  pinMode(TP_INT, OUTPUT);
  digitalWrite(TP_INT, LOW);

  // SD_CS stays high (deselected); everything else is pulled low to assert reset.
  expanderState = EXIO_SD_CS;
  expanderApply();
  delay(20);

  expanderState = EXIO_SD_CS | EXIO_LCD_RST | EXIO_TP_RST;
  expanderApply();
  delay(50);

  pinMode(TP_INT, INPUT);
  delay(50);
}

// ---------------------------------------------------------------------------
// GT911 touch
//
// Talked to directly rather than through an autodetecting library. The CH422G
// acknowledges every address in 0x20-0x3F, so probe-based detection reliably
// mis-identifies the expander as some other controller and then reads nonsense
// out of it. The GT911 sits at 0x5D (0x14 if its INT line was high at reset) and
// reports a "911" product id, which makes it unambiguous.
// ---------------------------------------------------------------------------

#define GT911_REG_PRODUCT_ID 0x8140
#define GT911_REG_STATUS 0x814E
#define GT911_REG_POINT1 0x8150

static uint8_t gt911Addr = 0x5D;

static bool gt911Read(uint16_t reg, uint8_t *buf, size_t len)
{
  Wire.beginTransmission(gt911Addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) // repeated start, no stop
    return false;
  if (Wire.requestFrom((uint8_t)gt911Addr, (uint8_t)len) != len)
    return false;
  for (size_t i = 0; i < len; i++)
    buf[i] = Wire.read();
  return true;
}

static bool gt911Write(uint16_t reg, uint8_t value)
{
  Wire.beginTransmission(gt911Addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool gt911Init()
{
  const uint8_t candidates[] = {0x5D, 0x14};
  for (uint8_t i = 0; i < sizeof(candidates); i++)
  {
    gt911Addr = candidates[i];
    uint8_t id[4] = {0, 0, 0, 0};
    if (gt911Read(GT911_REG_PRODUCT_ID, id, 4) &&
        id[0] == '9' && id[1] == '1' && id[2] == '1')
    {
      Serial.printf("[touch] GT911 at 0x%02X\n", gt911Addr);
      return true;
    }
  }
  Serial.println("[touch] no GT911 found at 0x5D or 0x14");
  return false;
}

// Number of active touch points; x/y hold the first one. The status flag has to be
// cleared after each read or the controller stops refreshing it.
static int gt911GetTouch(int16_t &x, int16_t &y)
{
  uint8_t status = 0;
  if (!gt911Read(GT911_REG_STATUS, &status, 1))
    return 0;
  if (!(status & 0x80))
    return 0;

  int count = status & 0x0F;
  if (count > 0)
  {
    uint8_t p[8];
    if (gt911Read(GT911_REG_POINT1, p, 8))
    {
      x = (int16_t)(p[1] | ((uint16_t)p[2] << 8));
      y = (int16_t)(p[3] | ((uint16_t)p[4] << 8));
    }
    else
    {
      count = 0;
    }
  }
  gt911Write(GT911_REG_STATUS, 0);
  return count;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

// Filled ring segment. Angles are degrees with 0 at 12 o'clock, increasing
// clockwise, which keeps the call sites readable regardless of what convention the
// graphics library happens to use internally.
static void ringSegment(int16_t cx, int16_t cy, int16_t rIn, int16_t rOut,
                        float startDeg, float endDeg, uint16_t color)
{
  if (endDeg <= startDeg)
    return;
  // 0.4 deg keeps successive spokes under a pixel apart at this radius, so the
  // band comes out solid.
  const float step = 0.4f;
  gfx->startWrite();
  for (float a = startDeg; a <= endDeg; a += step)
  {
    float rad = (a - 90.0f) * DEG_TO_RAD;
    float c = cosf(rad);
    float s = sinf(rad);
    for (int16_t r = rIn; r <= rOut; r++)
      gfx->writePixel((int16_t)lroundf(cx + c * r), (int16_t)lroundf(cy + s * r), color);
  }
  gfx->endWrite();
}

static void textAt(const uint8_t *font, int16_t x, int16_t baseline, uint16_t color, const char *s)
{
  gfx->setFont(font);
  gfx->setTextColor(color);
  gfx->setCursor(x, baseline);
  gfx->print(s);
}

static uint16_t textWidth(const uint8_t *font, const char *s)
{
  int16_t x1, y1;
  uint16_t w, h;
  gfx->setFont(font);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

static void textCentered(const uint8_t *font, int16_t cx, int16_t baseline, uint16_t color, const char *s)
{
  textAt(font, cx - textWidth(font, s) / 2, baseline, color, s);
}

static void clearRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
  gfx->fillRect(x, y, w, h, color);
}

// A small radiating mark, in the spirit of the Claude wordmark's burst.
static void drawBurst(int16_t cx, int16_t cy, int16_t r, uint16_t color)
{
  const int spokes = 12;
  gfx->startWrite();
  for (int i = 0; i < spokes; i++)
  {
    float rad = (i * (360.0f / spokes)) * DEG_TO_RAD;
    float c = cosf(rad);
    float s = sinf(rad);
    int16_t inner = r / 3;
    // Two offset spokes give each ray a little weight without needing polygons.
    for (int16_t d = -1; d <= 1; d++)
    {
      gfx->writeLine((int16_t)lroundf(cx + c * inner) + d, (int16_t)lroundf(cy + s * inner),
                     (int16_t)lroundf(cx + c * r) + d, (int16_t)lroundf(cy + s * r), color);
    }
  }
  gfx->endWrite();
}

// Needs gfx, touch and the text helpers above, so it is included here rather than
// at the top of the file.
#include "wifi_setup.h"

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

static void formatTokens(long v, char *out, size_t n)
{
  if (v >= 1000000000L)
    snprintf(out, n, "%.1fB", v / 1000000000.0);
  else if (v >= 1000000L)
    snprintf(out, n, "%.1fM", v / 1000000.0);
  else if (v >= 1000L)
    snprintf(out, n, "%.0fK", v / 1000.0);
  else
    snprintf(out, n, "%ld", v);
}

static void formatDuration(long seconds, char *out, size_t n)
{
  if (seconds <= 0)
  {
    snprintf(out, n, "now");
    return;
  }
  long h = seconds / 3600;
  long m = (seconds % 3600) / 60;
  if (h > 0)
    snprintf(out, n, "%ldh %ldm", h, m);
  else if (m > 0)
    snprintf(out, n, "%ldm", m);
  else
    snprintf(out, n, "<1m");
}

// Seconds remaining right now, counting down locally between polls.
static long liveResetsIn(long atFetch)
{
  if (atFetch <= 0)
    return 0;
  long elapsed = (long)((millis() - fetchedAt) / 1000UL);
  long remaining = atFetch - elapsed;
  return remaining > 0 ? remaining : 0;
}

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------

static void drawChrome()
{
  gfx->fillScreen(C_BG);

  clearRect(0, 0, SCREEN_W, HEADER_H, C_SURFACE);
  gfx->drawFastHLine(0, HEADER_H, SCREEN_W, C_LINE);
  drawBurst(36, 33, 15, C_ACCENT);
  textAt(u8g2_font_helvB18_tf, 68, 41, C_BONE, "Claude Usage");

  // Two cards on the right.
  gfx->fillRoundRect(CARD_X, CARD_A_Y, CARD_W, CARD_H, 12, C_SURFACE);
  gfx->drawRoundRect(CARD_X, CARD_A_Y, CARD_W, CARD_H, 12, C_LINE);
  textAt(u8g2_font_helvR12_tf, CARD_X + 22, CARD_A_Y + 32, C_MUTED, "7-DAY  ALL MODELS");

  gfx->fillRoundRect(CARD_X, CARD_B_Y, CARD_W, CARD_H, 12, C_SURFACE);
  gfx->drawRoundRect(CARD_X, CARD_B_Y, CARD_W, CARD_H, 12, C_LINE);
  textAt(u8g2_font_helvR12_tf, CARD_X + 22, CARD_B_Y + 32, C_MUTED, "7-DAY  OPUS");

  // Above the ring: the ring's own bottom edge reaches y = GAUGE_CY + GAUGE_R_OUT,
  // so anything drawn below it has to clear that line (see drawGaugeCaption).
  textCentered(u8g2_font_helvR12_tf, GAUGE_CX, 100, C_MUTED, "SESSION  5-HOUR WINDOW");

  chromeDrawn = true;
}

static void drawGauge()
{
  float pct = usage.valid ? usage.session.pct : 0.0f;
  if (pct > 100.0f)
    pct = 100.0f;
  uint16_t color = usage.blocked ? C_HOT : severityColor(pct);

  ringSegment(GAUGE_CX, GAUGE_CY, GAUGE_R_IN, GAUGE_R_OUT,
              GAUGE_SWEEP_START, GAUGE_SWEEP_START + GAUGE_SWEEP_TOTAL, C_LINE);
  if (pct > 0.0f)
    ringSegment(GAUGE_CX, GAUGE_CY, GAUGE_R_IN, GAUGE_R_OUT,
                GAUGE_SWEEP_START, GAUGE_SWEEP_START + GAUGE_SWEEP_TOTAL * (pct / 100.0f), color);

  // Centre readout. The clear rect must fit inside the ring's inner circle or its
  // corners eat into the ring: 90^2 + 50^2 = 103^2, comfortably under GAUGE_R_IN.
  clearRect(GAUGE_CX - 90, GAUGE_CY - 50, 180, 100, C_BG);

  if (!usage.valid)
  {
    textCentered(u8g2_font_helvB18_tf, GAUGE_CX, GAUGE_CY + 8, C_MUTED, "--");
    return;
  }

  char num[8];
  snprintf(num, sizeof(num), "%d", (int)lroundf(usage.session.pct));
  uint16_t wNum = textWidth(u8g2_font_logisoso58_tn, num);
  uint16_t wPct = textWidth(u8g2_font_helvB24_tf, "%");
  int16_t startX = GAUGE_CX - (wNum + 6 + wPct) / 2;

  textAt(u8g2_font_logisoso58_tn, startX, GAUGE_CY + 22, C_BONE, num);
  textAt(u8g2_font_helvB24_tf, startX + wNum + 6, GAUGE_CY + 22, color, "%");
}

// The caption sits clear of the ring rather than inside its bottom gap: the gap is a
// wedge, not a rectangle, so a rectangular clear at that height would bite into the
// ring's lower-left and lower-right arms every time the countdown ticked.
#define CAPTION_TOP (GAUGE_CY + GAUGE_R_OUT + 6)
#define CAPTION_BASELINE (CAPTION_TOP + 22)

static void drawGaugeCaption()
{
  clearRect(GAUGE_CX - 160, CAPTION_TOP, 320, 34, C_BG);
  if (!usage.valid)
    return;

  char buf[48];
  char d[24];
  if (usage.blocked)
  {
    formatDuration(liveResetsIn(usage.blockedResetsIn), d, sizeof(d));
    snprintf(buf, sizeof(buf), "LIMIT REACHED  %s", d);
    textCentered(u8g2_font_helvB14_tf, GAUGE_CX, CAPTION_BASELINE, C_HOT, buf);
  }
  else
  {
    formatDuration(liveResetsIn(usage.session.resetsIn), d, sizeof(d));
    snprintf(buf, sizeof(buf), "resets in %s", d);
    textCentered(u8g2_font_helvR14_tf, GAUGE_CX, CAPTION_BASELINE, C_MUTED, buf);
  }
}

static void drawCard(int16_t y, const Gauge &g)
{
  // Everything below the card's static label gets repainted. Stop short of the bottom
  // corners so the rounded outline isn't squared off by the fill.
  clearRect(CARD_X + 1, y + 44, CARD_W - 2, CARD_H - 58, C_SURFACE);

  if (!usage.valid)
  {
    textAt(u8g2_font_helvB24_tf, CARD_X + 22, y + 84, C_MUTED, "--");
    return;
  }

  float pct = g.pct > 100.0f ? 100.0f : g.pct;
  uint16_t color = severityColor(g.pct);

  char buf[64];
  snprintf(buf, sizeof(buf), "%d%%", (int)lroundf(g.pct));
  textAt(u8g2_font_helvB24_tf, CARD_X + 22, y + 84, C_BONE, buf);

  const int16_t barX = CARD_X + 22;
  const int16_t barY = y + 102;
  const int16_t barW = CARD_W - 44;
  const int16_t barH = 14;
  gfx->fillRoundRect(barX, barY, barW, barH, 7, C_LINE);
  int16_t fill = (int16_t)lroundf(barW * (pct / 100.0f));
  if (fill >= 3)
    gfx->fillRoundRect(barX, barY, fill, barH, 7, color);

  char used[16], limit[16];
  formatTokens(g.used, used, sizeof(used));
  formatTokens(g.limit, limit, sizeof(limit));
  snprintf(buf, sizeof(buf), "%s / %s  %s", used, limit, g.source);
  textAt(u8g2_font_helvR10_tf, barX, y + 140, C_MUTED, buf);
}

static void drawStatusBar()
{
  // Right side of the header: a state dot plus a short label.
  clearRect(430, 6, SCREEN_W - 436, HEADER_H - 12, C_SURFACE);

  uint16_t dot;
  const char *label;
  switch (status)
  {
  case ST_OK:
    dot = usage.blocked ? C_HOT : C_GOOD;
    label = usage.blocked ? "limit reached" : "connected";
    break;
  case ST_WIFI_CONNECTING:
    dot = C_WARN;
    label = "joining wi-fi";
    break;
  case ST_WIFI_FAILED:
    dot = C_HOT;
    label = "no wi-fi";
    break;
  case ST_BRIDGE_UNREACHABLE:
    dot = C_HOT;
    label = "bridge offline";
    break;
  default:
    dot = C_MUTED;
    label = "starting";
    break;
  }

  uint16_t w = textWidth(u8g2_font_helvR12_tf, label);
  textAt(u8g2_font_helvR12_tf, SCREEN_W - 34 - w, 40, C_MUTED, label);
  gfx->fillCircle(SCREEN_W - 24, 34, 6, dot);
}

static void drawFooter()
{
  clearRect(20, 458, SCREEN_W - 40, 20, C_BG);

  char buf[96];
  if (status == ST_OK && fetchedAt > 0)
  {
    unsigned long age = (millis() - fetchedAt) / 1000UL;
    snprintf(buf, sizeof(buf), "%s   updated %lus ago   tap to refresh, top bar for wi-fi",
             WiFi.localIP().toString().c_str(), age);
  }
  else
  {
    snprintf(buf, sizeof(buf), "%s", statusDetail);
  }
  textAt(u8g2_font_helvR10_tf, 24, 472, C_MUTED, buf);
}

static void drawAll()
{
  if (!chromeDrawn)
    drawChrome();
  drawGauge();
  drawGaugeCaption();
  drawCard(CARD_A_Y, usage.week);
  drawCard(CARD_B_Y, usage.opus);
  drawStatusBar();
  drawFooter();
}

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------

static void readGauge(JsonObjectConst src, Gauge &dst)
{
  dst.used = src["used"] | 0L;
  dst.limit = src["limit"] | 0L;
  dst.pct = src["pct"] | 0.0f;
  dst.resetsIn = src["resets_in"] | 0L;
  strlcpy(dst.source, src["source"] | "", sizeof(dst.source));
}

static bool fetchUsage()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    status = ST_WIFI_FAILED;
    snprintf(statusDetail, sizeof(statusDetail), "wi-fi disconnected");
    return false;
  }

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(BRIDGE_URL))
  {
    status = ST_BRIDGE_UNREACHABLE;
    snprintf(statusDetail, sizeof(statusDetail), "bad BRIDGE_URL");
    return false;
  }

  int code = http.GET();
  if (code != 200)
  {
    Serial.printf("[http] GET %s -> %d\n", BRIDGE_URL, code);
    http.end();
    status = ST_BRIDGE_UNREACHABLE;
    snprintf(statusDetail, sizeof(statusDetail), "bridge unreachable (%d) - is it running?", code);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err)
  {
    Serial.printf("[json] %s\n", err.c_str());
    status = ST_BRIDGE_UNREACHABLE;
    snprintf(statusDetail, sizeof(statusDetail), "bad response: %s", err.c_str());
    return false;
  }

  if (!(doc["ok"] | false))
  {
    const char *msg = doc["error"] | "bridge reported an error";
    status = ST_BRIDGE_UNREACHABLE;
    snprintf(statusDetail, sizeof(statusDetail), "%s", msg);
    return false;
  }

  readGauge(doc["session"], usage.session);
  readGauge(doc["week"], usage.week);
  readGauge(doc["opus"], usage.opus);
  usage.blocked = doc["blocked"] | false;
  usage.blockedResetsIn = doc["blocked_resets_in"] | 0L;
  usage.valid = true;

  fetchedAt = millis();
  status = ST_OK;
  Serial.printf("[usage] session %.1f%%  week %.1f%%  opus %.1f%%%s\n",
                usage.session.pct, usage.week.pct, usage.opus.pct,
                usage.blocked ? "  [BLOCKED]" : "");
  return true;
}

static void connectWiFi()
{
  status = ST_WIFI_CONNECTING;
  snprintf(statusDetail, sizeof(statusDetail), "joining %s", wifiSsid);
  drawStatusBar();
  drawFooter();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  // Tear down any half-finished association first, or the next begin() is refused
  // with "sta is connecting, cannot set config".
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(wifiSsid, wifiPass);

  Serial.printf("[wifi] joining %s\n", wifiSsid);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_MS)
  {
    // Stay responsive while we wait: a tap on the top bar aborts straight into
    // setup rather than leaving the panel dead for the whole attempt.
    if (touchReady)
    {
      int16_t tx = 0, ty = 0;
      if (gt911GetTouch(tx, ty) > 0 && tx < SCREEN_W && ty < HEADER_H)
      {
        setupRequested = true;
        Serial.println("\n[wifi] attempt aborted - opening setup");
        break;
      }
    }
    delay(100);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.printf("[wifi] connected, ip=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    snprintf(statusDetail, sizeof(statusDetail), "%s", WiFi.localIP().toString().c_str());
  }
  else
  {
    Serial.println("[wifi] failed - tap the top bar to open wi-fi setup (2.4GHz only)");
    status = ST_WIFI_FAILED;
    snprintf(statusDetail, sizeof(statusDetail), "wi-fi failed - tap the top bar to set it up");
  }
}

// Rate-limited reconnect, so a down network doesn't retry in a tight loop.
static unsigned long lastWifiAttempt = 0;

static bool ensureWiFi(bool force)
{
  if (WiFi.status() == WL_CONNECTED)
    return true;
  unsigned long now = millis();
  if (!force && lastWifiAttempt != 0 && now - lastWifiAttempt < WIFI_RETRY_MS)
    return false;
  lastWifiAttempt = now;
  connectWiFi();
  return WiFi.status() == WL_CONNECTED;
}

// Run the on-screen setup, then persist and reconnect if the user confirmed.
static void runWiFiSetup()
{
  bool confirmed = wifiSetupModal();
  chromeDrawn = false; // the modal painted over everything
  if (confirmed)
  {
    wifiSaveCredentials();
    lastWifiAttempt = 0;
    drawChrome();
    ensureWiFi(true);
    if (WiFi.status() == WL_CONNECTED)
      fetchUsage();
    lastPoll = millis();
  }
  drawAll();
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

static void scanI2C()
{
  Serial.print("[i2c] devices:");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++)
  {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0)
    {
      Serial.printf(" 0x%02X", a);
      found++;
    }
  }
  if (!found)
    Serial.print(" none");
  Serial.println();
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[boot] Claude usage widget");

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  scanI2C();
  resetPeripherals();

  if (!gfx->begin())
  {
    Serial.println("[lcd] gfx->begin() failed");
    while (true)
      delay(1000);
  }
  gfx->fillScreen(C_BG);
  expanderSet(EXIO_LCD_BL, true); // backlight on once there is something to show

  touchReady = gt911Init();
  if (!touchReady)
    Serial.println("[touch] not detected - display still works, tap-to-refresh won't");

  drawChrome();
  drawAll();

  wifiLoadCredentials();

  // A board that has never been set up goes straight to the keyboard. Attempting a
  // doomed association first just makes the user wait for a failure we can predict.
  if (touchReady && !wifiHasStoredCredentials())
  {
    Serial.println("[wifi] never configured - opening setup");
    runWiFiSetup();
    return;
  }

  ensureWiFi(true);
  if (WiFi.status() != WL_CONNECTED && touchReady)
  {
    runWiFiSetup();
    return;
  }
  if (WiFi.status() == WL_CONNECTED)
    fetchUsage();
  lastPoll = millis();
  drawAll();
}

void loop()
{
  unsigned long now = millis();

  if (setupRequested)
  {
    setupRequested = false;
    runWiFiSetup();
    lastTouch = millis();
    return;
  }

  // Tap anywhere to refresh, debounced.
  if (touchReady && now - lastTouch > 600)
  {
    int16_t tx = 0, ty = 0;
    if (gt911GetTouch(tx, ty) > 0 && tx < SCREEN_W && ty < SCREEN_H)
    {
      lastTouch = now;
      // Top bar opens wi-fi setup; anywhere else is a refresh.
      if (ty < HEADER_H)
      {
        Serial.println("[touch] header - opening wi-fi setup");
        runWiFiSetup();
        lastTouch = millis();
      }
      else
      {
        Serial.printf("[touch] %d,%d - refreshing\n", tx, ty);
        ensureWiFi(true);
        fetchUsage();
        lastPoll = now;
        drawAll();
      }
    }
  }

  if (now - lastPoll >= POLL_INTERVAL_MS)
  {
    lastPoll = now;
    ensureWiFi(false);
    fetchUsage();
    drawAll();
  }

  // Tick the countdown and the "updated Ns ago" line once a second without
  // repainting anything else.
  if (now - lastTick >= 1000)
  {
    lastTick = now;
    drawGaugeCaption();
    drawFooter();
  }

  delay(20);
}
