// ─────────────────────────────────────────────────────────────────────
//  ESP32 OTA GitHub Release Updater — Minimal Example
//
//  This sketch shows how to:
//    1. Configure the updater
//    2. Trigger a version check via Serial or a button
//    3. Install if an update is available
//    4. Register a maintenance callback to stop hardware before flash
//
//  Before uploading, set the 4 defines below to match your project.
// ─────────────────────────────────────────────────────────────────────

// ── Required: set these before including ota_updater.h ────────────────
#define FW_VERSION       "1.0.0"
#define OTA_GITHUB_USER  "your-username"
#define OTA_GITHUB_REPO  "your-releases-repo"

#include <WiFi.h>
#include "ota_updater.h"

// ── WiFi credentials ─────────────────────────────────────────────────
const char* WIFI_SSID = "your-ssid";
const char* WIFI_PASS = "your-password";

// Optional: called just before flashing starts
// Stop anything that shares the SPI bus, I2S, or high-priority tasks.
void beforeFlash() {
  Serial.println("Stopping hardware for OTA...");
  // e.g. i2sStop(); ledOff(); sensorTaskSuspend();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // Connect to WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nConnected: " + WiFi.localIP().toString());

  // Register maintenance callback (optional but recommended)
  otaSetMaintenanceCallback(beforeFlash);

  otaInit();  // prints firmware version to Serial

  // Kick off a non-blocking version check
  otaCheckAsync();
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
      Serial.printf("[OTA] update available: %s -> %s\n", FW_VERSION, info.remoteVersion.c_str());
      Serial.printf("[OTA] notes: %s\n", info.notes.c_str());
      Serial.println("[OTA] send 'y' over Serial to install, any other key to skip");
    }
  }

  // Install on Serial command
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'y' && otaGetState() == OTA_UPDATE_AVAILABLE) {
      Serial.println("[OTA] installing...");
      String err = otaInstall();  // blocks until reboot
      if (!err.isEmpty())
        Serial.println("[OTA] FAILED: " + err);
    } else if (c == 'c') {
      Serial.println("[OTA] checking for update...");
      otaCheckAsync();
    }
  }

  delay(10);
}
