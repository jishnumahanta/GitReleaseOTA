/**
 * GitReleaseOTA — Scheduled OTA Example
 *
 * Checks for a firmware update once at boot, then again every 6 hours.
 * Suitable for battery-powered or always-on devices that should not
 * check continuously but still stay up to date automatically.
 *
 * Hardware: Any ESP32 / ESP32-S3 board
 * Framework: Arduino
 */

#define FW_VERSION      "1.0.0"
#define OTA_GITHUB_USER "your-username"       // ← change this
#define OTA_GITHUB_REPO "your-releases-repo"  // ← change this
#include "ota_updater.h"

const char* WIFI_SSID = "your-ssid";
const char* WIFI_PASS = "your-password";

// Check interval: 6 hours
const unsigned long CHECK_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
unsigned long lastCheckMs = 0;
bool firstCheck = true;

// ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.printf("Connecting to %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nConnected: %s\n", WiFi.localIP().toString().c_str());

  otaInit();
  // First check happens in loop() immediately via firstCheck flag
}

void loop() {
  unsigned long now = millis();

  // Check on first boot, then every CHECK_INTERVAL_MS
  bool shouldCheck = firstCheck || (now - lastCheckMs >= CHECK_INTERVAL_MS);

  if (shouldCheck && WiFi.status() == WL_CONNECTED) {
    if (otaGetState() != OTA_CHECKING) {
      Serial.println("[SCHED] triggering OTA check...");
      otaCheckAsync();
      lastCheckMs = now;
      firstCheck  = false;
    }
  }

  // Install when ready
  if (otaGetState() == OTA_UPDATE_AVAILABLE) {
    Serial.println("[SCHED] update available — installing now");
    String err = otaInstall();  // blocks until reboot
    if (!err.isEmpty()) {
      Serial.println("[SCHED] OTA failed: " + err);
      // Back off before retrying
      lastCheckMs = millis();
    }
  }

  // Your application work here
  delay(100);
}
