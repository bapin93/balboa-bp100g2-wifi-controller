#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <mutex>

#include "balboa/spa.h"
#include "network/weather_client.h"
#include "scheduler/heat_controller.h"
#include "scheduler/thermal_model.h"
#include "scheduler/use_detector.h"
#include "scheduler/usage_model.h"
#include "storage/config_model.h"
#include "storage/store.h"

// Runs HeatController against the real spa: feeds it the live state once a second, sends the set temps
// it asks for, teaches the usage model when the tub gets used, and saves both so they survive a reboot.
// Commands arrive from the web server's task, so everything goes through one lock.
class SmartHeat {
 public:
  SmartHeat(Balboa::SpaController &spa, Store &store, const DeviceConfig &config, const ThermalModel &thermal,
            const WeatherClient &weather);

  void begin();
  void loop();

  // soak {target, readyAt (0 or absent = as soon as possible), hours}, cancelSoak, resumeSmart.
  static bool handles(const char *action);
  Balboa::CommandResult handleCommand(JsonVariantConst command);
  void writeStateJson(JsonObject out, uint32_t now);
  void writePlanJson(JsonObject out);
  // Someone is probably in the tub, as of the last tick.
  bool inUse() const { return inUse_; }

 private:
  ControllerTick makeTick(uint32_t now) const;
  PlanContext makeContext() const;
  float estimateWaterTemp(uint32_t now) const;

  Balboa::SpaController &spa_;
  Store &store_;
  const DeviceConfig &config_;
  const ThermalModel &thermal_;
  const WeatherClient &weather_;
  std::mutex mutex_;
  HeatController controller_;
  UsageModel usage_;
  UseDetector detector_;
  volatile bool inUse_ = false;
  uint32_t seenIntentAt_ = 0;
  uint32_t lastTickMs_ = 0;
  char message_[96] = "";
  char lastError_[96] = "";
};
