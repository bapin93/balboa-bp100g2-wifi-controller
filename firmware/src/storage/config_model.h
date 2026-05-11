#pragma once

#include <stdint.h>

#include <ArduinoJson.h>

constexpr const char *DefaultTimezone = "MST7MDT,M3.2.0/2,M11.1.0/2";

struct DeviceConfig {
  char timezone[64] = "MST7MDT,M3.2.0/2,M11.1.0/2";
  bool authEnabled = false;
  char authPassword[40] = "";
  bool jet1 = true;
  bool jet2 = false;
  bool light = true;
  bool heatMode = false;
  bool setTime = true;
  bool filterCycles = false;
  bool backlight = false;
  uint8_t filterCycle1Start = 8;
  uint8_t filterCycle1StartMinute = 0;
  uint8_t filterCycle1Duration = 2;
  uint8_t filterCycle1DurationMinute = 0;
  bool filterCycle2Enabled = false;
  uint8_t filterCycle2Start = 20;
  uint8_t filterCycle2StartMinute = 0;
  uint8_t filterCycle2Duration = 0;
  uint8_t filterCycle2DurationMinute = 0;
  uint8_t backlightTimeout = 0;
};

bool validateConfigJson(JsonVariantConst input, const DeviceConfig &current, DeviceConfig &next, const char **error);
void configToJson(const DeviceConfig &config, JsonObject out);
void configFromJson(JsonVariantConst input, DeviceConfig &config);
