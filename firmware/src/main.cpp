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
  ArduinoOTA.onStart([]() { Serial.println("[ota] start"); });
  ArduinoOTA.onEnd([]() { Serial.println("[ota] end"); });
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("[ota] error=%u\n", error); });
  ArduinoOTA.begin();
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

  Serial2.begin(AppConfig::SpaBaud, SERIAL_8N1, AppConfig::Rs485RxPin, AppConfig::Rs485TxPin);
  spa.begin(Serial2, AppConfig::Rs485TxEnablePin);
#ifdef BP100G2_BENCH_MODE
  spa.enableBenchMode();
#endif

  setupWifi();
  setupTime();
  setupOta();

  web = new WebServerHub(spa, store, deviceConfig);
  web->begin();
}

void loop() {
  spa.loop();
  if (web) {
    web->loop();
  }
  ArduinoOTA.handle();
}
