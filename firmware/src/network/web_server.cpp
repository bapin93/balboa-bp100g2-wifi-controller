#include "network/web_server.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

#include <LittleFS.h>

#include "config.h"
#include "storage/config_model.h"

namespace {

// Filter cycles are owned by the spa. The config copy is only a cache of its last read-back.
void copyFilterCycles(const DeviceConfig &from, DeviceConfig &to) {
  to.filterCycle1Start = from.filterCycle1Start;
  to.filterCycle1StartMinute = from.filterCycle1StartMinute;
  to.filterCycle1Duration = from.filterCycle1Duration;
  to.filterCycle1DurationMinute = from.filterCycle1DurationMinute;
  to.filterCycle2Enabled = from.filterCycle2Enabled;
  to.filterCycle2Start = from.filterCycle2Start;
  to.filterCycle2StartMinute = from.filterCycle2StartMinute;
  to.filterCycle2Duration = from.filterCycle2Duration;
  to.filterCycle2DurationMinute = from.filterCycle2DurationMinute;
}

bool sameFilterCycles(const DeviceConfig &a, const DeviceConfig &b) {
  return a.filterCycle1Start == b.filterCycle1Start && a.filterCycle1StartMinute == b.filterCycle1StartMinute &&
         a.filterCycle1Duration == b.filterCycle1Duration &&
         a.filterCycle1DurationMinute == b.filterCycle1DurationMinute &&
         a.filterCycle2Enabled == b.filterCycle2Enabled && a.filterCycle2Start == b.filterCycle2Start &&
         a.filterCycle2StartMinute == b.filterCycle2StartMinute && a.filterCycle2Duration == b.filterCycle2Duration &&
         a.filterCycle2DurationMinute == b.filterCycle2DurationMinute;
}

}  // namespace

WebServerHub::WebServerHub(Balboa::SpaController &spa, Store &store, DeviceConfig &config)
    : spa_(spa), store_(store), config_(config) {}

void WebServerHub::begin() {
  applyCapabilitiesToSpa();
  applyFilterCyclesToSpa();
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
    client->text(commandResultJson(result));
  });
  server_.addHandler(&ws_);

  // Assets are referenced with ?v=<content hash> (scripts/build_fs.py), so they can be cached forever.
  // index.html is served by its own routes with no-cache. Device-private files are never served.
  server_.serveStatic("/", LittleFS, "/")
      .setCacheControl("public, max-age=31536000, immutable")
      .setFilter([](AsyncWebServerRequest *request) { return request->url() != AppConfig::ConfigPath; });
  server_.begin();
  Serial.println("[web] server started on port 80");
}

void WebServerHub::loop() {
  ws_.cleanupClients();
  if (spa_.consumeFilterCyclesRead()) {
    persistFilterCyclesRead();
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
  server_.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) { sendIndex(request); });
  server_.on("/index.html", HTTP_GET, [this](AsyncWebServerRequest *request) { sendIndex(request); });
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
        copyFilterCycles(config_, next);
        config_ = next;
        const char *timezone = strlen(config_.timezone) > 0 ? config_.timezone : DefaultTimezone;
        setenv("TZ", timezone, 1);
        tzset();
        configTzTime(timezone, "pool.ntp.org", "time.nist.gov");
        applyCapabilitiesToSpa();
        store_.saveConfig(config_);
        sendConfig(request);
      });

  server_.onNotFound([this](AsyncWebServerRequest *request) {
    if (request->url().startsWith("/api/")) {
      request->send(404, "application/json", "{\"status\":\"not_found\"}");
      return;
    }
    sendIndex(request);
  });
}

void WebServerHub::sendIndex(AsyncWebServerRequest *request) {
  AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/index.html", "text/html");
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
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

void WebServerHub::persistFilterCyclesRead() {
  const Balboa::SpaState &spa = spa_.state();
  DeviceConfig read = config_;
  read.filterCycle1Start = spa.filterCycle1Start;
  read.filterCycle1StartMinute = spa.filterCycle1StartMinute;
  read.filterCycle1Duration = spa.filterCycle1Dur;
  read.filterCycle1DurationMinute = spa.filterCycle1DurMinute;
  read.filterCycle2Enabled = spa.filterCycle2Enabled;
  read.filterCycle2Start = spa.filterCycle2Start;
  read.filterCycle2StartMinute = spa.filterCycle2StartMinute;
  read.filterCycle2Duration = spa.filterCycle2Dur;
  read.filterCycle2DurationMinute = spa.filterCycle2DurMinute;
  if (sameFilterCycles(read, config_)) {
    return;  // Every page load reads the cycles; skip the flash write when nothing changed.
  }
  config_ = read;
  store_.saveConfig(config_);
}

void WebServerHub::applyCapabilitiesToSpa() {
  Balboa::SpaCapabilities caps;
  caps.jet1 = config_.jet1;
  caps.jet2 = config_.jet2;
  caps.light = config_.light;
  caps.setTime = config_.setTime;
  caps.backlight = config_.backlight;
  spa_.setCapabilities(caps);
}

// Seeds the display with the last known spa values until the first read-back arrives.
void WebServerHub::applyFilterCyclesToSpa() {
  spa_.setConfiguredFilterCycles(config_.filterCycle1Start, config_.filterCycle1StartMinute,
                                 config_.filterCycle1Duration, config_.filterCycle1DurationMinute,
                                 config_.filterCycle2Enabled, config_.filterCycle2Start,
                                 config_.filterCycle2StartMinute, config_.filterCycle2Duration,
                                 config_.filterCycle2DurationMinute);
}
