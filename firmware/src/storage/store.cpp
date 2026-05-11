#include "storage/store.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "config.h"

bool Store::begin() {
  if (!LittleFS.begin(true)) {
    Serial.println("[store] LittleFS mount failed");
    return false;
  }
  return ensureDefaults();
}

DeviceConfig Store::loadConfig() {
  DeviceConfig config;
  File file = LittleFS.open(AppConfig::ConfigPath, "r");
  if (!file) {
    saveConfig(config);
    return config;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.printf("[store] config parse failed: %s\n", error.c_str());
    return config;
  }

  configFromJson(doc.as<JsonVariantConst>(), config);
  return config;
}

bool Store::saveConfig(const DeviceConfig &config) {
  File file = LittleFS.open(AppConfig::ConfigPath, "w");
  if (!file) {
    Serial.println("[store] config open for write failed");
    return false;
  }

  JsonDocument doc;
  configToJson(config, doc.to<JsonObject>());
  return serializeJsonPretty(doc, file) > 0;
}

bool Store::ensureDefaults() {
  if (!LittleFS.exists(AppConfig::ConfigPath)) {
    DeviceConfig defaults;
    return saveConfig(defaults);
  }
  return true;
}
