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

bool setFloatIfPresent(JsonVariantConst input, const char *key, float &target, float min, float max,
                       const char *message, const char **error) {
  if (input[key].isNull()) {
    return true;
  }
  if (!input[key].is<float>() || input[key].as<float>() < min || input[key].as<float>() > max) {
    *error = message;
    return false;
  }
  target = input[key].as<float>();
  return true;
}

bool setIntIfPresent(JsonVariantConst input, const char *key, uint8_t &target, int min, int max,
                     const char *message, const char **error) {
  if (input[key].isNull()) {
    return true;
  }
  if (!input[key].is<int>() || input[key].as<int>() < min || input[key].as<int>() > max) {
    *error = message;
    return false;
  }
  target = static_cast<uint8_t>(input[key].as<int>());
  return true;
}

bool validateTou(JsonVariantConst tou, DeviceConfig &next, const char **error) {
  if (!setIntIfPresent(tou, "peakStart", next.tou.peakStart, 0, 23, "peak start must be 0-23", error) ||
      !setIntIfPresent(tou, "peakEnd", next.tou.peakEnd, 0, 24, "peak end must be 0-24", error)) {
    return false;
  }
  setBoolIfPresent(tou, "weekendsOffPeak", next.tou.weekendsOffPeak);
  if (tou["holidays"].is<const char *>() && !holidaySetFromName(tou["holidays"], &next.tou.holidays)) {
    *error = "holidays must be none, federal, federal10 or nerc";
    return false;
  }
  JsonVariantConst rates = tou["rates"];
  constexpr const char *RateError = "rates must be $0 to $5 per kWh";
  return setIntIfPresent(rates, "summerStartMonth", next.rates.summerStartMonth, 1, 12, "months are 1-12", error) &&
         setIntIfPresent(rates, "summerEndMonth", next.rates.summerEndMonth, 1, 12, "months are 1-12", error) &&
         setFloatIfPresent(rates, "summerPeak", next.rates.summerPeak, 0, 5, RateError, error) &&
         setFloatIfPresent(rates, "summerOffPeak", next.rates.summerOffPeak, 0, 5, RateError, error) &&
         setFloatIfPresent(rates, "winterPeak", next.rates.winterPeak, 0, 5, RateError, error) &&
         setFloatIfPresent(rates, "winterOffPeak", next.rates.winterOffPeak, 0, 5, RateError, error);
}

bool validatePower(JsonVariantConst power, DeviceConfig &next, const char **error) {
  constexpr const char *KwError = "power must be 0 to 20 kW";
  if (!setFloatIfPresent(power, "heaterKw", next.power.heaterKw, 0, 20, KwError, error) ||
      !setFloatIfPresent(power, "pump1LowKw", next.power.pump1LowKw, 0, 20, KwError, error) ||
      !setFloatIfPresent(power, "pump1HighKw", next.power.pump1HighKw, 0, 20, KwError, error) ||
      !setFloatIfPresent(power, "pump2Kw", next.power.pump2Kw, 0, 20, KwError, error)) {
    return false;
  }
  if (!power["standbyW"].isNull()) {
    if (!power["standbyW"].is<int>() || power["standbyW"].as<int>() < 0 || power["standbyW"].as<int>() > 1000) {
      *error = "standby must be 0 to 1000 W";
      return false;
    }
    next.power.standbyW = static_cast<uint16_t>(power["standbyW"].as<int>());
  }
  if (!power["tubGallons"].isNull()) {
    if (!power["tubGallons"].is<int>() || power["tubGallons"].as<int>() < 50 || power["tubGallons"].as<int>() > 2000) {
      *error = "tub volume must be 50 to 2000 gallons";
      return false;
    }
    next.power.tubGallons = static_cast<uint16_t>(power["tubGallons"].as<int>());
  }
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

  // "location": {"lat", "lon"} sets it; "location": {} clears it.
  if (input["location"].is<JsonObjectConst>()) {
    JsonVariantConst location = input["location"];
    if (location.size() == 0) {
      next.hasLocation = false;
    } else if (!location["lat"].is<float>() || !location["lon"].is<float>()) {
      *error = "location needs a latitude and longitude";
      return false;
    } else {
      const float lat = location["lat"].as<float>();
      const float lon = location["lon"].as<float>();
      if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        *error = "latitude must be -90 to 90 and longitude -180 to 180";
        return false;
      }
      next.hasLocation = true;
      next.latitude = lat;
      next.longitude = lon;
    }
  }

  JsonVariantConst caps = input["capabilities"];
  if (caps.is<JsonObjectConst>()) {
    setBoolIfPresent(caps, "setTime", next.setTime);
    setBoolIfPresent(caps, "hideJet2", next.hideJet2);
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

  if (input["tou"].is<JsonObjectConst>() && !validateTou(input["tou"], next, error)) {
    return false;
  }
  if (input["power"].is<JsonObjectConst>() && !validatePower(input["power"], next, error)) {
    return false;
  }
  if (input["smart"].is<JsonObjectConst>()) {
    if (!setIntIfPresent(input["smart"], "minTemp", next.smartMinTemp, 50, 104, "lowest temp must be 50-104°F", error)) {
      return false;
    }
    if (!setIntIfPresent(input["smart"], "readyTemp", next.smartReadyTemp, 80, 104, "ready temp must be 80-104°F", error)) {
      return false;
    }
    setBoolIfPresent(input["smart"], "auto", next.smartAuto);
  }

  *error = nullptr;
  return true;
}

void configToJson(const DeviceConfig &config, JsonObject out) {
  out["timezone"] = config.timezone;
  out["authEnabled"] = config.authEnabled;
  out["authPasswordSet"] = strlen(config.authPassword) > 0;

  if (config.hasLocation) {
    JsonObject location = out["location"].to<JsonObject>();
    location["lat"] = config.latitude;
    location["lon"] = config.longitude;
  } else {
    out["location"] = nullptr;
  }

  JsonObject caps = out["capabilities"].to<JsonObject>();
  caps["setTime"] = config.setTime;
  caps["hideJet2"] = config.hideJet2;

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

  JsonObject tou = out["tou"].to<JsonObject>();
  tou["peakStart"] = config.tou.peakStart;
  tou["peakEnd"] = config.tou.peakEnd;
  tou["weekendsOffPeak"] = config.tou.weekendsOffPeak;
  tou["holidays"] = holidaySetName(config.tou.holidays);
  JsonObject rates = tou["rates"].to<JsonObject>();
  rates["summerStartMonth"] = config.rates.summerStartMonth;
  rates["summerEndMonth"] = config.rates.summerEndMonth;
  rates["summerPeak"] = config.rates.summerPeak;
  rates["summerOffPeak"] = config.rates.summerOffPeak;
  rates["winterPeak"] = config.rates.winterPeak;
  rates["winterOffPeak"] = config.rates.winterOffPeak;

  JsonObject power = out["power"].to<JsonObject>();
  power["heaterKw"] = config.power.heaterKw;
  power["pump1LowKw"] = config.power.pump1LowKw;
  power["pump1HighKw"] = config.power.pump1HighKw;
  power["pump2Kw"] = config.power.pump2Kw;
  power["standbyW"] = config.power.standbyW;
  power["tubGallons"] = config.power.tubGallons;

  JsonObject smart = out["smart"].to<JsonObject>();
  smart["minTemp"] = config.smartMinTemp;
  smart["auto"] = config.smartAuto;
  smart["readyTemp"] = config.smartReadyTemp;
}

void configFromJson(JsonVariantConst input, DeviceConfig &config) {
  const char *error = nullptr;
  DeviceConfig next;
  if (validateConfigJson(input, config, next, &error)) {
    config = next;
  }
}
