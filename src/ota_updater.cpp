#include "ota_updater.h"
#include "gro_core.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>

// version.json is a few hundred bytes; anything bigger is not ours.
static const size_t   OTA_MAX_JSON_BYTES   = 4096;
static const size_t   OTA_PROGRESS_STEP    = 64 * 1024;
static const uint32_t OTA_STALL_TIMEOUT_MS = 10000;

// ── Global maintenance flag ───────────────────────────────────────────
volatile bool g_otaInProgress = false;

// ── Internal state ────────────────────────────────────────────────────
// s_info and s_busy are touched from the check task (core 0) and the caller's task, so
// every access goes through s_mtx. Configuration is written once before use.
static SemaphoreHandle_t s_mtx = xSemaphoreCreateMutex();
static OtaInfo  s_info;                         // zero state == OTA_IDLE
static bool     s_busy            = false;      // a check or an install is running
static bool     s_configured      = false;
static bool     s_allowUnverified = false;
static bool     s_warnedInsecure  = false;
static String   s_version, s_versionUrl, s_defaultBinUrl;
static const char *s_pubKey = nullptr;
static const char *s_caCert = nullptr;
static void   (*s_maintenanceCb)() = nullptr;
static void   (*s_failureCb)()     = nullptr;
static void   (*s_progressCb)(size_t, size_t) = nullptr;

namespace {
struct Lock {
  Lock()  { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
  ~Lock() { if (s_mtx) xSemaphoreGive(s_mtx); }
};

struct BusyGuard {
  ~BusyGuard() { Lock l; s_busy = false; }
};
}  // namespace

static void setState(OtaState st) {
  Lock l;
  s_info.state = st;
}

void otaSetMaintenanceCallback(void (*cb)()) { s_maintenanceCb = cb; }
void otaSetFailureCallback(void (*cb)())     { s_failureCb = cb; }
void otaSetProgressCallback(void (*cb)(size_t, size_t)) { s_progressCb = cb; }
void otaSetPublicKey(const char *pem)        { s_pubKey = (pem && *pem) ? pem : nullptr; }
void otaSetCACert(const char *pem)           { s_caCert = (pem && *pem) ? pem : nullptr; }
void otaAllowUnverified(bool allow)          { s_allowUnverified = allow; }

// ── Helpers ───────────────────────────────────────────────────────────

static bool validRepoName(const char *s) {
  if (!s || !*s) return false;
  for (; *s; s++) {
    if (!(isalnum((unsigned char)*s) || *s == '.' || *s == '_' || *s == '-')) return false;
  }
  return true;
}

static void configureTls(WiFiClientSecure &client) {
  if (s_caCert) {
    client.setCACert(s_caCert);
  } else {
    if (!s_warnedInsecure) {
      s_warnedInsecure = true;
      Serial.println(s_pubKey
        ? "[OTA] note: TLS certificate not validated (no CA set); firmware authenticity relies on the signature."
        : "[OTA] WARNING: TLS certificate not validated and no public key set — firmware is only integrity-checked. See otaSetPublicKey().");
    }
    client.setInsecure();
  }
}

// HTTPS GET with redirect following; the body is only returned for HTTP 200.
static String httpsGet(const String &url, int &httpCode) {
  WiFiClientSecure client;
  configureTls(client);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  http.setUserAgent("ESP32-OTA/" + s_version);
  http.begin(client, url);
  httpCode = http.GET();
  String body;
  if (httpCode == HTTP_CODE_OK) {
    int len = http.getSize();
    if (len > (int)OTA_MAX_JSON_BYTES) {
      httpCode = -1;                          // too large to be a version.json
    } else {
      body = http.getString();
      if (body.length() > OTA_MAX_JSON_BYTES) { body = ""; httpCode = -1; }
    }
  }
  http.end();
  return body;
}

// Stop everything before flashing — calls user callback if registered
static void enterMaintenanceMode() {
  Serial.println("[OTA] entering maintenance mode");
  g_otaInProgress = true;

  // Let running tasks see the flag and finish their current operation
  delay(400);

  // Call user-registered callback (stop I2S, turn off LEDs, etc.)
  if (s_maintenanceCb) s_maintenanceCb();

  Serial.println("[OTA] maintenance mode active");
}

// ── Version check ─────────────────────────────────────────────────────

static void runCheck() {
  if (WiFi.status() != WL_CONNECTED) {
    setState(OTA_ERROR_WIFI);
    return;
  }

  int code = 0;
  String body = httpsGet(s_versionUrl, code);
  if (code != HTTP_CODE_OK || body.isEmpty()) {
    Serial.printf("[OTA] HTTP %d — version.json fetch failed\n", code);
    setState(OTA_ERROR_HTTP);
    return;
  }

  char ver[40] = "", bin[300] = "", sha[80] = "", sig[220] = "", notes[256] = "";
  gro_json_get_string(body.c_str(), "version", ver, sizeof(ver));
  gro_json_get_string(body.c_str(), "bin",     bin, sizeof(bin));
  gro_json_get_string(body.c_str(), "sha256",  sha, sizeof(sha));
  gro_json_get_string(body.c_str(), "sig",     sig, sizeof(sig));
  gro_json_get_string(body.c_str(), "notes",   notes, sizeof(notes));

  bool versionOk = false;
  int cmp = gro_version_compare(ver, s_version.c_str(), &versionOk);
  if (!ver[0] || !versionOk) {
    Serial.println("[OTA] version.json: missing or invalid \"version\"");
    setState(OTA_ERROR_PARSE);
    return;
  }
  if (bin[0] && strncmp(bin, "https://", 8) != 0) {
    Serial.println("[OTA] version.json: \"bin\" must be an https:// URL");
    setState(OTA_ERROR_PARSE);
    return;
  }
  if (sha[0] && !gro_is_sha256_hex(sha)) {
    Serial.println("[OTA] version.json: \"sha256\" is not 64 hex characters");
    setState(OTA_ERROR_PARSE);
    return;
  }
  for (char *c = sha; *c; c++) *c = (char)tolower((unsigned char)*c);

  const bool newer = cmp > 0;
  bool signatureOk = false;
  if (newer && s_pubKey) {
    signatureOk = sha[0] && sig[0] && gro_verify_signature(s_pubKey, ver, sha, sig);
    if (!signatureOk) {
      Serial.println("[OTA] signature missing or invalid — update rejected");
      Lock l;
      s_info.remoteVersion = ver;
      s_info.updateAvailable = false;
      s_info.state = OTA_ERROR_SIGNATURE;
      return;
    }
  }

  {
    Lock l;
    s_info.remoteVersion     = ver;
    s_info.binUrl            = bin[0] ? String(bin) : s_defaultBinUrl;
    s_info.sha256            = sha;
    s_info.notes             = notes;
    s_info.updateAvailable   = newer;
    s_info.signatureVerified = signatureOk;
    s_info.state             = newer ? OTA_UPDATE_AVAILABLE : OTA_UP_TO_DATE;
  }

  Serial.printf("[OTA] local=%s remote=%s sha256=%.8s%s -> %s\n",
    s_version.c_str(), ver, sha[0] ? sha : "none",
    signatureOk ? " (signed)" : "",
    newer ? "UPDATE AVAILABLE" : "up to date");
}

static void checkTask(void *) {
  Serial.println("[OTA] checking version...");
  runCheck();
  { Lock l; s_busy = false; }
  vTaskDelete(nullptr);
}

// ── Public API ────────────────────────────────────────────────────────

bool otaBegin(const char *githubUser, const char *githubRepo, const char *currentVersion) {
  bool versionOk = false;
  gro_version_compare(currentVersion, currentVersion, &versionOk);
  if (!validRepoName(githubUser) || !validRepoName(githubRepo) || !versionOk) {
    Serial.println("[OTA] otaBegin: invalid user, repo or version");
    Lock l;
    s_configured = false;
    s_info.state = OTA_ERROR_CONFIG;
    return false;
  }

  String base = String("https://github.com/") + githubUser + "/" + githubRepo + "/releases/latest/download/";
  {
    Lock l;
    s_version        = currentVersion;
    s_versionUrl     = base + "version.json";
    s_defaultBinUrl  = base + "firmware.bin";
    s_configured     = true;
    s_info.state     = OTA_IDLE;
  }
  Serial.printf("[OTA] firmware v%s\n", currentVersion);
  return true;
}

void otaCheckAsync() {
  {
    Lock l;
    if (s_busy) return;
    if (!s_configured) {
      s_info.state = OTA_ERROR_CONFIG;
      Serial.println("[OTA] not configured — call otaBegin() first");
      return;
    }
    s_busy = true;
    s_info.state = OTA_CHECKING;
    s_info.updateAvailable = false;
  }

  if (xTaskCreatePinnedToCore(checkTask, "ota_check", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    Serial.println("[OTA] could not start check task (out of memory)");
    Lock l;
    s_busy = false;
    s_info.state = OTA_ERROR_HTTP;
  }
}

// Blocking install — enters maintenance mode, streams, verifies, reboots.
// Returns a human-readable error; on success the device restarts and this never returns.
String otaInstall() {
  OtaInfo info;
  {
    Lock l;
    if (!s_configured) return "OTA not configured — call otaBegin()";
    if (s_busy)        return "OTA busy — a check or install is already running";
    if (!s_info.updateAvailable || s_info.binUrl.isEmpty())
      return "No update available — run otaCheckAsync() first";
    s_busy = true;
    info = s_info;
  }
  BusyGuard guard;

  if (WiFi.status() != WL_CONNECTED) {
    setState(OTA_ERROR_WIFI);
    return "No WiFi connection";
  }
  if (info.sha256.isEmpty() && !s_allowUnverified) {
    setState(OTA_ERROR_HASH);
    return "version.json has no sha256 — refusing to install unverified firmware";
  }

  // 1. Maintenance mode
  enterMaintenanceMode();

  WiFiClientSecure dlClient;
  configureTls(dlClient);
  HTTPClient http;
  GroSha256 sha;
  bool updateBegun = false;

  // Every failure path after this point goes through here so cleanup is never skipped.
  auto fail = [&](OtaState st, const String &msg) -> String {
    if (updateBegun) Update.abort();
    http.end();
    g_otaInProgress = false;
    setState(st);
    Serial.printf("[OTA] FAILED: %s\n", msg.c_str());
    if (s_failureCb) s_failureCb();
    return msg;
  };

  // 2. Download firmware.bin
  setState(OTA_DOWNLOADING);
  Serial.printf("[OTA] downloading: %s\n", info.binUrl.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(60000);
  http.setUserAgent("ESP32-OTA/" + s_version);
  http.begin(dlClient, info.binUrl);

  int code = http.GET();
  if (code != HTTP_CODE_OK) return fail(OTA_ERROR_HTTP, "HTTP " + String(code) + " — download failed");

  const int contentLen = http.getSize();
  Serial.printf("[OTA] content-length: %d bytes\n", contentLen);

  // 3. Check flash space
  if (!Update.begin(contentLen > 0 ? (size_t)contentLen : UPDATE_SIZE_UNKNOWN, U_FLASH))
    return fail(OTA_ERROR_NO_SPACE, String("Update.begin failed: ") + Update.errorString());
  updateBegun = true;

  const bool doHash = !info.sha256.isEmpty();
  if (doHash && !sha.begin()) return fail(OTA_ERROR_HASH, "Could not start SHA-256");

  // 4. Stream + hash simultaneously
  WiFiClient *stream = http.getStreamPtr();
  size_t written     = 0;
  size_t nextReport  = OTA_PROGRESS_STEP;
  const size_t target = (contentLen > 0) ? (size_t)contentLen : SIZE_MAX;
  uint8_t buf[1024];
  unsigned long lastProgress = millis();

  Serial.println("[OTA] flashing...");

  while (http.connected() && written < target) {
    size_t avail = stream->available();
    if (!avail) {
      if (millis() - lastProgress > OTA_STALL_TIMEOUT_MS) return fail(OTA_ERROR_FLASH, "Stall timeout during download");
      delay(1);
      continue;
    }
    size_t rd = stream->readBytes(buf, min(avail, sizeof(buf)));
    if (rd == 0) continue;

    if (doHash) sha.update(buf, rd);

    size_t wr = Update.write(buf, rd);
    if (wr != rd) return fail(OTA_ERROR_FLASH, String("Write mismatch: expected ") + rd + " wrote " + wr);
    written += wr;
    lastProgress = millis();

    if (written >= nextReport) {
      nextReport += OTA_PROGRESS_STEP;
      Serial.printf("[OTA] %u / %d bytes (%.0f%%)\n", (unsigned)written, contentLen,
                    contentLen > 0 ? 100.0f * written / contentLen : 0.f);
      if (s_progressCb) s_progressCb(written, contentLen > 0 ? (size_t)contentLen : 0);
    }
  }
  if (s_progressCb) s_progressCb(written, contentLen > 0 ? (size_t)contentLen : 0);
  http.end();

  // A dropped connection must not be mistaken for a complete download.
  if (contentLen > 0 && written != (size_t)contentLen)
    return fail(OTA_ERROR_HTTP, String("Download truncated: ") + (unsigned)written + " of " + contentLen + " bytes");

  // 5. SHA-256 verification
  if (doHash) {
    setState(OTA_VERIFYING);
    char computed[65];
    if (!sha.finishHex(computed)) return fail(OTA_ERROR_HASH, "SHA-256 computation failed");

    Serial.printf("[OTA] SHA-256 expected: %.16s...\n", info.sha256.c_str());
    Serial.printf("[OTA] SHA-256 computed: %.16s...\n", computed);

    if (info.sha256 != computed) return fail(OTA_ERROR_HASH, "SHA-256 mismatch — firmware rejected");
    Serial.println("[OTA] SHA-256 OK");
  } else {
    Serial.println("[OTA] WARNING: no SHA-256 in version.json — installing unverified firmware");
  }

  // 6. Finalise OTA partition
  const bool ended = Update.end(true);
  updateBegun = false;                        // end() released the update either way
  if (!ended) return fail(OTA_ERROR_FLASH, String("Update.end failed: ") + Update.errorString());
  if (!Update.isFinished()) return fail(OTA_ERROR_FLASH, "Update not finished — partition may be corrupt");

  // 7. Success
  setState(OTA_SUCCESS);
  Serial.printf("[OTA] done — wrote %u bytes, rebooting\n", (unsigned)written);
  Serial.flush();
  delay(500);
  ESP.restart();
  return "";  // never reached
}

OtaInfo otaGetInfo() {
  Lock l;
  return s_info;
}

OtaState otaGetState() {
  Lock l;
  return s_info.state;
}

const char *otaStateStr(OtaState s) {
  switch (s) {
    case OTA_IDLE:             return "idle";
    case OTA_CHECKING:         return "checking";
    case OTA_UPDATE_AVAILABLE: return "update_available";
    case OTA_UP_TO_DATE:       return "up_to_date";
    case OTA_DOWNLOADING:      return "downloading";
    case OTA_VERIFYING:        return "verifying";
    case OTA_SUCCESS:          return "success";
    case OTA_ERROR_WIFI:       return "error_wifi";
    case OTA_ERROR_HTTP:       return "error_http";
    case OTA_ERROR_PARSE:      return "error_parse";
    case OTA_ERROR_FLASH:      return "error_flash";
    case OTA_ERROR_NO_SPACE:   return "error_no_space";
    case OTA_ERROR_HASH:       return "error_hash";
    case OTA_ERROR_SIGNATURE:  return "error_signature";
    case OTA_ERROR_CONFIG:     return "error_config";
    default:                   return "unknown";
  }
}

// ── Rollback support ──────────────────────────────────────────────────

bool otaIsPendingVerify() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  return running && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

bool otaMarkValid() {
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}
