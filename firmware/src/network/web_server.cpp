#include "network/web_server.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

#include <LittleFS.h>

#include "config.h"

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

    Balboa::CommandResult result = spa_.handleCommand(doc.as<JsonVariantConst>());
    client->text(commandResultJson(result));
  });
  server_.addHandler(&ws_);

  server_.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server_.begin();
  Serial.println("[web] server started on port 80");
}

void WebServerHub::loop() {
  ws_.cleanupClients();
  if (spa_.consumeStateChanged() && millis() - lastBroadcastMs_ > AppConfig::StateBroadcastMinMs) {
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
        setenv("TZ", config_.timezone, 1);
        tzset();
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
  Balboa::CommandResult result = spa_.handleCommand(doc.as<JsonVariantConst>());
  sendCommandResult(request, result);
}

bool WebServerHub::isAuthorized(AsyncWebServerRequest *request) const {
  if (!config_.authEnabled || strlen(config_.authPassword) == 0) {
    return true;
  }
  return request->authenticate("admin", config_.authPassword);
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

void WebServerHub::sendCommandResult(AsyncWebServerRequest *request, const Balboa::CommandResult &result) {
  const int code = result.status == Balboa::CommandStatus::Accepted ? 202 : 400;
  request->send(code, "application/json", commandResultJson(result));
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
  spa_.setConfiguredFilterCycles(config_.filterCycle1Start, config_.filterCycle1Duration,
                                 config_.filterCycle2Enabled, config_.filterCycle2Start,
                                 config_.filterCycle2Duration);
}
