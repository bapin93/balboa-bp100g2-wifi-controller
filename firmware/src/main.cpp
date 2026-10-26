#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdlib>
#include <ctime>

#include <esp_ota_ops.h>

#include "balboa/spa.h"
#include "config.h"
#include "network/web_server.h"
#include "scheduler/tou.h"
#include "storage/store.h"

// The Arduino core marks a freshly updated firmware "good" as soon as it boots, which disables the
// bootloader's rollback. Returning true defers that to confirmFirmwareOnceHealthy(): if a new firmware
// crashes or never reaches WiFi first, the next boot goes back to the previous one.
extern "C" bool verifyRollbackLater() {
  return true;
}

namespace {

constexpr uint32_t FirmwareProbationMs = 60UL * 1000UL;

Store store;
DeviceConfig deviceConfig;
Balboa::SpaController spa;
WebServerHub *web = nullptr;
RuntimeLog runtime;
MaintenanceLog maintenance;
WaterLog water;
uint32_t lastRuntimeTickMs = 0;
uint32_t lastRuntimeSaveMs = 0;
bool wifiConnected = false;
bool otaStarted = false;
bool failedBootsCleared = false;
bool firmwareConfirmed = false;
uint32_t lastWifiOkMs = 0;
uint32_t lastWifiAttemptMs = 0;

const char *configuredTimezone() {
  return strlen(deviceConfig.timezone) > 0 ? deviceConfig.timezone : DefaultTimezone;
}

void setupOta();
void confirmFirmware(const char *why);

// Opens the blocking setup portal. Only used when there's no saved network, or when several boots in a
// row never managed to join it.
void runSetupPortal(WiFiManager &wm) {
  bool configPortalStarted = false;
  wm.setAPCallback([&configPortalStarted](WiFiManager *) {
    configPortalStarted = true;
  });
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect(AppConfig::WifiApName)) {
    Serial.println("[wifi] setup portal timed out, restarting");
    delay(1000);
    ESP.restart();
  }

  if (configPortalStarted) {
    Serial.println("[wifi] setup portal completed, restarting before app server start");
    delay(1000);
    ESP.restart();
  }
}

// Starts joining WiFi without blocking, so the spa link and web server come up right away.
// superviseWifi() finishes the job and keeps retrying.
void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(AppConfig::Hostname);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // always on mains power; modem sleep adds 100+ ms to every request

  WiFiManager wm;
  const uint8_t failedBoots = store.loadWifiFailedBoots();
  if (!wm.getWiFiIsSaved() || failedBoots >= AppConfig::WifiPortalAfterFailedBoots) {
    Serial.printf("[wifi] opening setup portal (saved=%u failedBoots=%u)\n", wm.getWiFiIsSaved() ? 1 : 0,
                  failedBoots);
    store.saveWifiFailedBoots(0);
    runSetupPortal(wm);
    return;
  }
  // Counted as failed until this boot connects; superviseWifi() clears it.
  store.saveWifiFailedBoots(failedBoots + 1);
  WiFi.begin();
  lastWifiAttemptMs = millis();
  Serial.printf("[wifi] joining saved network \"%s\"\n", wm.getWiFiSSID().c_str());
}

void superviseWifi() {
  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      Serial.printf("[wifi] connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (!otaStarted) {
        setupOta();  // needs the network up for mDNS
        otaStarted = true;
      }
      if (!failedBootsCleared) {
        store.saveWifiFailedBoots(0);
        failedBootsCleared = true;
      }
    }
    lastWifiOkMs = now;
    return;
  }
  if (wifiConnected) {
    wifiConnected = false;
    Serial.println("[wifi] connection lost, retrying");
  }
  if (now - lastWifiAttemptMs >= AppConfig::WifiRetryIntervalMs) {
    lastWifiAttemptMs = now;
    Serial.println("[wifi] retrying saved network");
    WiFi.disconnect(false);
    WiFi.begin();
  }
  if (now - lastWifiOkMs >= AppConfig::WifiRestartAfterMs) {
    Serial.println("[wifi] down for 15 minutes, restarting");
    store.saveRuntime(runtime);
    if (maintenance.dirty()) {
      store.saveMaintenance(maintenance);
    }
    if (water.dirty()) {
      store.saveWater(water);
    }
    delay(200);
    ESP.restart();
  }
}

void setupTime() {
  const char *timezone = configuredTimezone();
  setenv("TZ", timezone, 1);
  tzset();
  configTzTime(timezone, "pool.ntp.org", "time.nist.gov");
  Serial.printf("[time] timezone=%s\n", timezone);
}

void setupOta() {
  ArduinoOTA.setHostname(AppConfig::Hostname);
  ArduinoOTA.onStart([]() {
    Serial.println("[ota] start");
    disableLoopWDT();  // receiving the image blocks loop() far longer than the watchdog allows
    // Taking an update proves WiFi and the main loop work, which is all probation checks. Without this,
    // the restart that follows a web-file upload would look like a failed boot and roll the firmware back.
    confirmFirmware("accepting an update");
    if (ArduinoOTA.getCommand() == U_FLASH) {
      store.saveRuntime(runtime);  // a filesystem upload replaces the file anyway
    }
    if (maintenance.dirty()) {
      store.saveMaintenance(maintenance);
    }
  });
  ArduinoOTA.onEnd([]() { Serial.println("[ota] end"); });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[ota] error=%u\n", error);
    enableLoopWDT();
  });
  ArduinoOTA.begin();
}

void confirmFirmware(const char *why) {
  if (firmwareConfirmed) {
    return;
  }
  firmwareConfirmed = true;
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("[ota] new firmware confirmed (%s); rollback cancelled\n", why);
  }
}

// Ends an update's probation once it has joined WiFi and stayed up for a minute.
void confirmFirmwareOnceHealthy() {
  if (!firmwareConfirmed && wifiConnected && millis() >= FirmwareProbationMs) {
    confirmFirmware("healthy for a minute");
  }
}

// Local date as YYYYMMDD, or 0 until NTP has set the clock.
bool inPeakNow() {
  const time_t now = time(nullptr);
  tm local;
  if (now < 1700000000 || !localtime_r(&now, &local)) {
    return false;
  }
  return isPeakTime(deviceConfig.tou, {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday}, local.tm_hour);
}

uint32_t localDate() {
  const time_t now = time(nullptr);
  tm local;
  if (now < 1700000000 || !localtime_r(&now, &local)) {
    return 0;
  }
  return static_cast<uint32_t>((local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday);
}

void trackRuntime() {
  const uint32_t now = millis();
  const uint32_t elapsedSec = (now - lastRuntimeTickMs) / 1000;
  if (elapsedSec == 0) {
    return;
  }
  lastRuntimeTickMs += elapsedSec * 1000;
  if (!spa.hasFreshState()) {
    return;  // without a live status there is nothing trustworthy to count
  }
  const Balboa::SpaState &state = spa.state();
  const uint32_t credited = elapsedSec > 5 ? 5 : elapsedSec;
  // Water moves through the filters whenever pump 1 or a circulation pump runs.
  if (state.jet1Speed > 0 || state.circulationPump) {
    maintenance.addPumpTime(credited);
    water.addPumpTime(credited);  // FROG cartridges sit in the same flow
  }
  const uint32_t date = localDate();
  if (date == 0) {
    return;  // daily totals need the clock
  }
  RuntimeSample sample;
  sample.heating = state.heating;
  sample.pump1Speed = state.jet1Speed;
  sample.pump2Speed = deviceConfig.hideJet2 ? 0 : state.jet2Speed;
  sample.peak = inPeakNow();
  sample.filterRunning = state.activeFilterCycle != 0;
  sample.waterTemp = state.waterTemp;
  sample.setTemp = state.setTemp;
  const uint32_t epoch = static_cast<uint32_t>(time(nullptr));
  const bool newDay = runtime.add(date, epoch - epoch % 3600, credited, sample);
  if (newDay || now - lastRuntimeSaveMs >= AppConfig::RuntimeSaveIntervalMs) {
    store.saveRuntime(runtime);
    if (maintenance.dirty()) {
      store.saveMaintenance(maintenance);
    }
    if (water.dirty()) {
      store.saveWater(water);
    }
    lastRuntimeSaveMs = now;
  }
}

}  // namespace

void setup() {
  Serial.begin(AppConfig::SerialBaud);
  delay(250);
  Serial.println();
  Serial.println("[boot] BP100G2 manual controller");

  if (!store.begin()) {
    Serial.println("[boot] storage unavailable");
  }
  deviceConfig = store.loadConfig();
  store.loadRuntime(runtime);
  // Until the spa reports water temperature again, show the latest hour we have on record.
  for (auto hour = runtime.hours().rbegin(); hour != runtime.hours().rend(); ++hour) {
    if (hour->waterTempSec) {
      spa.seedLastWaterTemp(hour->waterTempAvg, hour->start + 1800);
      break;
    }
  }
  store.loadMaintenance(maintenance);
  maintenance.ensureDefaultTasks(static_cast<uint32_t>(time(nullptr)));
  if (maintenance.dirty()) {
    store.saveMaintenance(maintenance);
  }
  store.loadWater(water);

  Serial2.begin(AppConfig::SpaBaud, SERIAL_8N1, AppConfig::Rs485RxPin, AppConfig::Rs485TxPin);
  spa.begin(Serial2, AppConfig::Rs485TxEnablePin);
#ifdef BP100G2_BENCH_MODE
  spa.enableBenchMode();
#endif

  setupWifi();
  setupTime();

  web = new WebServerHub(spa, store, deviceConfig, runtime, maintenance, water);
  web->begin();

  // A loop() stuck for 5 s panics and reboots, saving a crash report to the coredump partition.
  enableLoopWDT();
}

void loop() {
  superviseWifi();
  confirmFirmwareOnceHealthy();
#ifdef BP100G2_CRASH_TEST
  // Bench test for rollback: built only with -D BP100G2_CRASH_TEST, never shipped.
  if (millis() > 20000) {
    abort();
  }
#endif
  spa.loop();
  trackRuntime();
  if (web) {
    web->loop();
  }
  if (otaStarted) {
    ArduinoOTA.handle();
  }
}
