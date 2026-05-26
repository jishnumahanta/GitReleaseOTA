#include "ota_updater.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>   // hardware-accelerated SHA-256 on ESP32-S3

// ── Global maintenance flag ───────────────────────────────────────────
volatile bool g_otaInProgress = false;

// ── Internal state ────────────────────────────────────────────────────
static OtaInfo  s_info  = { "", OTA_BIN_URL, "", "", false, OTA_IDLE };
static bool     s_busy  = false;
static void   (*s_maintenanceCb)() = nullptr;

void otaSetMaintenanceCallback(void (*cb)()) {
  s_maintenanceCb = cb;
}

// ── Helpers ───────────────────────────────────────────────────────────

static bool versionGreater(const String &a, const String &b) {
  int ma=0,mi_a=0,pa=0, mb=0,mi_b=0,pb=0;
  sscanf(a.c_str(), "%d.%d.%d", &ma, &mi_a, &pa);
  sscanf(b.c_str(), "%d.%d.%d", &mb, &mi_b, &pb);
  if (ma!=mb)    return ma>mb;
  if (mi_a!=mi_b) return mi_a>mi_b;
  return pa>pb;
}

// Minimal JSON string extractor — no heap allocation
static String jsonStr(const String &json, const String &field) {
  String needle = "\"" + field + "\"";
  int idx = json.indexOf(needle);
  if (idx < 0) return "";
  int s = json.indexOf('"', idx + needle.length() + 1);
  if (s < 0) return ""; s++;
  int e = json.indexOf('"', s);
  if (e < 0) return "";
  return json.substring(s, e);
}

// Compute hex SHA-256 of a buffer
static String sha256hex(const uint8_t *data, size_t len) {
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, data, len);
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  String hex = "";
  hex.reserve(65);
  for (int i = 0; i < 32; i++) {
    char tmp[3]; snprintf(tmp, sizeof(tmp), "%02x", hash[i]);
    hex += tmp;
  }
  return hex;
}

// HTTPS GET with redirect following
static String httpsGet(const String &url, int &httpCode) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  http.setUserAgent("ESP32-OTA/" FW_VERSION);
  http.begin(client, url);
  httpCode = http.GET();
  String body = "";
  if (httpCode == HTTP_CODE_OK) body = http.getString();
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

// ── Public API ────────────────────────────────────────────────────────

void otaInit() {
  Serial.printf("[OTA] firmware v%s\n", FW_VERSION);
}

void otaCheckAsync() {
  if (s_busy) return;
  s_busy = true;
  s_info.state = OTA_CHECKING;
  s_info.updateAvailable = false;

  xTaskCreatePinnedToCore([](void*) {
    Serial.println("[OTA] checking version...");

    if (WiFi.status() != WL_CONNECTED) {
      s_info.state = OTA_ERROR_WIFI;
      s_busy = false; vTaskDelete(nullptr); return;
    }

    int code = 0;
    String body = httpsGet(OTA_VERSION_URL, code);

    if (code != HTTP_CODE_OK || body.isEmpty()) {
      Serial.printf("[OTA] HTTP %d — version.json fetch failed\n", code);
      s_info.state = OTA_ERROR_HTTP;
      s_busy = false; vTaskDelete(nullptr); return;
    }

    String remote = jsonStr(body, "version");
    String binUrl = jsonStr(body, "bin");
    String sha256 = jsonStr(body, "sha256");
    String notes  = jsonStr(body, "notes");

    if (remote.isEmpty()) {
      Serial.println("[OTA] version.json parse error");
      s_info.state = OTA_ERROR_PARSE;
      s_busy = false; vTaskDelete(nullptr); return;
    }

    s_info.remoteVersion   = remote;
    s_info.binUrl          = binUrl.isEmpty() ? String(OTA_BIN_URL) : binUrl;
    s_info.sha256          = sha256;
    s_info.notes           = notes;
    s_info.updateAvailable = versionGreater(remote, FW_VERSION);
    s_info.state           = s_info.updateAvailable ? OTA_UPDATE_AVAILABLE : OTA_UP_TO_DATE;

    Serial.printf("[OTA] local=%s remote=%s sha256=%s -> %s\n",
      FW_VERSION, remote.c_str(),
      sha256.isEmpty() ? "none" : sha256.substring(0,8).c_str(),
      s_info.updateAvailable ? "UPDATE AVAILABLE" : "up to date");

    s_busy = false;
    vTaskDelete(nullptr);
  }, "ota_check", 12288, nullptr, 1, nullptr, 0);
}

// Blocking install — enters maintenance mode, streams, verifies SHA-256, reboots
String otaInstall() {
  if (WiFi.status() != WL_CONNECTED) return "No WiFi connection";
  if (s_info.binUrl.isEmpty())        return "No firmware URL — run check first";

  // 1. Maintenance mode
  enterMaintenanceMode();

  // 2. Download firmware.bin
  s_info.state = OTA_DOWNLOADING;
  Serial.printf("[OTA] downloading: %s\n", s_info.binUrl.c_str());

  WiFiClientSecure dlClient;
  dlClient.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(60000);
  http.setUserAgent("ESP32-OTA/" FW_VERSION);
  http.begin(dlClient, s_info.binUrl);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    g_otaInProgress = false;
    s_info.state = OTA_ERROR_HTTP;
    return "HTTP " + String(code) + " — download failed";
  }

  int contentLen = http.getSize();
  Serial.printf("[OTA] content-length: %d bytes\n", contentLen);

  // 3. Check flash space
  if (!Update.begin(contentLen > 0 ? (size_t)contentLen : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    http.end();
    g_otaInProgress = false;
    s_info.state = OTA_ERROR_NO_SPACE;
    return String("Update.begin failed: ") + Update.errorString();
  }

  // 4. Stream + SHA-256 simultaneously
  mbedtls_sha256_context sha_ctx;
  bool doHash = !s_info.sha256.isEmpty();
  if (doHash) {
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
  }

  WiFiClient *stream  = http.getStreamPtr();
  size_t written      = 0;
  size_t target       = (contentLen > 0) ? (size_t)contentLen : SIZE_MAX;
  uint8_t buf[1024];
  unsigned long lastProgress = millis();

  Serial.println("[OTA] flashing...");

  while (http.connected() && written < target) {
    size_t avail = stream->available();
    if (!avail) {
      if (millis() - lastProgress > 10000) {
        http.end(); Update.abort();
        if (doHash) mbedtls_sha256_free(&sha_ctx);
        g_otaInProgress = false;
        s_info.state = OTA_ERROR_FLASH;
        return "Stall timeout during download";
      }
      delay(1); continue;
    }
    size_t rd = stream->readBytes(buf, min(avail, sizeof(buf)));
    if (rd == 0) continue;

    if (doHash) mbedtls_sha256_update(&sha_ctx, buf, rd);

    size_t wr = Update.write(buf, rd);
    if (wr != rd) {
      http.end(); Update.abort();
      if (doHash) mbedtls_sha256_free(&sha_ctx);
      g_otaInProgress = false;
      s_info.state = OTA_ERROR_FLASH;
      return String("Write mismatch: expected ") + rd + " wrote " + wr;
    }
    written += wr;
    lastProgress = millis();

    if (written % (64*1024) < sizeof(buf)) {
      Serial.printf("[OTA] %u / %d bytes (%.0f%%)\n",
        written, contentLen, contentLen > 0 ? 100.0f*written/contentLen : 0.f);
    }
  }
  http.end();

  // 5. SHA-256 verification
  if (doHash) {
    s_info.state = OTA_VERIFYING;
    uint8_t hash[32];
    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);

    String computed = "";
    computed.reserve(65);
    for (int i = 0; i < 32; i++) {
      char tmp[3]; snprintf(tmp, sizeof(tmp), "%02x", hash[i]);
      computed += tmp;
    }

    String expected = s_info.sha256;
    expected.toLowerCase();
    computed.toLowerCase();

    Serial.printf("[OTA] SHA-256 expected: %.16s...\n", expected.c_str());
    Serial.printf("[OTA] SHA-256 computed: %.16s...\n", computed.c_str());

    if (computed != expected) {
      Update.abort();
      g_otaInProgress = false;
      s_info.state = OTA_ERROR_HASH;
      return "SHA-256 mismatch — firmware rejected";
    }
    Serial.println("[OTA] SHA-256 OK");
  } else {
    Serial.println("[OTA] no SHA-256 in version.json — skipping hash check");
  }

  // 6. Finalise OTA partition
  if (!Update.end(true)) {
    g_otaInProgress = false;
    s_info.state = OTA_ERROR_FLASH;
    return String("Update.end failed: ") + Update.errorString();
  }
  if (!Update.isFinished()) {
    g_otaInProgress = false;
    s_info.state = OTA_ERROR_FLASH;
    return "Update not finished — partition may be corrupt";
  }

  // 7. Success
  s_info.state = OTA_SUCCESS;
  Serial.printf("[OTA] done — wrote %u bytes, rebooting\n", written);
  Serial.flush();
  delay(500);
  ESP.restart();
  return "";  // never reached
}

OtaInfo     otaGetInfo()        { return s_info; }
OtaState    otaGetState()       { return s_info.state; }

const char* otaStateStr(OtaState s) {
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
    default:                   return "unknown";
  }
}
