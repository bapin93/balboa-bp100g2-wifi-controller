#pragma once

#include <stdint.h>

#include <ArduinoJson.h>

#include "scheduler/tou.h"

constexpr const char *DefaultTimezone = "MST7MDT,M3.2.0/2,M11.1.0/2";

// $/kWh. Summer runs from the first day of summerStartMonth to the end of summerEndMonth.
// All zero means rates haven't been entered and costs aren't shown.
struct EnergyRates {
  float summerPeak = 0;
  float summerOffPeak = 0;
  float winterPeak = 0;
  float winterOffPeak = 0;
  uint8_t summerStartMonth = 6;
  uint8_t summerEndMonth = 9;
};

// What each load draws while running, for cost estimates.
struct PowerSpecs {
  float heaterKw = 4.0f;
  float pump1LowKw = 0.5f;
  float pump1HighKw = 1.8f;
  float pump2Kw = 1.5f;
  uint16_t standbyW = 30;  // control board, ozonator, etc.; counted all day
  uint16_t tubGallons = 200;  // seeds how fast the heater warms the water until it has been measured
};

struct DeviceConfig {
  char timezone[64] = "MST7MDT,M3.2.0/2,M11.1.0/2";
  bool authEnabled = false;
  char authPassword[40] = "";
  bool setTime = true;
  bool hideJet2 = false;  // some packs are configured for a pump that isn't installed
  bool hasLocation = false;  // for outside temperature from Open-Meteo
  float latitude = 0;
  float longitude = 0;
  uint8_t filterCycle1Start = 8;
  uint8_t filterCycle1StartMinute = 0;
  uint8_t filterCycle1Duration = 2;
  uint8_t filterCycle1DurationMinute = 0;
  bool filterCycle2Enabled = false;
  uint8_t filterCycle2Start = 20;
  uint8_t filterCycle2StartMinute = 0;
  uint8_t filterCycle2Duration = 0;
  uint8_t filterCycle2DurationMinute = 0;
  uint8_t smartMinTemp = 90;  // smart heating never plans or sets below this
  bool smartAuto = true;  // keep the tub ready by usage when no soak is planned
  uint8_t smartReadyTemp = 102;  // the temp Smart Standby keeps within reach
  TouSchedule tou;
  EnergyRates rates;
  PowerSpecs power;
};

bool validateConfigJson(JsonVariantConst input, const DeviceConfig &current, DeviceConfig &next, const char **error);
void configToJson(const DeviceConfig &config, JsonObject out);
void configFromJson(JsonVariantConst input, DeviceConfig &config);
