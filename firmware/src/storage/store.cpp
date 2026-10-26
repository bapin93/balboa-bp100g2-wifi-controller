#include "storage/store.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>

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

std::vector<RuntimeDay> Store::loadRuntime() {
  std::vector<RuntimeDay> days;
  File file = LittleFS.open(AppConfig::RuntimePath, "r");
  if (!file) {
    return days;
  }
  JsonDocument doc;
  if (deserializeJson(doc, file)) {
    Serial.println("[store] runtime parse failed");
    return days;
  }
  for (JsonObjectConst item : doc.as<JsonArrayConst>()) {
    RuntimeDay day;
    day.date = item["d"] | 0;
    day.heatingSec = item["h"] | 0;
    day.pumpLowSec = item["l"] | 0;
    day.pumpHighSec = item["p"] | 0;
    day.filterSec = item["f"] | 0;
    if (day.date) {
      days.push_back(day);
    }
  }
  return days;
}

bool Store::saveRuntime(const RuntimeLog &log) {
  File file = LittleFS.open(AppConfig::RuntimePath, "w");
  if (!file) {
    Serial.println("[store] runtime open for write failed");
    return false;
  }
  JsonDocument doc;
  JsonArray days = doc.to<JsonArray>();
  for (const RuntimeDay &day : log.days()) {
    JsonObject item = days.add<JsonObject>();
    item["d"] = day.date;
    item["h"] = day.heatingSec;
    item["l"] = day.pumpLowSec;
    item["p"] = day.pumpHighSec;
    item["f"] = day.filterSec;
  }
  return serializeJson(doc, file) > 0;
}

namespace {
constexpr const char *MaintenanceNamespace = "maint";
constexpr const char *MaintenanceKey = "data";
}  // namespace

void Store::loadMaintenance(MaintenanceLog &log) {
  Preferences prefs;
  if (!prefs.begin(MaintenanceNamespace, true)) {
    return;  // nothing saved yet
  }
  const size_t length = prefs.getBytesLength(MaintenanceKey);
  if (length > 0) {
    std::vector<char> buffer(length);
    prefs.getBytes(MaintenanceKey, buffer.data(), length);
    JsonDocument doc;
    if (deserializeJson(doc, buffer.data(), length)) {
      Serial.println("[store] maintenance parse failed");
    } else {
      log.fromJson(doc.as<JsonVariantConst>());
    }
  }
  prefs.end();
}

bool Store::saveMaintenance(MaintenanceLog &log) {
  JsonDocument doc;
  log.toJson(doc.to<JsonObject>());
  std::string body;
  serializeJson(doc, body);
  Preferences prefs;
  if (!prefs.begin(MaintenanceNamespace, false)) {
    Serial.println("[store] maintenance NVS open failed");
    return false;
  }
  const size_t written = prefs.putBytes(MaintenanceKey, body.data(), body.size());
  prefs.end();
  if (written != body.size()) {
    Serial.printf("[store] maintenance save failed (%u bytes)\n", static_cast<unsigned>(body.size()));
    return false;
  }
  log.clearDirty();
  return true;
}

bool Store::ensureDefaults() {
  if (!LittleFS.exists(AppConfig::ConfigPath)) {
    DeviceConfig defaults;
    return saveConfig(defaults);
  }
  return true;
}
