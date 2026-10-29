/**
 * GitReleaseOTA — Scheduled OTA Example
 *
 * Checks for a firmware update once at boot, then again every 6 hours.
 * Suitable for always-on devices that should not check continuously but
 * still stay up to date automatically.
 *
 * Hardware: Any ESP32 / ESP32-S3 board
 * Framework: Arduino
 */

#include <WiFi.h>
#include <GitReleaseOTA.h>

const char* FW_VERSION_STR = "1.0.0";                  // version of THIS firmware
const char* GITHUB_USER    = "your-username";          // ← change this
const char* GITHUB_REPO    = "your-releases-repo";     // ← change this

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

  otaBegin(GITHUB_USER, GITHUB_REPO, FW_VERSION_STR);
  if (otaIsPendingVerify()) otaMarkValid();
  // First check happens in loop() immediately via the firstCheck flag
}

void loop() {
  unsigned long now = millis();

  // Check on first boot, then every CHECK_INTERVAL_MS
  bool shouldCheck = firstCheck || (now - lastCheckMs >= CHECK_INTERVAL_MS);

  if (shouldCheck && WiFi.status() == WL_CONNECTED && otaGetState() != OTA_CHECKING) {
    Serial.println("[SCHED] triggering OTA check...");
    otaCheckAsync();
    lastCheckMs = now;
    firstCheck  = false;
  }

  // Install when ready
  if (otaGetState() == OTA_UPDATE_AVAILABLE) {
    Serial.println("[SCHED] update available — installing now");
    String err = otaInstall();  // blocks until reboot
    if (!err.isEmpty()) {
      Serial.println("[SCHED] OTA failed: " + err);
      lastCheckMs = millis();   // back off a full interval before checking again
    }
  }

  // Your application work here
  delay(100);
}
