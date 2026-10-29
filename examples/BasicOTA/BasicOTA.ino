// ─────────────────────────────────────────────────────────────────────
//  GitReleaseOTA — Minimal Example
//
//  This sketch shows how to:
//    1. Configure the updater at runtime (works in Arduino IDE and PlatformIO)
//    2. Check for a new release via Serial
//    3. Install it if one is available
//    4. Register callbacks to stop/restart hardware around the flash
//
//  Before uploading, set the three values marked "change this".
// ─────────────────────────────────────────────────────────────────────

#include <WiFi.h>
#include <GitReleaseOTA.h>

const char* FW_VERSION_STR = "1.0.0";                  // version of THIS firmware
const char* GITHUB_USER    = "your-username";          // change this
const char* GITHUB_REPO    = "your-releases-repo";     // change this

const char* WIFI_SSID = "your-ssid";
const char* WIFI_PASS = "your-password";

// Optional but recommended: authenticate releases with your own signing key.
// Generate one with `./release.sh keygen`, paste the public key here, and set
// OTA_SIGNING_KEY when running release.sh. Leave commented out to use SHA-256 only.
// const char* OTA_PUBLIC_KEY =
//   "-----BEGIN PUBLIC KEY-----\n"
//   "...\n"
//   "-----END PUBLIC KEY-----\n";

// Called just before flashing starts: stop anything sharing the SPI bus, I2S,
// or high-priority tasks.
void beforeFlash() {
  Serial.println("Stopping hardware for OTA...");
  // e.g. i2sStop(); ledOff(); sensorTaskSuspend();
}

// Called if the install fails and the device keeps running the old firmware.
void afterFailedFlash() {
  Serial.println("OTA failed — restarting hardware");
  // e.g. i2sStart(); ledOn();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nConnected: " + WiFi.localIP().toString());

  if (!otaBegin(GITHUB_USER, GITHUB_REPO, FW_VERSION_STR)) {
    Serial.println("OTA configuration is invalid");
    return;
  }
  // otaSetPublicKey(OTA_PUBLIC_KEY);
  otaSetMaintenanceCallback(beforeFlash);
  otaSetFailureCallback(afterFailedFlash);

  // Call this once the firmware has proven healthy after an update (see README: Rollback).
  if (otaIsPendingVerify()) otaMarkValid();

  otaCheckAsync();   // non-blocking
}

void loop() {
  // Print OTA status once it resolves
  static OtaState lastState = OTA_IDLE;
  OtaState state = otaGetState();

  if (state != lastState) {
    Serial.printf("[OTA] state: %s\n", otaStateStr(state));
    lastState = state;

    if (state == OTA_UPDATE_AVAILABLE) {
      OtaInfo info = otaGetInfo();
      Serial.printf("[OTA] update available: %s -> %s\n", FW_VERSION_STR, info.remoteVersion.c_str());
      Serial.printf("[OTA] notes: %s\n", info.notes.c_str());
      Serial.println("[OTA] send 'y' over Serial to install, 'c' to check again");
    }
  }

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'y' && otaGetState() == OTA_UPDATE_AVAILABLE) {
      Serial.println("[OTA] installing...");
      String err = otaInstall();  // blocks until reboot
      if (!err.isEmpty()) Serial.println("[OTA] FAILED: " + err);
    } else if (c == 'c') {
      Serial.println("[OTA] checking for update...");
      otaCheckAsync();
    }
  }

  delay(10);
}
