#pragma once

// On-screen Wi-Fi setup: a network picker plus a touch keyboard, so credentials can
// be entered on the device instead of being compiled in. Included from the main
// sketch after gfx, touch and the drawing helpers exist.
//
// Credentials live in NVS under the "claudewgt" namespace. config.h is only ever a
// fallback for a device that has never been set up.

#include <Preferences.h>

#define WIFI_SSID_MAX 33
#define WIFI_PASS_MAX 65

static Preferences wifiPrefs;

char wifiSsid[WIFI_SSID_MAX] = "";
char wifiPass[WIFI_PASS_MAX] = "";

// --- modal geometry --------------------------------------------------------

#define MD_X 60
#define MD_Y 40
#define MD_W 680
#define MD_H 400

#define FLD_X 84
#define FLD_W 632
#define FLD_H 40
#define SSID_Y 116
#define PASS_Y 186
#define SHOW_W 84

#define KB_X 78
#define KEY_W 59
#define KEY_H 34
#define KEY_GX 6
#define KB_R0 240
#define KEY_GY 5
#define KB_ROW(n) (KB_R0 + (n) * (KEY_H + KEY_GY))

// Key actions. Anything >= 0 is a character index into the current layer.
enum KeyAction
{
  KA_CHAR = 0,
  KA_SHIFT,
  KA_SYM,
  KA_DEL,
  KA_SPACE,
  KA_CANCEL,
  KA_CONNECT
};

struct Key
{
  int16_t x, y, w, h;
  const char *label;
  KeyAction action;
  char ch;
};

static Key kbKeys[64];
static int kbCount = 0;
static bool kbShift = false;
static bool kbSym = false;
static int activeField = 1; // 0 = ssid, 1 = password
static bool showPass = false;

static const char *ROW0 = "1234567890";
static const char *ROW1_ABC = "qwertyuiop";
static const char *ROW2_ABC = "asdfghjkl";
static const char *ROW3_ABC = "zxcvbnm";
static const char *ROW1_SYM = "!@#$%^&*()";
static const char *ROW2_SYM = "-_=+[]{};";
static const char *ROW3_SYM = ":'\",./";

static void kbAdd(int16_t x, int16_t y, int16_t w, int16_t h, const char *label, KeyAction a, char ch)
{
  if (kbCount >= (int)(sizeof(kbKeys) / sizeof(kbKeys[0])))
    return;
  kbKeys[kbCount++] = {x, y, w, h, label, a, ch};
}

static void kbAddRow(const char *chars, int16_t y, int16_t startX)
{
  static char labels[64][2]; // stable storage for single-character labels
  for (int i = 0; chars[i]; i++)
  {
    char c = chars[i];
    if (kbShift && !kbSym && c >= 'a' && c <= 'z')
      c = c - 'a' + 'A';
    int slot = kbCount % 64;
    labels[slot][0] = c;
    labels[slot][1] = 0;
    kbAdd(startX + i * (KEY_W + KEY_GX), y, KEY_W, KEY_H, labels[slot], KA_CHAR, c);
  }
}

static void kbBuild()
{
  kbCount = 0;
  const char *r1 = kbSym ? ROW1_SYM : ROW1_ABC;
  const char *r2 = kbSym ? ROW2_SYM : ROW2_ABC;
  const char *r3 = kbSym ? ROW3_SYM : ROW3_ABC;

  kbAddRow(ROW0, KB_ROW(0), KB_X);
  kbAddRow(r1, KB_ROW(1), KB_X);
  kbAddRow(r2, KB_ROW(2), KB_X + 32);

  int16_t y3 = KB_ROW(3);
  kbAdd(KB_X, y3, 92, KEY_H, kbShift ? "SHIFT" : "shift", KA_SHIFT, 0);
  kbAddRow(r3, y3, KB_X + 92 + KEY_GX);
  int n3 = (int)strlen(r3);
  kbAdd(KB_X + 92 + KEY_GX + n3 * (KEY_W + KEY_GX), y3, 92, KEY_H, "del", KA_DEL, 0);

  int16_t y4 = KB_ROW(4);
  kbAdd(KB_X, y4, 92, KEY_H, kbSym ? "abc" : "?123", KA_SYM, 0);
  kbAdd(KB_X + 98, y4, 274, KEY_H, "space", KA_SPACE, ' ');
  kbAdd(KB_X + 378, y4, 120, KEY_H, "cancel", KA_CANCEL, 0);
  kbAdd(KB_X + 504, y4, 140, KEY_H, "connect", KA_CONNECT, 0);
}

// --- touch ------------------------------------------------------------------

// Blocks until a finger lands and lifts, so one press yields exactly one tap.
// timeoutMs == 0 waits indefinitely - used for the setup screen, which must stay up
// until the user actually finishes rather than vanishing out from under them.
static bool waitTap(int16_t &tx, int16_t &ty, uint32_t timeoutMs)
{
  if (!touchReady)
  {
    delay(timeoutMs ? timeoutMs : 1000);
    return false;
  }
  unsigned long deadline = millis() + timeoutMs;
  while (timeoutMs == 0 || millis() < deadline)
  {
    int16_t x = 0, y = 0;
    if (gt911GetTouch(x, y) > 0)
    {
      // Log before filtering, so an unexpected coordinate space shows up in the
      // trace instead of being silently discarded.
      Serial.printf("[raw] %d,%d\n", x, y);
      if (x < SCREEN_W && y < SCREEN_H)
      {
        tx = x;
        ty = y;
        int16_t dx, dy;
        unsigned long quiet = millis();
        while (millis() - quiet < 120)
        {
          if (gt911GetTouch(dx, dy) > 0)
            quiet = millis();
          delay(10);
        }
        return true;
      }
    }
    delay(15);
  }
  return false;
}

static bool hit(int16_t x, int16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh)
{
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// --- drawing ----------------------------------------------------------------

static void drawField(int16_t y, const char *label, const char *value, bool active, bool mask)
{
  int16_t w = (y == PASS_Y) ? (FLD_W - SHOW_W - 8) : FLD_W;
  textAt(u8g2_font_helvR10_tf, FLD_X, y - 8, C_MUTED, label);
  gfx->fillRoundRect(FLD_X, y, w, FLD_H, 8, C_BG);
  gfx->drawRoundRect(FLD_X, y, w, FLD_H, 8, active ? C_ACCENT : C_LINE);

  char shown[WIFI_PASS_MAX];
  if (mask && !showPass)
  {
    size_t n = strlen(value);
    if (n > WIFI_PASS_MAX - 1)
      n = WIFI_PASS_MAX - 1;
    memset(shown, '*', n);
    shown[n] = 0;
  }
  else
  {
    strlcpy(shown, value, sizeof(shown));
  }
  textAt(u8g2_font_helvR14_tf, FLD_X + 12, y + 27, C_BONE, shown);

  if (y == PASS_Y)
  {
    int16_t bx = FLD_X + w + 8;
    gfx->fillRoundRect(bx, y, SHOW_W, FLD_H, 8, C_SURFACE);
    gfx->drawRoundRect(bx, y, SHOW_W, FLD_H, 8, C_LINE);
    textCentered(u8g2_font_helvR12_tf, bx + SHOW_W / 2, y + 26, C_MUTED, showPass ? "hide" : "show");
  }
}

static void drawKeyboard()
{
  for (int i = 0; i < kbCount; i++)
  {
    const Key &k = kbKeys[i];
    uint16_t bg = C_SURFACE;
    uint16_t fg = C_BONE;
    if (k.action == KA_CONNECT)
    {
      bg = C_ACCENT;
      fg = C_BG;
    }
    else if (k.action == KA_SHIFT && kbShift)
      bg = C_LINE;
    else if (k.action == KA_SYM && kbSym)
      bg = C_LINE;
    else if (k.action != KA_CHAR && k.action != KA_SPACE)
      fg = C_MUTED;

    gfx->fillRoundRect(k.x, k.y, k.w, k.h, 6, bg);
    gfx->drawRoundRect(k.x, k.y, k.w, k.h, 6, C_LINE);
    const uint8_t *f = (k.action == KA_CHAR) ? u8g2_font_helvB14_tf : u8g2_font_helvR12_tf;
    textCentered(f, k.x + k.w / 2, k.y + k.h / 2 + 6, fg, k.label);
  }
}

static void drawModalFrame(const char *title)
{
  gfx->fillRoundRect(MD_X, MD_Y, MD_W, MD_H, 14, C_SURFACE);
  gfx->drawRoundRect(MD_X, MD_Y, MD_W, MD_H, 14, C_LINE);
  gfx->fillRect(MD_X + 1, MD_Y + 1, MD_W - 2, 56, C_SURFACE);
  textAt(u8g2_font_helvB18_tf, FLD_X, MD_Y + 38, C_BONE, title);
  gfx->drawFastHLine(MD_X + 1, MD_Y + 56, MD_W - 2, C_LINE);
}

static void drawSetupScreen()
{
  gfx->fillScreen(C_BG);
  drawModalFrame("Wi-Fi setup");
  drawField(SSID_Y, "NETWORK  (tap to scan)", wifiSsid, activeField == 0, false);
  drawField(PASS_Y, "PASSWORD", wifiPass, activeField == 1, true);
  drawKeyboard();
}

// --- network picker ---------------------------------------------------------

// Returns true if the user chose a network.
static bool pickNetwork()
{
  gfx->fillScreen(C_BG);
  drawModalFrame("Choose a network");
  textCentered(u8g2_font_helvR14_tf, SCREEN_W / 2, 250, C_MUTED, "scanning...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  int n = WiFi.scanNetworks();

  gfx->fillScreen(C_BG);
  drawModalFrame("Choose a network");

  const int16_t rowH = 40;
  const int16_t listY = 110;
  const int maxRows = 7;
  if (n <= 0)
    textAt(u8g2_font_helvR14_tf, FLD_X, listY + 26, C_MUTED, "no networks found");

  int rows = n < maxRows ? n : maxRows;
  for (int i = 0; i < rows; i++)
  {
    int16_t y = listY + i * rowH;
    gfx->drawFastHLine(FLD_X, y + rowH - 1, FLD_W, C_LINE);
    textAt(u8g2_font_helvR14_tf, FLD_X + 8, y + 26, C_BONE, WiFi.SSID(i).c_str());
    char rs[24];
    snprintf(rs, sizeof(rs), "%d dBm%s", WiFi.RSSI(i),
             WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "  open" : "");
    uint16_t w = textWidth(u8g2_font_helvR10_tf, rs);
    textAt(u8g2_font_helvR10_tf, FLD_X + FLD_W - 8 - w, y + 26, C_MUTED, rs);
  }

  int16_t backY = MD_Y + MD_H - 56;
  gfx->fillRoundRect(FLD_X, backY, 140, 40, 8, C_SURFACE);
  gfx->drawRoundRect(FLD_X, backY, 140, 40, 8, C_LINE);
  textCentered(u8g2_font_helvR12_tf, FLD_X + 70, backY + 26, C_MUTED, "back");

  while (true)
  {
    int16_t tx, ty;
    if (!waitTap(tx, ty, 300000))
      return false;
    if (hit(tx, ty, FLD_X, backY, 140, 40))
      return false;
    for (int i = 0; i < rows; i++)
    {
      if (hit(tx, ty, FLD_X, listY + i * rowH, FLD_W, rowH))
      {
        strlcpy(wifiSsid, WiFi.SSID(i).c_str(), sizeof(wifiSsid));
        WiFi.scanDelete();
        return true;
      }
    }
  }
}

// --- credentials ------------------------------------------------------------

// True once the user has completed setup on the device at least once.
static bool wifiHasStoredCredentials()
{
  wifiPrefs.begin("claudewgt", true);
  String s = wifiPrefs.getString("ssid", "");
  wifiPrefs.end();
  return s.length() > 0;
}

static void wifiLoadCredentials()
{
  wifiPrefs.begin("claudewgt", true);
  String s = wifiPrefs.getString("ssid", "");
  String p = wifiPrefs.getString("pass", "");
  wifiPrefs.end();

  if (s.length())
  {
    strlcpy(wifiSsid, s.c_str(), sizeof(wifiSsid));
    strlcpy(wifiPass, p.c_str(), sizeof(wifiPass));
    Serial.printf("[wifi] using stored credentials for %s\n", wifiSsid);
  }
  else
  {
    strlcpy(wifiSsid, WIFI_SSID, sizeof(wifiSsid));
    strlcpy(wifiPass, WIFI_PASSWORD, sizeof(wifiPass));
    // Don't carry the shipped placeholder forward as if it were a real password.
    if (strstr(wifiPass, "PUT_YOUR_WIFI_PASSWORD") != NULL)
      wifiPass[0] = 0;
    Serial.println("[wifi] no stored credentials, using config.h");
  }
}

static void wifiSaveCredentials()
{
  wifiPrefs.begin("claudewgt", false);
  wifiPrefs.putString("ssid", wifiSsid);
  wifiPrefs.putString("pass", wifiPass);
  wifiPrefs.end();
  Serial.printf("[wifi] saved credentials for %s\n", wifiSsid);
}

// --- the modal ---------------------------------------------------------------

// Blocks until the user hits connect or cancel. Returns true on connect.
static bool wifiSetupModal()
{
  if (!touchReady)
  {
    Serial.println("[wifi] setup needs touch, which did not initialise");
    return false;
  }

  kbShift = false;
  kbSym = false;
  showPass = false;
  activeField = 1;
  // Always start the password empty. A stored one can't be usefully edited, and a
  // config.h placeholder would otherwise sit in the field looking like a real value.
  wifiPass[0] = 0;
  kbBuild();
  drawSetupScreen();

  while (true)
  {
    int16_t tx, ty;
    if (!waitTap(tx, ty, 0)) // stays up until cancel or connect
      return false;
    Serial.printf("[tap] %d,%d\n", tx, ty);

    if (hit(tx, ty, FLD_X, SSID_Y, FLD_W, FLD_H))
    {
      if (pickNetwork())
        activeField = 1;
      kbBuild();
      drawSetupScreen();
      continue;
    }
    if (hit(tx, ty, FLD_X + FLD_W - SHOW_W, PASS_Y, SHOW_W, FLD_H))
    {
      showPass = !showPass;
      drawField(PASS_Y, "PASSWORD", wifiPass, activeField == 1, true);
      continue;
    }
    if (hit(tx, ty, FLD_X, PASS_Y, FLD_W - SHOW_W - 8, FLD_H))
    {
      activeField = 1;
      drawField(SSID_Y, "NETWORK  (tap to scan)", wifiSsid, false, false);
      drawField(PASS_Y, "PASSWORD", wifiPass, true, true);
      continue;
    }

    for (int i = 0; i < kbCount; i++)
    {
      // By value, not by reference: kbBuild() below rewrites kbKeys, and a reference
      // into it would start reporting whatever key later lands at this index.
      const Key k = kbKeys[i];
      if (!hit(tx, ty, k.x, k.y, k.w, k.h))
        continue;
      Serial.printf("[key] %s\n", k.label);

      char *target = (activeField == 0) ? wifiSsid : wifiPass;
      size_t cap = (activeField == 0) ? WIFI_SSID_MAX : WIFI_PASS_MAX;

      switch (k.action)
      {
      case KA_CANCEL:
        return false;
      case KA_CONNECT:
        return true;
      case KA_SHIFT:
        kbShift = !kbShift;
        kbBuild();
        drawKeyboard();
        break;
      case KA_SYM:
        kbSym = !kbSym;
        kbShift = false;
        kbBuild();
        drawKeyboard();
        break;
      case KA_DEL:
      {
        size_t n = strlen(target);
        if (n)
          target[n - 1] = 0;
        break;
      }
      case KA_SPACE:
      case KA_CHAR:
      {
        size_t n = strlen(target);
        if (n + 1 < cap)
        {
          target[n] = k.ch;
          target[n + 1] = 0;
        }
        // One-shot shift, the way a phone keyboard behaves.
        if (kbShift && k.action == KA_CHAR)
        {
          kbShift = false;
          kbBuild();
          drawKeyboard();
        }
        break;
      }
      }

      if (k.action == KA_DEL || k.action == KA_CHAR || k.action == KA_SPACE)
      {
        if (activeField == 0)
          drawField(SSID_Y, "NETWORK  (tap to scan)", wifiSsid, true, false);
        else
          drawField(PASS_Y, "PASSWORD", wifiPass, true, true);
      }
      break;
    }
  }
}
