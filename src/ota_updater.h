#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────────────
//  GitReleaseOTA — ESP32 OTA updates from GitHub Releases
//
//  Fetches version.json from the latest GitHub Release, compares it to
//  your firmware version, streams firmware.bin into the inactive OTA
//  partition and verifies it (SHA-256, plus an optional ECDSA signature)
//  before activating it.
//
//  Configure at runtime — works in the Arduino IDE and PlatformIO:
//
//    otaBegin("your-username", "your-releases-repo", "1.0.0");
//
//  Legacy compile-time configuration still works: define FW_VERSION,
//  OTA_GITHUB_USER and OTA_GITHUB_REPO (before including this header, or
//  as PlatformIO build_flags) and call otaInit().
// ─────────────────────────────────────────────────────────────────────

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
  OTA_ERROR_HASH,      // SHA-256 mismatch (or missing) — firmware rejected
  OTA_ERROR_SIGNATURE, // signature missing/invalid for the configured public key
  OTA_ERROR_CONFIG,    // otaBegin() not called or given invalid arguments
};

// ── Version info returned after a check ─────────────────────────────
struct OtaInfo {
  String   remoteVersion;
  String   binUrl;
  String   sha256;          // lowercase hex SHA-256 from version.json
  String   notes;
  bool     updateAvailable;
  OtaState state;
  bool     signatureVerified;  // true when a public key is set and the signature checked out
};

// ── Configuration ────────────────────────────────────────────────────
// Call once in setup(), before otaCheckAsync(). `githubUser`/`githubRepo` identify the
// releases repository (letters, digits, '.', '_', '-'); `currentVersion` is the version
// of the running firmware ("1.2.3", optional leading "v", optional "-rc1" suffix).
// Returns false (and sets OTA_ERROR_CONFIG) on invalid arguments.
bool otaBegin(const char *githubUser, const char *githubRepo, const char *currentVersion);

// Trust anchor for firmware authenticity. `pem` must be a NUL-terminated ECDSA P-256
// public key (see `./release.sh keygen`) and must stay valid for the program's lifetime.
// Once set, version.json must carry a valid "sig" or the update is rejected.
void otaSetPublicKey(const char *pem);

// Optional TLS validation. Pass a root CA PEM (kept valid for the program's lifetime) to
// validate the server certificate; without it the connection is encrypted but not
// authenticated (use otaSetPublicKey() so that doesn't matter).
void otaSetCACert(const char *pem);

// By default a release without "sha256" is refused. Set true to allow installing it anyway
// (not recommended).
void otaAllowUnverified(bool allow);

// ── Callbacks (all optional) ─────────────────────────────────────────
// Before flashing starts: stop I2S, turn off LEDs, pause sensors, etc.
void otaSetMaintenanceCallback(void (*cb)());
// After an install attempt fails and the device keeps running: restart what you stopped.
void otaSetFailureCallback(void (*cb)());
// During download: bytes written so far and total (total is 0 if the server didn't say).
void otaSetProgressCallback(void (*cb)(size_t written, size_t total));

// ── Public API ───────────────────────────────────────────────────────
void        otaCheckAsync();   // non-blocking version check (spawns a FreeRTOS task)
String      otaInstall();      // blocking: download → verify → flash → reboot. Returns an error message.
                               // Runs TLS in the calling task: give it >= 10 KB of stack.
OtaInfo     otaGetInfo();      // thread-safe copy of the last check result
OtaState    otaGetState();
const char* otaStateStr(OtaState s);

// ── Rollback support ─────────────────────────────────────────────────
// After booting a freshly installed image, call otaMarkValid() once the firmware has
// proved healthy. Automatic rollback to the previous slot only happens if the bootloader
// was built with CONFIG_APP_ROLLBACK_ENABLE (ESP-IDF); stock Arduino cores do not enable it.
bool otaIsPendingVerify();
bool otaMarkValid();

// ── Legacy compile-time configuration ────────────────────────────────
#if defined(FW_VERSION) && defined(OTA_GITHUB_USER) && defined(OTA_GITHUB_REPO)
static inline void otaInit() { otaBegin(OTA_GITHUB_USER, OTA_GITHUB_REPO, FW_VERSION); }
#endif
