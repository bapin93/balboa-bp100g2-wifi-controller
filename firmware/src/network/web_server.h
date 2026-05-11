#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include "balboa/spa.h"
#include "storage/store.h"

class WebServerHub {
 public:
  WebServerHub(Balboa::SpaController &spa, Store &store, DeviceConfig &config);

  void begin();
  void loop();
  void broadcastState();

 private:
  void setupRoutes();
  void handleCommandBody(AsyncWebServerRequest *request, uint8_t *data, size_t len);
  bool isAuthorized(AsyncWebServerRequest *request) const;
  bool isValidCommandLogName(const String &name) const;
  void sendState(AsyncWebServerRequest *request);
  void sendConfig(AsyncWebServerRequest *request);
  void sendCommandLogList(AsyncWebServerRequest *request);
  void sendCommandLogFile(AsyncWebServerRequest *request);
  void sendCommandResult(AsyncWebServerRequest *request, const Balboa::CommandResult &result);
  void cacheAcceptedCommand(JsonVariantConst command, const Balboa::CommandResult &result);
  void persistFilterCycleCache(const Balboa::FilterCycleCache &cache);
  String stateJson();
  String commandResultJson(const Balboa::CommandResult &result);
  void applyConfigToSpa();

  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  Balboa::SpaController &spa_;
  Store &store_;
  DeviceConfig &config_;
  uint32_t lastBroadcastMs_ = 0;
  bool stateBroadcastPending_ = false;
};
