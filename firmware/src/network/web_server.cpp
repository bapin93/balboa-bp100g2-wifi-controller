#include "network/web_server.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

#include <LittleFS.h>

#include "config.h"
#include "storage/config_model.h"

WebServerHub::WebServerHub(Balboa::SpaController &spa, Store &store, DeviceConfig &config)
    : spa_(spa), store_(store), config_(config) {}

void WebServerHub::begin() {
  applyConfigToSpa();
  setupRoutes();

  ws_.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg,
                     uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      client->text(stateJson());
      return;
    }
    if (type != WS_EVT_DATA) {
      return;
    }

    AwsFrameInfo *info = static_cast<AwsFrameInfo *>(arg);
    if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT) {
      client->text("{\"type\":\"cmd_result\",\"status\":\"invalid_value\",\"message\":\"fragmented websocket messages are unsupported\"}");
      return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, data, len);
    if (error) {
      client->text("{\"type\":\"cmd_result\",\"status\":\"invalid_value\",\"message\":\"invalid json\"}");
      return;
    }

    JsonVariantConst command = doc.as<JsonVariantConst>();
    Balboa::CommandResult result = spa_.handleCommand(command);
    cacheAcceptedCommand(command, result);
    client->text(commandResultJson(result));
  });
  server_.addHandler(&ws_);

  server_.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server_.begin();
  Serial.println("[web] server started on port 80");
}

void WebServerHub::loop() {
  ws_.cleanupClients();
  Balboa::FilterCycleCache filter;
  if (spa_.consumeFilterCycleCache(filter)) {
    persistFilterCycleCache(filter);
  }
  stateBroadcastPending_ = spa_.consumeStateChanged() || stateBroadcastPending_;
  const uint32_t now = millis();
  if (stateBroadcastPending_ && now - lastBroadcastMs_ > AppConfig::StateBroadcastMinMs) {
    broadcastState();
    stateBroadcastPending_ = false;
  } else if (!stateBroadcastPending_ && now - lastBroadcastMs_ > 1000) {
    broadcastState();
  }
}

void WebServerHub::broadcastState() {
  const String json = stateJson();
  ws_.textAll(json);
  lastBroadcastMs_ = millis();
}

void WebServerHub::setupRoutes() {
  server_.on("/api/state", HTTP_GET, [this](AsyncWebServerRequest *request) { sendState(request); });
  server_.on("/api/debug-log", HTTP_GET, [this](AsyncWebServerRequest *request) {
    if (!isAuthorized(request)) {
      return request->requestAuthentication();
    }
    request->send(200, "text/plain", spa_.debugLogText());
  });
  server_.on("/api/debug-log", HTTP_DELETE, [this](AsyncWebServerRequest *request) {
    if (!isAuthorized(request)) {
      return request->requestAuthentication();
    }
    spa_.clearDebugLog();
    request->send(204);
  });
  server_.on("/api/status-capture", HTTP_GET, [this](AsyncWebServerRequest *request) {
    if (!isAuthorized(request)) {
      return request->requestAuthentication();
    }
    request->send(200, "text/plain", spa_.statusCaptureText());
  });
  server_.on("/api/status-capture", HTTP_DELETE, [this](AsyncWebServerRequest *request) {
    if (!isAuthorized(request)) {
      return request->requestAuthentication();
    }
    spa_.clearStatusCapture();
    request->send(204);
  });
  server_.on("/api/command-logs", HTTP_GET, [this](AsyncWebServerRequest *request) {
    if (!isAuthorized(request)) {
      return request->requestAuthentication();
    }
    if (request->hasParam("file")) {
      sendCommandLogFile(request);
    } else {
      sendCommandLogList(request);
    }
  });

  server_.on(
      "/api/cmd", HTTP_POST,
      [](AsyncWebServerRequest *request) {},
      nullptr,
      [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (index == 0 && len == total) {
          handleCommandBody(request, data, len);
        }
      });

  server_.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request) { sendConfig(request); });

  server_.on(
      "/api/config", HTTP_POST,
      [](AsyncWebServerRequest *request) {},
      nullptr,
      [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (index != 0 || len != total) {
          return;
        }
        if (!isAuthorized(request)) {
          return request->requestAuthentication();
        }
        JsonDocument doc;
        DeserializationError jsonError = deserializeJson(doc, data, len);
        if (jsonError) {
          request->send(400, "application/json", "{\"status\":\"invalid_value\",\"message\":\"invalid json\"}");
          return;
        }
        const char *error = nullptr;
        DeviceConfig next;
        if (!validateConfigJson(doc.as<JsonVariantConst>(), config_, next, &error)) {
          JsonDocument out;
          out["status"] = "invalid_value";
          out["message"] = error;
          String body;
          serializeJson(out, body);
          request->send(400, "application/json", body);
          return;
        }
        config_ = next;
        const char *timezone = strlen(config_.timezone) > 0 ? config_.timezone : DefaultTimezone;
        setenv("TZ", timezone, 1);
        tzset();
        configTzTime(timezone, "pool.ntp.org", "time.nist.gov");
        applyConfigToSpa();
        store_.saveConfig(config_);
        sendConfig(request);
      });

  server_.onNotFound([](AsyncWebServerRequest *request) {
    if (request->url().startsWith("/api/")) {
      request->send(404, "application/json", "{\"status\":\"not_found\"}");
      return;
    }
    request->send(LittleFS, "/index.html", "text/html");
  });
}

void WebServerHub::handleCommandBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  if (!isAuthorized(request)) {
    return request->requestAuthentication();
  }
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, data, len);
  if (error) {
    request->send(400, "application/json", "{\"status\":\"invalid_value\",\"message\":\"invalid json\"}");
    return;
  }
  JsonVariantConst command = doc.as<JsonVariantConst>();
  Balboa::CommandResult result = spa_.handleCommand(command);
  cacheAcceptedCommand(command, result);
  sendCommandResult(request, result);
}

bool WebServerHub::isAuthorized(AsyncWebServerRequest *request) const {
  if (!config_.authEnabled || strlen(config_.authPassword) == 0) {
    return true;
  }
  return request->authenticate("admin", config_.authPassword);
}

bool WebServerHub::isValidCommandLogName(const String &name) const {
  if (name.length() == 0 || name.length() > 80 || !name.endsWith(".log")) {
    return false;
  }
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name[i];
    const bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '-' || c == '_' || c == '.';
    if (!valid) {
      return false;
    }
  }
  return true;
}

void WebServerHub::sendState(AsyncWebServerRequest *request) {
  if (!isAuthorized(request)) {
    return request->requestAuthentication();
  }
  request->send(200, "application/json", stateJson());
}

void WebServerHub::sendConfig(AsyncWebServerRequest *request) {
  if (!isAuthorized(request)) {
    return request->requestAuthentication();
  }
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["type"] = "config";
  configToJson(config_, root["data"].to<JsonObject>());
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void WebServerHub::sendCommandLogList(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonArray logs = doc["logs"].to<JsonArray>();
  if (LittleFS.exists("/command-logs")) {
    File root = LittleFS.open("/command-logs");
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        JsonObject item = logs.add<JsonObject>();
        String name = file.name();
        const int slash = name.lastIndexOf('/');
        if (slash >= 0) {
          name = name.substring(slash + 1);
        }
        item["name"] = name;
        item["size"] = file.size();
      }
      file = root.openNextFile();
    }
  }
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

void WebServerHub::sendCommandLogFile(AsyncWebServerRequest *request) {
  const String name = request->getParam("file")->value();
  if (!isValidCommandLogName(name)) {
    request->send(400, "application/json", "{\"status\":\"invalid_value\",\"message\":\"invalid log name\"}");
    return;
  }
  const String path = String("/command-logs/") + name;
  if (!LittleFS.exists(path)) {
    request->send(404, "application/json", "{\"status\":\"not_found\"}");
    return;
  }
  request->send(LittleFS, path, "text/plain");
}

void WebServerHub::sendCommandResult(AsyncWebServerRequest *request, const Balboa::CommandResult &result) {
  const int code = result.status == Balboa::CommandStatus::Accepted ? 202 : 400;
  request->send(code, "application/json", commandResultJson(result));
}

void WebServerHub::cacheAcceptedCommand(JsonVariantConst command, const Balboa::CommandResult &result) {
  if (result.status != Balboa::CommandStatus::Accepted) {
    return;
  }

  const char *action = command["action"] | "";
  if (strcmp(action, "setFilter1") != 0 && strcmp(action, "setFilter2") != 0) {
    return;
  }

  if (strcmp(action, "setFilter1") == 0) {
    config_.filterCycle1Start = static_cast<uint8_t>(command["startHour"] | 0);
    config_.filterCycle1StartMinute = static_cast<uint8_t>(command["startMinute"] | 0);
    config_.filterCycle1Duration = static_cast<uint8_t>(command["durationHour"] | 0);
    config_.filterCycle1DurationMinute = static_cast<uint8_t>(command["durationMinute"] | 0);
  } else {
    config_.filterCycle2Enabled = command["enabled"] | false;
    config_.filterCycle2Start = static_cast<uint8_t>(command["startHour"] | 0);
    config_.filterCycle2StartMinute = static_cast<uint8_t>(command["startMinute"] | 0);
    config_.filterCycle2Duration = static_cast<uint8_t>(command["durationHour"] | 0);
    config_.filterCycle2DurationMinute = static_cast<uint8_t>(command["durationMinute"] | 0);
  }
  applyConfigToSpa();
  store_.saveConfig(config_);
  stateBroadcastPending_ = true;
}

void WebServerHub::persistFilterCycleCache(const Balboa::FilterCycleCache &cache) {
  if (cache.startHour > 23 || cache.startMinute > 45 || cache.startMinute % 15 != 0 ||
      cache.durationHour > 24 || cache.durationMinute > 45 || cache.durationMinute % 15 != 0) {
    Serial.printf("[web] ignoring invalid filter%u cache start=%u:%02u duration=%u:%02u\n",
                  cache.cycle, cache.startHour, cache.startMinute, cache.durationHour, cache.durationMinute);
    return;
  }
  if (cache.cycle == 1) {
    config_.filterCycle1Start = cache.startHour;
    config_.filterCycle1StartMinute = cache.startMinute;
    config_.filterCycle1Duration = cache.durationHour;
    config_.filterCycle1DurationMinute = cache.durationMinute;
  } else if (cache.cycle == 2) {
    config_.filterCycle2Enabled = cache.enabled;
    config_.filterCycle2Start = cache.startHour;
    config_.filterCycle2StartMinute = cache.startMinute;
    config_.filterCycle2Duration = cache.durationHour;
    config_.filterCycle2DurationMinute = cache.durationMinute;
  } else {
    return;
  }
  store_.saveConfig(config_);
  stateBroadcastPending_ = true;
}

String WebServerHub::stateJson() {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["type"] = "state";
  JsonObject data = root["data"].to<JsonObject>();
  spa_.writeStateJson(data);
  spa_.writeCapabilitiesJson(data["capabilities"].to<JsonObject>());
  String body;
  serializeJson(doc, body);
  return body;
}

String WebServerHub::commandResultJson(const Balboa::CommandResult &result) {
  JsonDocument doc;
  doc["type"] = "cmd_result";
  doc["status"] = Balboa::commandStatusName(result.status);
  doc["message"] = result.message;
  String body;
  serializeJson(doc, body);
  return body;
}

void WebServerHub::applyConfigToSpa() {
  Balboa::SpaCapabilities caps;
  caps.jet1 = config_.jet1;
  caps.jet2 = config_.jet2;
  caps.light = config_.light;
  caps.heatMode = config_.heatMode;
  caps.setTime = config_.setTime;
  caps.filterCycles = config_.filterCycles;
  caps.backlight = config_.backlight;
  spa_.setCapabilities(caps);
  spa_.setConfiguredFilterCycles(config_.filterCycle1Start, config_.filterCycle1StartMinute,
                                 config_.filterCycle1Duration, config_.filterCycle1DurationMinute,
                                 config_.filterCycle2Enabled, config_.filterCycle2Start,
                                 config_.filterCycle2StartMinute, config_.filterCycle2Duration,
                                 config_.filterCycle2DurationMinute);
}
