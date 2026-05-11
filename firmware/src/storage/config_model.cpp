#include "storage/config_model.h"

#include <string.h>
#include <stdio.h>

namespace {

bool copyString(const char *value, char *target, size_t capacity) {
  if (!value || strlen(value) == 0 || strlen(value) >= capacity) {
    return false;
  }
  snprintf(target, capacity, "%s", value);
  return true;
}

void setBoolIfPresent(JsonVariantConst input, const char *key, bool &target) {
  if (input[key].is<bool>()) {
    target = input[key].as<bool>();
  }
}

bool setHourIfPresent(JsonVariantConst input, const char *key, uint8_t &target, const char **error) {
  if (!input[key].is<int>()) {
    return true;
  }
  const int value = input[key].as<int>();
  if (value < 0 || value > 23) {
    *error = "filter cycle start must be 0-23";
    return false;
  }
  target = static_cast<uint8_t>(value);
  return true;
}

bool setDurationIfPresent(JsonVariantConst input, const char *key, uint8_t &target, const char **error) {
  if (!input[key].is<int>()) {
    return true;
  }
  const int value = input[key].as<int>();
  if (value < 0 || value > 24) {
    *error = "filter cycle duration must be 0-24 hours";
    return false;
  }
  target = static_cast<uint8_t>(value);
  return true;
}

bool setQuarterMinuteIfPresent(JsonVariantConst input, const char *key, uint8_t &target, const char **error) {
  if (!input[key].is<int>()) {
    return true;
  }
  const int value = input[key].as<int>();
  if (value < 0 || value > 45 || value % 15 != 0) {
    *error = "filter minutes must be 0, 15, 30, or 45";
    return false;
  }
  target = static_cast<uint8_t>(value);
  return true;
}

}  // namespace

bool validateConfigJson(JsonVariantConst input, const DeviceConfig &current, DeviceConfig &next, const char **error) {
  next = current;

  if (!input.is<JsonObjectConst>()) {
    *error = "config body must be an object";
    return false;
  }

  if (input["timezone"].is<const char *>()) {
    if (!copyString(input["timezone"], next.timezone, sizeof(next.timezone))) {
      *error = "timezone is required and must fit";
      return false;
    }
  }

  if (input["authEnabled"].is<bool>()) {
    next.authEnabled = input["authEnabled"].as<bool>();
  }
  if (input["authPassword"].is<const char *>()) {
    if (!copyString(input["authPassword"], next.authPassword, sizeof(next.authPassword))) {
      *error = "auth password is too long";
      return false;
    }
  }

  JsonVariantConst caps = input["capabilities"];
  if (caps.is<JsonObjectConst>()) {
    setBoolIfPresent(caps, "jet1", next.jet1);
    setBoolIfPresent(caps, "jet2", next.jet2);
    setBoolIfPresent(caps, "light", next.light);
    setBoolIfPresent(caps, "heatMode", next.heatMode);
    setBoolIfPresent(caps, "setTime", next.setTime);
    setBoolIfPresent(caps, "filterCycles", next.filterCycles);
    setBoolIfPresent(caps, "backlight", next.backlight);
  }

  JsonVariantConst filters = input["filterCycles"];
  if (filters.is<JsonObjectConst>()) {
    if (!setHourIfPresent(filters, "cycle1Start", next.filterCycle1Start, error)) {
      return false;
    }
    if (!setQuarterMinuteIfPresent(filters, "cycle1StartMinute", next.filterCycle1StartMinute, error)) {
      return false;
    }
    if (!setDurationIfPresent(filters, "cycle1Duration", next.filterCycle1Duration, error)) {
      return false;
    }
    if (!setQuarterMinuteIfPresent(filters, "cycle1DurationMinute", next.filterCycle1DurationMinute, error)) {
      return false;
    }
    setBoolIfPresent(filters, "cycle2Enabled", next.filterCycle2Enabled);
    if (!setHourIfPresent(filters, "cycle2Start", next.filterCycle2Start, error)) {
      return false;
    }
    if (!setQuarterMinuteIfPresent(filters, "cycle2StartMinute", next.filterCycle2StartMinute, error)) {
      return false;
    }
    if (!setDurationIfPresent(filters, "cycle2Duration", next.filterCycle2Duration, error)) {
      return false;
    }
    if (!setQuarterMinuteIfPresent(filters, "cycle2DurationMinute", next.filterCycle2DurationMinute, error)) {
      return false;
    }
  }

  if (input["backlightTimeout"].is<int>()) {
    const int timeout = input["backlightTimeout"].as<int>();
    if (timeout < 0 || timeout > 60) {
      *error = "backlight timeout must be 0-60 minutes";
      return false;
    }
    next.backlightTimeout = static_cast<uint8_t>(timeout);
  }

  *error = nullptr;
  return true;
}

void configToJson(const DeviceConfig &config, JsonObject out) {
  out["timezone"] = config.timezone;
  out["authEnabled"] = config.authEnabled;
  out["authPasswordSet"] = strlen(config.authPassword) > 0;
  out["backlightTimeout"] = config.backlightTimeout;

  JsonObject caps = out["capabilities"].to<JsonObject>();
  caps["jet1"] = config.jet1;
  caps["jet2"] = config.jet2;
  caps["light"] = config.light;
  caps["heatMode"] = config.heatMode;
  caps["setTime"] = config.setTime;
  caps["filterCycles"] = config.filterCycles;
  caps["backlight"] = config.backlight;

  JsonObject filters = out["filterCycles"].to<JsonObject>();
  filters["cycle1Start"] = config.filterCycle1Start;
  filters["cycle1StartMinute"] = config.filterCycle1StartMinute;
  filters["cycle1Duration"] = config.filterCycle1Duration;
  filters["cycle1DurationMinute"] = config.filterCycle1DurationMinute;
  filters["cycle2Enabled"] = config.filterCycle2Enabled;
  filters["cycle2Start"] = config.filterCycle2Start;
  filters["cycle2StartMinute"] = config.filterCycle2StartMinute;
  filters["cycle2Duration"] = config.filterCycle2Duration;
  filters["cycle2DurationMinute"] = config.filterCycle2DurationMinute;
}

void configFromJson(JsonVariantConst input, DeviceConfig &config) {
  const char *error = nullptr;
  DeviceConfig next;
  if (validateConfigJson(input, config, next, &error)) {
    config = next;
  }
}
