#include "balboa/spa.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <utility>

#include "config.h"

namespace Balboa {
namespace {

constexpr uint8_t ButtonTempUp = 0x50;
constexpr uint8_t ButtonTempDown = 0x51;
constexpr uint8_t ButtonJet1 = 0x04;
constexpr uint8_t ButtonJet2 = 0x05;
constexpr uint8_t ButtonLight = 0x11;
constexpr uint8_t ButtonHeatMode = 0x51;

bool isFiniteTemp(float value) {
  return !isnan(value) && value > 30.0f && value < 120.0f;
}

float decodeTemp(uint8_t raw) {
  if (raw == 0xff || raw == 0x00) {
    return NAN;
  }
  return static_cast<float>(raw);
}

}  // namespace

void SpaController::begin(HardwareSerial &serial, int txEnablePin) {
  serial_ = &serial;
  txEnablePin_ = txEnablePin;
  pinMode(txEnablePin_, OUTPUT);
  digitalWrite(txEnablePin_, LOW);
}

void SpaController::enableBenchMode() {
  benchMode_ = true;
  state_.waterTemp = 98.0f;
  state_.setTemp = 100.0f;
  state_.heatMode = 0;
  state_.heating = true;
  state_.hour = 12;
  state_.minute = 0;
  state_.filterCycle1Start = 8;
  state_.filterCycle1Dur = 2;
  state_.filterCycle2Enabled = false;
  state_.filterCycle2Start = 20;
  state_.filterCycle2Dur = 0;
  state_.lastUpdateMs = millis();
  state_.valid = true;
  stateChanged_ = true;
  Serial.println("[bench] simulated spa state enabled");
}

void SpaController::loop() {
  if (benchMode_) {
    updateBenchState();
    return;
  }
  processRx();
  processTx();
}

bool SpaController::hasFreshState(uint32_t nowMs) const {
  return state_.valid && nowMs - state_.lastUpdateMs <= AppConfig::StateStaleMs;
}

bool SpaController::consumeStateChanged() {
  const bool changed = stateChanged_;
  stateChanged_ = false;
  return changed;
}

void SpaController::setConfiguredFilterCycles(uint8_t cycle1Start, uint8_t cycle1Duration, bool cycle2Enabled,
                                              uint8_t cycle2Start, uint8_t cycle2Duration) {
  if (!benchMode_) {
    return;
  }
  state_.filterCycle1Start = cycle1Start;
  state_.filterCycle1Dur = cycle1Duration;
  state_.filterCycle2Enabled = cycle2Enabled;
  state_.filterCycle2Start = cycle2Start;
  state_.filterCycle2Dur = cycle2Duration;
  state_.lastUpdateMs = millis();
  stateChanged_ = true;
  Serial.printf("[bench] filter cycles c1=%u/%uh c2=%s %u/%uh\n", cycle1Start, cycle1Duration,
                cycle2Enabled ? "on" : "off", cycle2Start, cycle2Duration);
}

void SpaController::writeStateJson(JsonObject out) const {
  if (isnan(state_.waterTemp)) {
    out["waterTemp"] = nullptr;
  } else {
    out["waterTemp"] = state_.waterTemp;
  }
  if (isnan(state_.setTemp)) {
    out["setTemp"] = nullptr;
  } else {
    out["setTemp"] = state_.setTemp;
  }
  out["jet1"] = state_.jet1;
  out["jet2"] = state_.jet2;
  out["blower"] = state_.blower;
  out["light"] = state_.light;
  out["heatMode"] = state_.heatMode;
  out["heating"] = state_.heating;
  out["priming"] = state_.priming;
  out["tempRangeHigh"] = state_.tempRangeHigh;
  JsonObject filters = out["filterCycles"].to<JsonObject>();
  filters["cycle1Start"] = state_.filterCycle1Start;
  filters["cycle1Duration"] = state_.filterCycle1Dur;
  filters["cycle2Enabled"] = state_.filterCycle2Enabled;
  filters["cycle2Start"] = state_.filterCycle2Start;
  filters["cycle2Duration"] = state_.filterCycle2Dur;
  char timeBuffer[6];
  snprintf(timeBuffer, sizeof(timeBuffer), "%02u:%02u", state_.hour, state_.minute);
  out["spaTime"] = timeBuffer;
  out["lastUpdateMs"] = state_.lastUpdateMs;
  out["valid"] = state_.valid;
}

void SpaController::writeCapabilitiesJson(JsonObject out) const {
  out["jet1"] = capabilities_.jet1;
  out["jet2"] = capabilities_.jet2;
  out["light"] = capabilities_.light;
  out["heatMode"] = capabilities_.heatMode;
  out["setTime"] = capabilities_.setTime;
  out["filterCycles"] = capabilities_.filterCycles;
  out["backlight"] = capabilities_.backlight;
}

CommandResult SpaController::handleCommand(JsonVariantConst command) {
  const char *action = command["action"] | "";

  if (benchMode_) {
    return handleBenchCommand(action, command);
  }

  if (strcmp(action, "setTemp") == 0) {
    if (!hasFreshState(millis()) || !isFiniteTemp(state_.setTemp)) {
      return {CommandStatus::SpaStateUnavailable, "current set temperature is not known yet"};
    }
    const int value = command["value"] | -1;
    if (value < 50 || value > 104) {
      return {CommandStatus::InvalidValue, "set temperature must be 50-104 F"};
    }
    const int current = static_cast<int>(roundf(state_.setTemp));
    if (value == current) {
      return {CommandStatus::Accepted, "already at requested set temperature"};
    }
    const uint8_t button = value > current ? ButtonTempUp : ButtonTempDown;
    const int presses = abs(value - current);
    if (queue_.size() + presses > AppConfig::MaxCommandQueue) {
      return {CommandStatus::QueueFull, "command queue is full"};
    }
    for (int i = 0; i < presses; ++i) {
      enqueueButton(value > current ? "temp_up" : "temp_down", button);
    }
    return {CommandStatus::Accepted, "temperature command accepted"};
  }

  if (strcmp(action, "jet1") == 0) {
    return capabilities_.jet1 ? enqueueButton("jet1", ButtonJet1)
                              : CommandResult{CommandStatus::UnsupportedFeature, "jet1 unsupported"};
  }
  if (strcmp(action, "jet2") == 0) {
    return capabilities_.jet2 ? enqueueButton("jet2", ButtonJet2)
                              : CommandResult{CommandStatus::UnsupportedFeature, "jet2 unsupported"};
  }
  if (strcmp(action, "light") == 0) {
    return capabilities_.light ? enqueueButton("light", ButtonLight)
                               : CommandResult{CommandStatus::UnsupportedFeature, "light unsupported"};
  }
  if (strcmp(action, "heatMode") == 0) {
    if (!capabilities_.heatMode) {
      return {CommandStatus::UnsupportedFeature, "heat mode unsupported"};
    }
    const int value = command["value"] | -1;
    if (value < 0 || value > 2) {
      return {CommandStatus::InvalidValue, "heat mode must be 0, 1, or 2"};
    }
    return enqueueButton("heat_mode", ButtonHeatMode);
  }
  if (strcmp(action, "syncTime") == 0) {
    if (!capabilities_.setTime) {
      return {CommandStatus::UnsupportedFeature, "time sync unsupported"};
    }
    time_t now = time(nullptr);
    tm localTime;
    if (now < 100000 || !localtime_r(&now, &localTime)) {
      return {CommandStatus::SpaStateUnavailable, "local time is not synchronized"};
    }
    return enqueue("sync_time", buildSetTimeCommand(localTime.tm_hour, localTime.tm_min));
  }

  return {CommandStatus::InvalidValue, "unknown command action"};
}

void SpaController::updateBenchState() {
  const uint32_t now = millis();
  if (now - lastBenchUpdateMs_ < 1000) {
    return;
  }
  lastBenchUpdateMs_ = now;

  state_.lastUpdateMs = now;
  state_.minute = (state_.minute + 1) % 60;
  if (state_.minute == 0) {
    state_.hour = (state_.hour + 1) % 24;
  }
  if (isFiniteTemp(state_.waterTemp) && isFiniteTemp(state_.setTemp)) {
    if (state_.waterTemp < state_.setTemp - 0.2f) {
      state_.waterTemp += 0.1f;
      state_.heating = true;
    } else {
      state_.heating = false;
    }
  }
  stateChanged_ = true;
}

CommandResult SpaController::handleBenchCommand(const char *action, JsonVariantConst command) {
  if (strcmp(action, "setTemp") == 0) {
    const int value = command["value"] | -1;
    if (value < 50 || value > 104) {
      return {CommandStatus::InvalidValue, "set temperature must be 50-104 F"};
    }
    state_.setTemp = static_cast<float>(value);
    state_.lastUpdateMs = millis();
    stateChanged_ = true;
    Serial.printf("[bench] setTemp=%d\n", value);
    return {CommandStatus::Accepted, "bench set temperature updated"};
  }
  if (strcmp(action, "jet1") == 0) {
    state_.jet1 = !state_.jet1;
    stateChanged_ = true;
    Serial.printf("[bench] jet1=%s\n", state_.jet1 ? "on" : "off");
    return {CommandStatus::Accepted, "bench jet1 toggled"};
  }
  if (strcmp(action, "jet2") == 0) {
    state_.jet2 = !state_.jet2;
    stateChanged_ = true;
    Serial.printf("[bench] jet2=%s\n", state_.jet2 ? "on" : "off");
    return {CommandStatus::Accepted, "bench jet2 toggled"};
  }
  if (strcmp(action, "light") == 0) {
    state_.light = !state_.light;
    stateChanged_ = true;
    Serial.printf("[bench] light=%s\n", state_.light ? "on" : "off");
    return {CommandStatus::Accepted, "bench light toggled"};
  }
  if (strcmp(action, "heatMode") == 0) {
    const int value = command["value"] | -1;
    if (value < 0 || value > 2) {
      return {CommandStatus::InvalidValue, "heat mode must be 0, 1, or 2"};
    }
    state_.heatMode = static_cast<uint8_t>(value);
    stateChanged_ = true;
    Serial.printf("[bench] heatMode=%d\n", value);
    return {CommandStatus::Accepted, "bench heat mode updated"};
  }
  if (strcmp(action, "syncTime") == 0) {
    time_t now = time(nullptr);
    tm localTime;
    if (now >= 100000 && localtime_r(&now, &localTime)) {
      state_.hour = static_cast<uint8_t>(localTime.tm_hour);
      state_.minute = static_cast<uint8_t>(localTime.tm_min);
    }
    stateChanged_ = true;
    Serial.println("[bench] time synced");
    return {CommandStatus::Accepted, "bench time synced"};
  }
  return {CommandStatus::InvalidValue, "unknown command action"};
}

void SpaController::processRx() {
  if (!serial_) {
    return;
  }
  while (serial_->available() > 0) {
    const uint8_t byte = static_cast<uint8_t>(serial_->read());
    auto parsed = parser_.ingest(byte);
    if (!parsed.has_value()) {
      continue;
    }
    if (parsed->frame.has_value()) {
      Serial.printf("[balboa] rx %s\n", bytesToHex(encodeFrame(*parsed->frame)).c_str());
      handleFrame(*parsed->frame);
    } else {
      Serial.printf("[balboa] parse error: %s\n", parseErrorName(parsed->error));
    }
  }
}

void SpaController::processTx() {
  if (!serial_ || queue_.empty()) {
    return;
  }
  const uint32_t now = millis();
  if (now - lastTxMs_ < 120) {
    return;
  }

  PendingCommand command = queue_.front();
  queue_.pop_front();
  Serial.printf("[balboa] tx %s %s\n", command.label.c_str(), bytesToHex(command.frame).c_str());
  transmit(command.frame);
  lastTxMs_ = now;
}

void SpaController::handleFrame(const Frame &frame) {
  if (frame.type == MessageStatusUpdate && decodeStatusFrame(frame)) {
    state_.lastUpdateMs = millis();
    state_.valid = true;
    stateChanged_ = true;
  }
}

bool SpaController::decodeStatusFrame(const Frame &frame) {
  if (frame.payload.size() < 20) {
    return false;
  }

  // These offsets match the common BP-series status frame layout used by open
  // Balboa clients. Serial logs expose raw frames so a real pack can adjust v2.
  state_.hour = frame.payload[1] & 0x1f;
  state_.minute = frame.payload[2] & 0x3f;
  state_.heatMode = frame.payload[3] & 0x03;
  state_.tempRangeHigh = (frame.payload[3] & 0x04) != 0;
  state_.heating = (frame.payload[4] & 0x30) != 0;
  state_.priming = (frame.payload[4] & 0x01) != 0;
  state_.waterTemp = decodeTemp(frame.payload[5]);
  state_.setTemp = decodeTemp(frame.payload[20 < frame.payload.size() ? 20 : frame.payload.size() - 1]);
  state_.jet1 = frame.payload[8] != 0;
  state_.jet2 = frame.payload[9] != 0;
  state_.light = frame.payload[11] != 0;
  return true;
}

CommandResult SpaController::enqueue(const char *label, std::vector<uint8_t> frame) {
  if (queue_.size() >= AppConfig::MaxCommandQueue) {
    return {CommandStatus::QueueFull, "command queue is full"};
  }
  queue_.push_back({std::move(frame), millis(), label});
  return {CommandStatus::Accepted, "command accepted"};
}

CommandResult SpaController::enqueueButton(const char *label, uint8_t buttonCode) {
  return enqueue(label, buildPanelCommand(buttonCode));
}

void SpaController::transmit(const std::vector<uint8_t> &frame) {
  digitalWrite(txEnablePin_, HIGH);
  delayMicroseconds(80);
  serial_->write(frame.data(), frame.size());
  serial_->flush();
  delayMicroseconds(80);
  digitalWrite(txEnablePin_, LOW);
}

const char *commandStatusName(CommandStatus status) {
  switch (status) {
    case CommandStatus::Accepted:
      return "accepted";
    case CommandStatus::InvalidValue:
      return "invalid_value";
    case CommandStatus::UnsupportedFeature:
      return "unsupported_feature";
    case CommandStatus::SpaStateUnavailable:
      return "spa_state_unavailable";
    case CommandStatus::QueueFull:
      return "queue_full";
  }
  return "unknown";
}

}  // namespace Balboa
