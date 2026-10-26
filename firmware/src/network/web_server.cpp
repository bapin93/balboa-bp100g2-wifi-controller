#include "network/web_server.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

#include <LittleFS.h>
#include <WiFi.h>
#include <esp_core_dump.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "build_info.h"

#include "config.h"
#include "storage/config_model.h"

namespace {

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
  }
}

const char *otaStateName(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "on probation";
    case ESP_OTA_IMG_VALID: return "confirmed";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "rolled back";
    default: return "flashed over USB";
  }
}

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

WebServerHub::WebServerHub(Balboa::SpaController &spa, Store &store, DeviceConfig &config,
                           const RuntimeLog &runtime, MaintenanceLog &maintenance, WaterLog &water)
    : spa_(spa), store_(store), config_(config), runtime_(runtime), maintenance_(maintenance), water_(water) {}

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
  // Controller health: why it last restarted, memory, WiFi, firmware slot and crash-report status.
  server_.on("/api/system", HTTP_GET, [this](AsyncWebServerRequest *request) {
    sendJson(request, [this](JsonObject out) {
      out["uptimeSec"] = millis() / 1000;
      out["resetReason"] = resetReasonName(esp_reset_reason());
      out["freeHeap"] = ESP.getFreeHeap();
      out["minFreeHeap"] = ESP.getMinFreeHeap();
      out["largestFreeBlock"] = ESP.getMaxAllocHeap();
      out["wifiRssi"] = WiFi.RSSI();
      out["built"] = FIRMWARE_BUILD;
      const esp_partition_t *running = esp_ota_get_running_partition();
      out["firmwareSlot"] = running->label;
      esp_ota_img_states_t state;
      out["firmwareState"] = esp_ota_get_state_partition(running, &state) == ESP_OK ? otaStateName(state)
                                                                                    : "flashed over USB";
      out["crashReportStored"] = esp_core_dump_image_check() == ESP_OK;
      out["settingsPartition"] = store_.hasDataPartition() ? "userdata" : "shared with web files";
    });
  });
  server_.on("/api/spa-info", HTTP_GET, [this](AsyncWebServerRequest *request) {
    sendJson(request, [this](JsonObject out) { spa_.writeSpaInfoJson(out); });
  });
  server_.on("/api/faults", HTTP_GET, [this](AsyncWebServerRequest *request) {
    sendJson(request, [this](JsonObject out) { spa_.writeFaultLogJson(out); });
  });
  server_.on("/api/runtime", HTTP_GET, [this](AsyncWebServerRequest *request) {
    sendJson(request, [this](JsonObject out) {
      JsonArray days = out["days"].to<JsonArray>();
      for (const RuntimeDay &day : runtime_.days()) {
        JsonObject item = days.add<JsonObject>();
        item["date"] = day.date;
        item["heatingSec"] = day.heatingSec;
        item["pumpLowSec"] = day.pumpLowSec;
        item["pumpHighSec"] = day.pumpHighSec;
        item["filterSec"] = day.filterSec;
        if (day.waterTempSec) {
          item["waterTemp"] = roundf(day.waterTempAvg * 10.0f) / 10.0f;
        }
        if (day.setTempSec) {
          item["setTemp"] = roundf(day.setTempAvg * 10.0f) / 10.0f;
        }
      }
      JsonArray hours = out["hours"].to<JsonArray>();
      for (const RuntimeHour &hour : runtime_.hours()) {
        JsonObject item = hours.add<JsonObject>();
        item["start"] = hour.start;
        item["heatingSec"] = hour.heatingSec;
        if (hour.waterTempSec) {
          item["waterTemp"] = roundf(hour.waterTempAvg * 10.0f) / 10.0f;
        }
        if (hour.setTempSec) {
          item["setTemp"] = roundf(hour.setTempAvg * 10.0f) / 10.0f;
        }
      }
    });
  });
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

  server_.on("/api/maintenance", HTTP_GET, [this](AsyncWebServerRequest *request) {
    sendJson(request, [this](JsonObject out) { writeMaintenanceJson(out); });
  });
  server_.on(
      "/api/maintenance", HTTP_POST,
      [](AsyncWebServerRequest *request) {},
      nullptr,
      [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (index == 0 && len == total) {
          handleMaintenanceBody(request, data, len);
        }
      });

  server_.on("/api/water", HTTP_GET, [this](AsyncWebServerRequest *request) {
    sendJson(request, [this](JsonObject out) { writeWaterJson(out); });
  });
  server_.on(
      "/api/water", HTTP_POST,
      [](AsyncWebServerRequest *request) {},
      nullptr,
      [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (index == 0 && len == total) {
          handleWaterBody(request, data, len);
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

void WebServerHub::writeMaintenanceJson(JsonObject out) {
  maintenance_.toJson(out);
  out["now"] = static_cast<uint32_t>(time(nullptr));
}

void WebServerHub::handleMaintenanceBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  if (!isAuthorized(request)) {
    return request->requestAuthentication();
  }
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    request->send(400, "application/json", "{\"status\":\"invalid_value\",\"message\":\"invalid json\"}");
    return;
  }
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  const char *error = nullptr;
  if (now < 1700000000) {
    error = "the controller clock isn't set yet";
  }
  // Actions take an optional date so a swap can be recorded after the fact; it can't be in the future.
  const uint32_t ts = doc["ts"] | now;
  if (!error && (ts < 1600000000 || ts > now + 600)) {
    error = "date must not be in the future";
  }
  const char *action = doc["action"] | "";
  const uint16_t id = doc["id"] | 0;
  if (!error) {
    if (strcmp(action, "add") == 0) {
      error = maintenance_.addFilter(doc["name"] | "", ts);
    } else if (strcmp(action, "rename") == 0) {
      error = maintenance_.renameFilter(id, doc["name"] | "");
    } else if (strcmp(action, "delete") == 0) {
      error = maintenance_.deleteFilter(id, now);
    } else if (strcmp(action, "install") == 0) {
      error = maintenance_.install(id, ts);
    } else if (strcmp(action, "remove") == 0) {
      error = maintenance_.takeOut(id, ts);
    } else if (strcmp(action, "swap") == 0) {
      error = maintenance_.swap(doc["outId"] | 0, doc["inId"] | 0, ts);
    } else if (strcmp(action, "clean") == 0) {
      error = maintenance_.clean(id, ts);
    } else if (strcmp(action, "addTask") == 0) {
      error = maintenance_.addTask(doc["name"] | "", doc["intervalDays"] | 0, now);
    } else if (strcmp(action, "editTask") == 0) {
      error = maintenance_.editTask(id, doc["name"] | "", doc["intervalDays"] | 0);
    } else if (strcmp(action, "deleteTask") == 0) {
      error = maintenance_.deleteTask(id);
    } else if (strcmp(action, "taskDone") == 0) {
      error = maintenance_.completeTask(id, ts);
    } else {
      error = "unknown maintenance action";
    }
  }
  if (error) {
    sendInvalid(request, error);
    return;
  }
  store_.saveMaintenance(maintenance_);
  sendJson(request, [this](JsonObject out) { writeMaintenanceJson(out); });
}

void WebServerHub::writeWaterJson(JsonObject out) {
  water_.toJson(out);
  out["now"] = static_cast<uint32_t>(time(nullptr));
}

void WebServerHub::handleWaterBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  if (!isAuthorized(request)) {
    return request->requestAuthentication();
  }
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    request->send(400, "application/json", "{\"status\":\"invalid_value\",\"message\":\"invalid json\"}");
    return;
  }
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  const char *error = nullptr;
  if (now < 1700000000) {
    error = "the controller clock isn't set yet";
  }
  // Like maintenance, entries can be backdated but not put in the future.
  const uint32_t ts = doc["ts"] | now;
  if (!error && (ts < 1600000000 || ts > now + 600)) {
    error = "date must not be in the future";
  }
  const char *action = doc["action"] | "";
  CartridgeSlot slot = CartridgeSlot::Sanitizer;
  const bool hasSlot = cartridgeSlotFromName(doc["slot"] | "", &slot);
  if (!error) {
    if (strcmp(action, "reading") == 0) {
      error = water_.addReading(ts, doc["san"] | NAN, doc["ph"] | NAN, doc["ta"] | NAN, doc["th"] | NAN,
                                doc["note"] | "");
    } else if (strcmp(action, "dose") == 0) {
      error = water_.addDose(ts, doc["chem"] | "", doc["amount"] | NAN, doc["unit"] | "", doc["note"] | "");
    } else if (strcmp(action, "replace") == 0) {
      error = hasSlot ? water_.replaceCartridge(slot, ts, doc["dial"] | -1) : "unknown cartridge";
    } else if (strcmp(action, "dial") == 0) {
      error = water_.setDial(ts, doc["dial"] | -1);
    } else if (strcmp(action, "life") == 0) {
      error = hasSlot ? water_.setLifeDays(slot, doc["days"] | 0) : "unknown cartridge";
    } else if (strcmp(action, "system") == 0) {
      error = water_.setSystem(doc["system"] | "");
    } else if (strcmp(action, "delete") == 0) {
      error = water_.deleteEntry(doc["id"] | 0);
    } else {
      error = "unknown water action";
    }
  }
  if (error) {
    sendInvalid(request, error);
    return;
  }
  store_.saveWater(water_);
  sendJson(request, [this](JsonObject out) { writeWaterJson(out); });
}

void WebServerHub::sendInvalid(AsyncWebServerRequest *request, const char *message) {
  JsonDocument out;
  out["status"] = "invalid_value";
  out["message"] = message;
  String body;
  serializeJson(out, body);
  request->send(400, "application/json", body);
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

void WebServerHub::sendJson(AsyncWebServerRequest *request, const std::function<void(JsonObject)> &fill) {
  if (!isAuthorized(request)) {
    return request->requestAuthentication();
  }
  JsonDocument doc;
  fill(doc.to<JsonObject>());
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
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
  spa_.setUserCapabilities(config_.setTime, config_.hideJet2);
}

// Seeds the display with the last known spa values until the first read-back arrives.
void WebServerHub::applyFilterCyclesToSpa() {
  spa_.setConfiguredFilterCycles(config_.filterCycle1Start, config_.filterCycle1StartMinute,
                                 config_.filterCycle1Duration, config_.filterCycle1DurationMinute,
                                 config_.filterCycle2Enabled, config_.filterCycle2Start,
                                 config_.filterCycle2StartMinute, config_.filterCycle2Duration,
                                 config_.filterCycle2DurationMinute);
}
