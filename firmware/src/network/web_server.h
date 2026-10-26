#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include <functional>

#include "balboa/spa.h"
#include "storage/maintenance.h"
#include "storage/runtime_log.h"
#include "storage/store.h"
#include "storage/water_log.h"

class WebServerHub {
 public:
  WebServerHub(Balboa::SpaController &spa, Store &store, DeviceConfig &config, const RuntimeLog &runtime,
               MaintenanceLog &maintenance, WaterLog &water);

  void begin();
  void loop();
  void broadcastState();

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
  void sendCommandResult(AsyncWebServerRequest *request, const Balboa::CommandResult &result);
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
  uint32_t lastBroadcastMs_ = 0;
  bool stateBroadcastPending_ = false;
};
