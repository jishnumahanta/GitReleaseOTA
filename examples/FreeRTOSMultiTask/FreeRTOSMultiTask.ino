/**
 * GitReleaseOTA — FreeRTOS Multi-Task Example
 *
 * Production-grade pattern for devices with concurrent FreeRTOS tasks
 * (sensors, audio, LEDs, etc.) that must be safely paused before OTA flash.
 *
 * Key points:
 *  - All tasks check g_otaInProgress and pause voluntarily
 *  - beforeFlash() is called after tasks have had time to pause (stops I2S, LEDs, etc.)
 *  - afterFailedFlash() undoes it if the install fails and the device keeps running
 *  - otaCheckAsync() runs in its own task — won't block the application tasks
 *  - otaInstall() runs TLS in the calling task, so the OTA task needs a big stack (12 KB)
 *
 * Hardware: Any ESP32 / ESP32-S3 board
 * Framework: Arduino + FreeRTOS (included with ESP32 Arduino core)
 */

#include <WiFi.h>
#include <GitReleaseOTA.h>

const char* FW_VERSION_STR = "1.0.0";                  // version of THIS firmware
const char* GITHUB_USER    = "your-username";          // ← change this
const char* GITHUB_REPO    = "your-releases-repo";     // ← change this

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
    Serial.printf("[SENSOR] temp=%.1f C\n", temperature);

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

// ── Maintenance callbacks ────────────────────────────────────────────
// g_otaInProgress is already true when beforeFlash() is called, and
// tasks have had 400 ms to notice and pause.
void beforeFlash() {
  Serial.println("[APP] stopping hardware for OTA...");
  // i2sStop();
  // ledSetState(LED_OFF);
  // sensorPowerDown();
}

// Called when the install failed and we are still running: g_otaInProgress is
// already false again, so the tasks above resume by themselves. Restart anything
// beforeFlash() powered down.
void afterFailedFlash() {
  Serial.println("[APP] OTA failed — restoring hardware");
  // i2sStart();
  // sensorPowerUp();
}

// ── OTA monitor task — checks state and installs when ready ──────────
void otaMonitorTask(void*) {
  for (;;) {
    if (otaGetState() == OTA_UPDATE_AVAILABLE) {
      Serial.println("[OTA] update available — installing...");
      String err = otaInstall();  // blocks until reboot (or error)
      if (!err.isEmpty()) {
        Serial.println("[OTA] install failed: " + err);
        vTaskDelay(pdMS_TO_TICKS(60UL * 60UL * 1000UL));  // try again in an hour
        otaCheckAsync();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

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

  // Spawn application tasks (core 1)
  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(ledTask,    "led",    2048, nullptr, 1, nullptr, 1);

  // OTA setup
  otaBegin(GITHUB_USER, GITHUB_REPO, FW_VERSION_STR);
  otaSetMaintenanceCallback(beforeFlash);
  otaSetFailureCallback(afterFailedFlash);
  if (otaIsPendingVerify()) otaMarkValid();   // or mark valid later, once your self-test passes
  otaCheckAsync();  // non-blocking — spawns its own FreeRTOS task

  // otaInstall() does the TLS handshake on this task's stack: 12 KB, not 4 KB.
  xTaskCreatePinnedToCore(otaMonitorTask, "ota_mon", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
  delay(1000);
}
