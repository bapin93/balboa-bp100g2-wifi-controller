#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdlib>
#include <ctime>

#include "balboa/spa.h"
#include "config.h"
#include "network/web_server.h"
#include "storage/store.h"

namespace {

Store store;
DeviceConfig deviceConfig;
Balboa::SpaController spa;
WebServerHub *web = nullptr;
RuntimeLog runtime;
MaintenanceLog maintenance;
uint32_t lastRuntimeTickMs = 0;
uint32_t lastRuntimeSaveMs = 0;

const char *configuredTimezone() {
  return strlen(deviceConfig.timezone) > 0 ? deviceConfig.timezone : DefaultTimezone;
}

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(AppConfig::Hostname);

  WiFiManager wm;
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

  Serial.printf("[wifi] connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  if (configPortalStarted) {
    Serial.println("[wifi] setup portal completed, restarting before app server start");
    delay(1000);
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
    if (ArduinoOTA.getCommand() == U_FLASH) {
      store.saveRuntime(runtime);  // a filesystem upload replaces the file anyway
    }
    if (maintenance.dirty()) {
      store.saveMaintenance(maintenance);
    }
  });
  ArduinoOTA.onEnd([]() { Serial.println("[ota] end"); });
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("[ota] error=%u\n", error); });
  ArduinoOTA.begin();
}

// Local date as YYYYMMDD, or 0 until NTP has set the clock.
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
  }
  const uint32_t date = localDate();
  if (date == 0) {
    return;  // daily totals need the clock
  }
  RuntimeSample sample;
  sample.heating = state.heating;
  sample.pump1Speed = state.jet1Speed;
  sample.filterRunning = state.activeFilterCycle != 0;
  const bool newDay = runtime.add(date, credited, sample);
  if (newDay || now - lastRuntimeSaveMs >= AppConfig::RuntimeSaveIntervalMs) {
    store.saveRuntime(runtime);
    if (maintenance.dirty()) {
      store.saveMaintenance(maintenance);
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
  runtime.load(store.loadRuntime());
  store.loadMaintenance(maintenance);
  maintenance.ensureDefaultTasks(static_cast<uint32_t>(time(nullptr)));
  if (maintenance.dirty()) {
    store.saveMaintenance(maintenance);
  }

  Serial2.begin(AppConfig::SpaBaud, SERIAL_8N1, AppConfig::Rs485RxPin, AppConfig::Rs485TxPin);
  spa.begin(Serial2, AppConfig::Rs485TxEnablePin);
#ifdef BP100G2_BENCH_MODE
  spa.enableBenchMode();
#endif

  setupWifi();
  setupTime();
  setupOta();

  web = new WebServerHub(spa, store, deviceConfig, runtime, maintenance);
  web->begin();
}

void loop() {
  spa.loop();
  trackRuntime();
  if (web) {
    web->loop();
  }
  ArduinoOTA.handle();
}
