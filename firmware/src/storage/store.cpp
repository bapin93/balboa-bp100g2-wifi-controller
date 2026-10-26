#include "storage/store.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "config.h"

bool Store::begin() {
  // Web app files: replaced wholesale by uploadfs.
  if (!LittleFS.begin(true)) {
    Serial.println("[store] web partition mount failed");
  }
  // Settings and history get their own partition so uploads don't erase them. Older partition tables
  // don't have it; fall back to the web partition rather than losing settings altogether.
  if (dataFs_.begin(true, "/userdata", 5, AppConfig::DataPartitionLabel)) {
    data_ = &dataFs_;
    Serial.println("[store] settings on the userdata partition");
  } else {
    data_ = &LittleFS;
    Serial.println("[store] no userdata partition; keeping settings with the web files");
  }
  return ensureDefaults();
}

DeviceConfig Store::loadConfig() {
  DeviceConfig config;
  File file = data_->open(AppConfig::ConfigPath, "r");
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
  File file = data_->open(AppConfig::ConfigPath, "w");
  if (!file) {
    Serial.println("[store] config open for write failed");
    return false;
  }

  JsonDocument doc;
  configToJson(config, doc.to<JsonObject>());
  return serializeJsonPretty(doc, file) > 0;
}

void Store::loadRuntime(RuntimeLog &log) {
  File file = data_->open(AppConfig::RuntimePath, "r");
  if (!file) {
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, file)) {
    Serial.println("[store] runtime parse failed");
    return;
  }
  // Older files were just the array of days.
  JsonArrayConst dayItems = doc.is<JsonArrayConst>() ? doc.as<JsonArrayConst>() : doc["days"].as<JsonArrayConst>();
  std::vector<RuntimeDay> days;
  for (JsonObjectConst item : dayItems) {
    RuntimeDay day;
    day.date = item["d"] | 0;
    day.heatingSec = item["h"] | 0;
    day.pumpLowSec = item["l"] | 0;
    day.pumpHighSec = item["p"] | 0;
    day.filterSec = item["f"] | 0;
    day.waterTempAvg = item["w"] | NAN;
    day.waterTempSec = item["ws"] | 0;
    day.setTempAvg = item["s"] | NAN;
    day.setTempSec = item["ss"] | 0;
    if (day.date) {
      days.push_back(day);
    }
  }
  std::vector<RuntimeHour> hours;
  for (JsonObjectConst item : doc["hours"].as<JsonArrayConst>()) {
    RuntimeHour hour;
    hour.start = item["t"] | 0;
    hour.waterTempAvg = item["w"] | NAN;
    hour.waterTempSec = item["ws"] | 0;
    hour.setTempAvg = item["s"] | NAN;
    hour.setTempSec = item["ss"] | 0;
    hour.heatingSec = item["h"] | 0;
    if (hour.start) {
      hours.push_back(hour);
    }
  }
  log.load(days, hours);
}

bool Store::saveRuntime(const RuntimeLog &log) {
  File file = data_->open(AppConfig::RuntimePath, "w");
  if (!file) {
    Serial.println("[store] runtime open for write failed");
    return false;
  }
  JsonDocument doc;
  JsonArray days = doc["days"].to<JsonArray>();
  for (const RuntimeDay &day : log.days()) {
    JsonObject item = days.add<JsonObject>();
    item["d"] = day.date;
    item["h"] = day.heatingSec;
    item["l"] = day.pumpLowSec;
    item["p"] = day.pumpHighSec;
    item["f"] = day.filterSec;
    if (day.waterTempSec) {
      item["w"] = day.waterTempAvg;
      item["ws"] = day.waterTempSec;
    }
    if (day.setTempSec) {
      item["s"] = day.setTempAvg;
      item["ss"] = day.setTempSec;
    }
  }
  JsonArray hours = doc["hours"].to<JsonArray>();
  for (const RuntimeHour &hour : log.hours()) {
    JsonObject item = hours.add<JsonObject>();
    item["t"] = hour.start;
    item["h"] = hour.heatingSec;
    if (hour.waterTempSec) {
      item["w"] = hour.waterTempAvg;
      item["ws"] = hour.waterTempSec;
    }
    if (hour.setTempSec) {
      item["s"] = hour.setTempAvg;
      item["ss"] = hour.setTempSec;
    }
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

uint8_t Store::loadWifiFailedBoots() {
  Preferences prefs;
  if (!prefs.begin("wifi", true)) {
    return 0;
  }
  const uint8_t count = prefs.getUChar("failedBoots", 0);
  prefs.end();
  return count;
}

void Store::saveWifiFailedBoots(uint8_t count) {
  Preferences prefs;
  if (prefs.begin("wifi", false)) {
    prefs.putUChar("failedBoots", count);
    prefs.end();
  }
}

bool Store::ensureDefaults() {
  if (!data_->exists(AppConfig::ConfigPath)) {
    DeviceConfig defaults;
    return saveConfig(defaults);
  }
  return true;
}
