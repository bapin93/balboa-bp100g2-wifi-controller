#pragma once

#include <stdint.h>

#include <ArduinoJson.h>

constexpr const char *DefaultTimezone = "MST7MDT,M3.2.0/2,M11.1.0/2";

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
};

bool validateConfigJson(JsonVariantConst input, const DeviceConfig &current, DeviceConfig &next, const char **error);
void configToJson(const DeviceConfig &config, JsonObject out);
void configFromJson(JsonVariantConst input, DeviceConfig &config);
