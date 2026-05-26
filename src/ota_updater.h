#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────────────
//  ESP32 OTA GitHub Release Updater
//  Stream firmware from GitHub Releases with SHA-256 integrity check.
//
//  Required defines (set in your main header OR platformio.ini build_flags):
//
//    #define FW_VERSION       "1.0.0"
//    #define OTA_GITHUB_USER  "your-username"
//    #define OTA_GITHUB_REPO  "your-releases-repo"
//
//  The updater fetches version.json from the latest GitHub Release,
//  compares it to FW_VERSION, and streams firmware.bin directly to
//  the OTA partition with hardware-accelerated SHA-256 verification.
// ─────────────────────────────────────────────────────────────────────

#ifndef FW_VERSION
  #error "Define FW_VERSION before including ota_updater.h  e.g. #define FW_VERSION \"1.0.0\""
#endif
#ifndef OTA_GITHUB_USER
  #error "Define OTA_GITHUB_USER before including ota_updater.h"
#endif
#ifndef OTA_GITHUB_REPO
  #error "Define OTA_GITHUB_REPO before including ota_updater.h"
#endif

// Built from your user/repo — no need to touch these
#define OTA_VERSION_URL  "https://github.com/" OTA_GITHUB_USER "/" OTA_GITHUB_REPO "/releases/latest/download/version.json"
#define OTA_BIN_URL      "https://github.com/" OTA_GITHUB_USER "/" OTA_GITHUB_REPO "/releases/latest/download/firmware.bin"

// ── Global maintenance flag ──────────────────────────────────────────
// Set true during flash. Your FreeRTOS tasks should check this and pause.
// Example:
//   if (g_otaInProgress) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
extern volatile bool g_otaInProgress;

// ── States ───────────────────────────────────────────────────────────
enum OtaState {
  OTA_IDLE,
  OTA_CHECKING,
  OTA_UPDATE_AVAILABLE,
  OTA_UP_TO_DATE,
  OTA_DOWNLOADING,
  OTA_VERIFYING,       // SHA-256 in progress
  OTA_SUCCESS,
  OTA_ERROR_WIFI,
  OTA_ERROR_HTTP,
  OTA_ERROR_PARSE,
  OTA_ERROR_FLASH,
  OTA_ERROR_NO_SPACE,
  OTA_ERROR_HASH,      // SHA-256 mismatch — firmware rejected
};

// ── Version info returned after a check ─────────────────────────────
struct OtaInfo {
  String   remoteVersion;
  String   binUrl;
  String   sha256;          // hex SHA-256 from version.json (empty = skip check)
  String   notes;
  bool     updateAvailable;
  OtaState state;
};

// ── Maintenance callback (optional) ─────────────────────────────────
// Register a function to be called before flashing starts.
// Use it to stop I2S, turn off LEDs, pause sensors, etc.
// Example:
//   void onOtaStart() { i2sStop(); ledSetState(LED_OFF); }
//   otaSetMaintenanceCallback(onOtaStart);
void otaSetMaintenanceCallback(void (*cb)());

// ── Public API ───────────────────────────────────────────────────────
void        otaInit();         // call once in setup() — prints firmware version
void        otaCheckAsync();   // non-blocking version check (spawns FreeRTOS task)
String      otaInstall();      // blocking: download → verify → flash → reboot
OtaInfo     otaGetInfo();      // get last check result
OtaState    otaGetState();
const char* otaStateStr(OtaState s);
