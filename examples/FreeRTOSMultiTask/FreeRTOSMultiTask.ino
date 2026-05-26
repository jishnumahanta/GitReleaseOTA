/**
 * GitReleaseOTA — FreeRTOS Multi-Task Example
 *
 * Production-grade pattern for devices with concurrent FreeRTOS tasks
 * (sensors, audio, LEDs, etc.) that must be safely paused before OTA flash.
 *
 * Key points:
 *  - All tasks check g_otaInProgress and pause voluntarily
 *  - beforeFlash() is called after tasks have paused (stops I2S, LEDs, etc.)
 *  - otaCheckAsync() runs on core 0 in its own task — won't block core 1 work
 *
 * Hardware: Any ESP32 / ESP32-S3 board
 * Framework: Arduino + FreeRTOS (included with ESP32 Arduino core)
 */

#define FW_VERSION      "1.0.0"
#define OTA_GITHUB_USER "your-username"       // ← change this
#define OTA_GITHUB_REPO "your-releases-repo"  // ← change this
#include "ota_updater.h"

// ── Wi-Fi credentials ────────────────────────────────────────────────
const char* WIFI_SSID = "your-ssid";
const char* WIFI_PASS = "your-password";

// ── Simulated hardware task: sensor read ─────────────────────────────
void sensorTask(void*) {
  for (;;) {
    // CRITICAL: check OTA flag before touching any hardware
    if (g_otaInProgress) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Normal sensor work
    float temperature = 25.0f; // replace with real sensor read
    Serial.printf("[SENSOR] temp=%.1f°C\n", temperature);

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ── Simulated hardware task: LED animation ───────────────────────────
void ledTask(void*) {
  for (;;) {
    if (g_otaInProgress) {
      // Turn off LED and wait for OTA to finish
      // digitalWrite(LED_PIN, LOW);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Normal LED work
    // ledTick();
    vTaskDelay(pdMS_TO_TICKS(16)); // ~60 fps
  }
}

// ── Maintenance callback — called by OTA just before flash ───────────
// g_otaInProgress is already true when this is called.
// Tasks have had 400 ms to notice and pause.
void beforeFlash() {
  Serial.println("[APP] stopping hardware for OTA...");
  // i2sStop();
  // ledSetState(LED_OFF);
  // sensorPowerDown();
  Serial.println("[APP] hardware stopped — OTA may proceed");
}

// ── OTA monitor task — checks state and installs when ready ──────────
void otaMonitorTask(void*) {
  for (;;) {
    if (otaGetState() == OTA_UPDATE_AVAILABLE) {
      Serial.println("[OTA] update available — installing...");
      String err = otaInstall();  // blocks until reboot (or error)
      if (!err.isEmpty()) {
        Serial.println("[OTA] install failed: " + err);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  // Wi-Fi
  Serial.printf("Connecting to %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nConnected: %s\n", WiFi.localIP().toString().c_str());

  // Spawn application tasks (core 1)
  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(ledTask,    "led",    2048, nullptr, 1, nullptr, 1);

  // OTA setup — otaCheckAsync runs on core 0
  otaSetMaintenanceCallback(beforeFlash);
  otaInit();
  otaCheckAsync();  // non-blocking — spawns its own FreeRTOS task

  // OTA monitor on core 0 (could also be checked in loop())
  xTaskCreatePinnedToCore(otaMonitorTask, "ota_mon", 4096, nullptr, 1, nullptr, 0);
}

void loop() {
  delay(1000);
}
