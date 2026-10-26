#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include <functional>

#include "balboa/spa.h"
#include "network/weather_client.h"
#include "scheduler/smart_heat.h"
#include "scheduler/thermal_model.h"
#include "storage/maintenance.h"
#include "storage/runtime_log.h"
#include "storage/store.h"
#include "storage/water_log.h"

class WebServerHub {
 public:
  WebServerHub(Balboa::SpaController &spa, Store &store, DeviceConfig &config, const RuntimeLog &runtime,
               MaintenanceLog &maintenance, WaterLog &water, const ThermalModel &thermal,
               const WeatherClient &weather, SmartHeat &smart);

  void begin();
  void loop();
  void broadcastState();  // sends now, changed or not
  // The longest main-loop pass since boot, for /api/system.
  void noteLoopTime(uint32_t ms) { loopMaxMs_ = ms > loopMaxMs_ ? ms : loopMaxMs_; }

 private:
  void setupRoutes();
  void handleCommandBody(AsyncWebServerRequest *request, uint8_t *data, size_t len);
  void handleMaintenanceBody(AsyncWebServerRequest *request, uint8_t *data, size_t len);
  void writeMaintenanceJson(JsonObject out);
  void handleWaterBody(AsyncWebServerRequest *request, uint8_t *data, size_t len);
  void writeWaterJson(JsonObject out);
  void sendInvalid(AsyncWebServerRequest *request, const char *message);
  bool isAuthorized(AsyncWebServerRequest *request) const;
  void sendIndex(AsyncWebServerRequest *request);
  void sendState(AsyncWebServerRequest *request);
  void sendJson(AsyncWebServerRequest *request, const std::function<void(JsonObject)> &fill);
  void sendConfig(AsyncWebServerRequest *request);
  void sendPlanPreview(AsyncWebServerRequest *request);
  void sendCommandResult(AsyncWebServerRequest *request, const Balboa::CommandResult &result);
  Balboa::CommandResult runCommand(JsonVariantConst command);
  void persistFilterCyclesRead();
  String stateJson();
  String commandResultJson(const Balboa::CommandResult &result);
  void applyCapabilitiesToSpa();
  void applyFilterCyclesToSpa();

  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  Balboa::SpaController &spa_;
  Store &store_;
  DeviceConfig &config_;
  const RuntimeLog &runtime_;
  MaintenanceLog &maintenance_;
  WaterLog &water_;
  const ThermalModel &thermal_;
  const WeatherClient &weather_;
  SmartHeat &smart_;
  uint32_t lastBroadcastMs_ = 0;
  String lastStateJson_;  // what was last sent, to skip repeats
  volatile uint32_t loopMaxMs_ = 0;
  bool stateBroadcastPending_ = false;
};
