/*
 * Radio isolation test for the Waveshare ESP32-S3-Touch-LCD-4.3B.
 *
 * No display, no LVGL, no expander - only the Wi-Fi radio. Answers one question
 * at a time so a failure can't be blamed on the wrong layer:
 *
 *   1. Can the board TRANSMIT?  It starts an open access point. If a phone can see
 *      "ClaudeWidget-Test" in its Wi-Fi list, TX works. If it can also join it, the
 *      board's whole WPA/association path works in the AP role.
 *
 *   2. Can it RECEIVE?  It scans and prints everything it hears with RSSI.
 *
 *   3. Can it JOIN as a station?  Only if you fill in the two defines below. It logs
 *      every disconnect reason code.
 *
 * Flash with the same board settings as the main firmware, open serial at 115200.
 */

#include <WiFi.h>

// Leave blank to skip the station test. Fill in to test joining your network from
// a sketch with nothing else running on the chip.
#define TEST_STA_SSID ""
#define TEST_STA_PASS ""

static const char *AP_NAME = "ClaudeWidget-Test";

static void onEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
  switch (event)
  {
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    Serial.printf("[sta] disconnected, reason %d\n", info.wifi_sta_disconnected.reason);
    break;
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Serial.println("[sta] associated");
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Serial.printf("[sta] got ip %s\n", WiFi.localIP().toString().c_str());
    break;
  case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
    Serial.println("[ap] a device joined the test access point - TX and association both work");
    break;
  default:
    break;
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n[test] radio isolation test");
  Serial.printf("[test] chip %s rev %d, core %s\n", ESP.getChipModel(), ESP.getChipRevision(),
                ESP_ARDUINO_VERSION_STR);
  WiFi.onEvent(onEvent);

  // --- 1. transmit test: open AP ---------------------------------------------
  WiFi.mode(WIFI_AP_STA);
  bool apOk = WiFi.softAP(AP_NAME); // open, no password
  Serial.printf("[ap] softAP('%s') -> %s, mac %s\n", AP_NAME, apOk ? "started" : "FAILED",
                WiFi.softAPmacAddress().c_str());
  Serial.println("[ap] >>> look for this name in a phone's Wi-Fi list <<<");

  // --- 2. receive test: scan ---------------------------------------------------
  int n = WiFi.scanNetworks();
  Serial.printf("[scan] %d networks\n", n);
  for (int i = 0; i < n; i++)
    Serial.printf("[scan]   %-26s ch%-2d %4d dBm auth=%d %s\n", WiFi.SSID(i).c_str(),
                  WiFi.channel(i), WiFi.RSSI(i), (int)WiFi.encryptionType(i),
                  WiFi.BSSIDstr(i).c_str());
  WiFi.scanDelete();

  // --- 3. station test (optional) -----------------------------------------------
  if (strlen(TEST_STA_SSID))
  {
    Serial.printf("[sta] joining %s\n", TEST_STA_SSID);
    WiFi.setAutoReconnect(true);
    WiFi.begin(TEST_STA_SSID, TEST_STA_PASS);
  }
  else
  {
    Serial.println("[sta] skipped (TEST_STA_SSID is empty)");
  }
}

void loop()
{
  static unsigned long last = 0;
  if (millis() - last > 5000)
  {
    last = millis();
    Serial.printf("[test] t=%lus  ap clients=%d  sta=%s\n", millis() / 1000UL,
                  WiFi.softAPgetStationNum(),
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "not connected");
  }
  delay(50);
}
