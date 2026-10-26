#include "balboa/spa.h"

#include <cmath>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <utility>

#include "config.h"

namespace Balboa {
namespace {

constexpr uint8_t ButtonJet1 = 0x04;
constexpr uint8_t ButtonJet2 = 0x05;
constexpr uint8_t ToggleLight1 = 0x11;
constexpr size_t DebugLogMaxLines = 160;
constexpr size_t StatusCaptureMaxLines = 500;

bool isFiniteTemp(float value) {
  return !isnan(value) && value > 30.0f && value < 120.0f;
}

float decodeTemp(uint8_t raw) {
  if (raw == 0xff || raw == 0x00) {
    return NAN;
  }
  return static_cast<float>(raw);
}

bool shouldLogStatus(const SpaState &state) {
  static int lastWaterTemp = -1;
  static int lastSetTemp = -1;
  static uint8_t lastHour = 0xff;
  static uint8_t lastMinute = 0xff;
  static uint8_t lastHeatMode = 0xff;
  static bool lastHeating = false;
  static bool lastHeatPending = false;
  static bool lastJet1 = false;
  static bool lastJet2 = false;
  static uint8_t lastJet1Speed = 0xff;
  static uint8_t lastJet2Speed = 0xff;
  static bool lastLight = false;
  static bool initialized = false;

  const int waterTemp = isFiniteTemp(state.waterTemp) ? static_cast<int>(roundf(state.waterTemp)) : -1;
  const int setTemp = isFiniteTemp(state.setTemp) ? static_cast<int>(roundf(state.setTemp)) : -1;
  const bool changed = !initialized || waterTemp != lastWaterTemp || setTemp != lastSetTemp ||
                       state.hour != lastHour || state.minute != lastMinute ||
                       state.heatMode != lastHeatMode || state.heating != lastHeating ||
                       state.heatPending != lastHeatPending ||
                       state.jet1 != lastJet1 || state.jet2 != lastJet2 ||
                       state.jet1Speed != lastJet1Speed || state.jet2Speed != lastJet2Speed ||
                       state.light != lastLight;
  if (changed) {
    initialized = true;
    lastWaterTemp = waterTemp;
    lastSetTemp = setTemp;
    lastHour = state.hour;
    lastMinute = state.minute;
    lastHeatMode = state.heatMode;
    lastHeating = state.heating;
    lastHeatPending = state.heatPending;
    lastJet1 = state.jet1;
    lastJet2 = state.jet2;
    lastJet1Speed = state.jet1Speed;
    lastJet2Speed = state.jet2Speed;
    lastLight = state.light;
  }
  return changed;
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
  state_.filterCycle1StartMinute = 0;
  state_.filterCycle1Dur = 2;
  state_.filterCycle1DurMinute = 0;
  state_.filterCycle2Enabled = false;
  state_.filterCycle2Start = 20;
  state_.filterCycle2StartMinute = 0;
  state_.filterCycle2Dur = 0;
  state_.filterCycle2DurMinute = 0;
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

bool SpaController::consumeStateChanged() {
  const bool changed = stateChanged_;
  stateChanged_ = false;
  return changed;
}

void SpaController::setConfiguredFilterCycles(uint8_t cycle1Start, uint8_t cycle1StartMinute,
                                              uint8_t cycle1Duration, uint8_t cycle1DurationMinute,
                                              bool cycle2Enabled,
                                              uint8_t cycle2Start, uint8_t cycle2StartMinute,
                                              uint8_t cycle2Duration, uint8_t cycle2DurationMinute) {
  state_.filterCycle1Start = cycle1Start;
  state_.filterCycle1StartMinute = cycle1StartMinute;
  state_.filterCycle1Dur = cycle1Duration;
  state_.filterCycle1DurMinute = cycle1DurationMinute;
  state_.filterCycle2Enabled = cycle2Enabled;
  state_.filterCycle2Start = cycle2Start;
  state_.filterCycle2StartMinute = cycle2StartMinute;
  state_.filterCycle2Dur = cycle2Duration;
  state_.filterCycle2DurMinute = cycle2DurationMinute;
  stateChanged_ = true;
  Serial.printf("[balboa] cached filter cycles c1=%u:%02u/%u:%02u c2=%s %u:%02u/%u:%02u\n", cycle1Start,
                cycle1StartMinute, cycle1Duration, cycle1DurationMinute, cycle2Enabled ? "on" : "off",
                cycle2Start, cycle2StartMinute, cycle2Duration, cycle2DurationMinute);
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
  out["jet1Speed"] = state_.jet1Speed;
  out["jet2Speed"] = state_.jet2Speed;
  out["blower"] = state_.blower;
  out["light"] = state_.light;
  out["heatMode"] = state_.heatMode;
  out["heating"] = state_.heating;
  out["heatPending"] = state_.heatPending;
  out["priming"] = state_.priming;
  out["tempRangeHigh"] = state_.tempRangeHigh;
  out["activeFilterCycle"] = state_.activeFilterCycle;
  JsonObject filters = out["filterCycles"].to<JsonObject>();
  filters["cycle1Start"] = state_.filterCycle1Start;
  filters["cycle1StartMinute"] = state_.filterCycle1StartMinute;
  filters["cycle1Duration"] = state_.filterCycle1Dur;
  filters["cycle1DurationMinute"] = state_.filterCycle1DurMinute;
  filters["cycle2Enabled"] = state_.filterCycle2Enabled;
  filters["cycle2Start"] = state_.filterCycle2Start;
  filters["cycle2StartMinute"] = state_.filterCycle2StartMinute;
  filters["cycle2Duration"] = state_.filterCycle2Dur;
  filters["cycle2DurationMinute"] = state_.filterCycle2DurMinute;
  filters["valid"] = state_.filterCyclesValid;
  filters["seq"] = filterCyclesSeq_;
  char timeBuffer[6];
  snprintf(timeBuffer, sizeof(timeBuffer), "%02u:%02u", state_.hour, state_.minute);
  out["spaTime"] = timeBuffer;
  out["lastUpdateMs"] = state_.lastUpdateMs;
  out["valid"] = state_.valid;
  out["statusCaptureActive"] = statusCaptureActive_;
  out["statusCaptureLines"] = statusCaptureLog_.size();
}

void SpaController::writeCapabilitiesJson(JsonObject out) const {
  out["jet1"] = capabilities_.jet1;
  out["jet2"] = capabilities_.jet2;
  out["light"] = capabilities_.light;
  out["setTime"] = capabilities_.setTime;
  out["backlight"] = capabilities_.backlight;
}

String SpaController::debugLogText() const {
  String out;
  for (const String &line : debugLog_) {
    out += line;
    out += '\n';
  }
  return out;
}

void SpaController::clearDebugLog() {
  debugLog_.clear();
}

void SpaController::startStatusCapture() {
  statusCaptureActive_ = true;
  statusCaptureLog_.clear();
  logStatusCapture("capture started");
}

void SpaController::stopStatusCapture() {
  logStatusCapture("capture stopped");
  statusCaptureActive_ = false;
}

void SpaController::clearStatusCapture() {
  statusCaptureLog_.clear();
}

String SpaController::statusCaptureText() const {
  String out;
  for (const String &line : statusCaptureLog_) {
    out += line;
    out += '\n';
  }
  return out;
}

bool SpaController::consumeFilterCyclesRead() {
  const bool pending = filterCyclesReadPending_;
  filterCyclesReadPending_ = false;
  return pending;
}

void SpaController::publishFilterCyclesRead() {
  state_.filterCyclesValid = true;
  ++filterCyclesSeq_;
  filterCyclesReadPending_ = true;
  stateChanged_ = true;
}

void SpaController::logDebug(const char *format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  char line[300];
  snprintf(line, sizeof(line), "%lu %s", static_cast<unsigned long>(millis()), message);
  Serial.println(line);
  debugLog_.push_back(line);
  while (debugLog_.size() > DebugLogMaxLines) {
    debugLog_.pop_front();
  }
}

void SpaController::logStatusCapture(const char *format, ...) {
  if (!statusCaptureActive_) {
    return;
  }

  char message[360];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  char line[420];
  snprintf(line, sizeof(line), "%lu %s", static_cast<unsigned long>(millis()), message);
  statusCaptureLog_.push_back(line);
  while (statusCaptureLog_.size() > StatusCaptureMaxLines) {
    statusCaptureLog_.pop_front();
  }
}

CommandResult SpaController::handleCommand(JsonVariantConst command) {
  const char *action = command["action"] | "";

  if (strcmp(action, "startStatusCapture") == 0) {
    startStatusCapture();
    stateChanged_ = true;
    return {CommandStatus::Accepted, "status capture started"};
  }
  if (strcmp(action, "stopStatusCapture") == 0) {
    stopStatusCapture();
    stateChanged_ = true;
    return {CommandStatus::Accepted, "status capture stopped"};
  }
  if (strcmp(action, "clearStatusCapture") == 0) {
    clearStatusCapture();
    stateChanged_ = true;
    return {CommandStatus::Accepted, "status capture cleared"};
  }

  if (benchMode_) {
    return handleBenchCommand(action, command);
  }

  if (strcmp(action, "setTemp") == 0) {
    const int value = command["value"] | -1;
    if (value < 50 || value > 104) {
      return {CommandStatus::InvalidValue, "set temperature must be 50-104 F"};
    }
    Serial.printf("[balboa] direct target temp=%d\n", value);
    return enqueue("set_temp_direct", buildSetTemperatureCommand(static_cast<uint8_t>(value)));
  }

  if (strcmp(action, "setFilter1") == 0 || strcmp(action, "setFilter2") == 0) {
    const bool isFilter2 = strcmp(action, "setFilter2") == 0;
    const bool enabled = !isFilter2 || (command["enabled"] | false);
    const int startHour = command["startHour"] | -1;
    const int startMinute = command["startMinute"] | 0;
    const int durationHour = command["durationHour"] | -1;
    const int durationMinute = command["durationMinute"] | 0;
    if (enabled && (startHour < 0 || startHour > 23 || durationHour < 0 || durationHour > 24)) {
      return {CommandStatus::InvalidValue, "filter hours must be in range"};
    }
    if (enabled && (startMinute < 0 || startMinute > 45 || startMinute % 15 != 0 || durationMinute < 0 ||
                    durationMinute > 45 || durationMinute % 15 != 0)) {
      return {CommandStatus::InvalidValue, "filter minutes must be 0, 15, 30, or 45"};
    }
    if (!enabled && (startMinute < 0 || startMinute > 45 || startMinute % 15 != 0 || durationMinute < 0 ||
                     durationMinute > 45 || durationMinute % 15 != 0)) {
      return {CommandStatus::InvalidValue, "filter minutes must be 0, 15, 30, or 45"};
    }
    // The set command writes both cycles, so the untouched one must come from the spa, not a stale cache.
    if (!state_.filterCyclesValid) {
      enqueue("read_filter_cycles", buildFilterCyclesRequest());
      return {CommandStatus::SpaStateUnavailable, "filter cycles not read from spa yet, try again"};
    }

    uint8_t cycle1Start = state_.filterCycle1Start;
    uint8_t cycle1StartMinute = state_.filterCycle1StartMinute;
    uint8_t cycle1Duration = state_.filterCycle1Dur;
    uint8_t cycle1DurationMinute = state_.filterCycle1DurMinute;
    bool cycle2Enabled = state_.filterCycle2Enabled;
    uint8_t cycle2Start = state_.filterCycle2Start;
    uint8_t cycle2StartMinute = state_.filterCycle2StartMinute;
    uint8_t cycle2Duration = state_.filterCycle2Dur;
    uint8_t cycle2DurationMinute = state_.filterCycle2DurMinute;
    if (isFilter2) {
      cycle2Enabled = enabled;
      cycle2Start = static_cast<uint8_t>(enabled ? startHour : 0);
      cycle2StartMinute = static_cast<uint8_t>(startMinute);
      cycle2Duration = static_cast<uint8_t>(enabled ? durationHour : 0);
      cycle2DurationMinute = static_cast<uint8_t>(durationMinute);
    } else {
      cycle1Start = static_cast<uint8_t>(startHour);
      cycle1StartMinute = static_cast<uint8_t>(startMinute);
      cycle1Duration = static_cast<uint8_t>(durationHour);
      cycle1DurationMinute = static_cast<uint8_t>(durationMinute);
    }

    logDebug("[balboa] direct filter set c1=%02u:%02u/%02u:%02u c2=%u %02u:%02u/%02u:%02u",
             cycle1Start, cycle1StartMinute, cycle1Duration, cycle1DurationMinute,
             cycle2Enabled ? 1 : 0, cycle2Start, cycle2StartMinute, cycle2Duration, cycle2DurationMinute);
    CommandResult result = enqueue("set_filter_cycles_direct",
                                   buildSetFilterCyclesCommand(cycle1Start, cycle1StartMinute,
                                                               cycle1Duration, cycle1DurationMinute,
                                                               cycle2Enabled, cycle2Start, cycle2StartMinute,
                                                               cycle2Duration, cycle2DurationMinute));
    if (result.status == CommandStatus::Accepted) {
      enqueue("read_filter_cycles_confirm", buildFilterCyclesRequest(), 900);
    }
    return result;
  }

  if (strcmp(action, "readFilterCycles") == 0) {
    return enqueue("read_filter_cycles", buildFilterCyclesRequest());
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
    return capabilities_.light ? enqueue("light_toggle_item", buildToggleItemCommand(ToggleLight1))
                               : CommandResult{CommandStatus::UnsupportedFeature, "light unsupported"};
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
      state_.heatPending = false;
    } else {
      state_.heating = false;
      state_.heatPending = false;
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
    state_.jet1Speed = (state_.jet1Speed + 1) % 3;
    state_.jet1 = state_.jet1Speed != 0;
    stateChanged_ = true;
    Serial.printf("[bench] jet1 speed=%u\n", state_.jet1Speed);
    return {CommandStatus::Accepted, "bench jet1 toggled"};
  }
  if (strcmp(action, "jet2") == 0) {
    state_.jet2Speed = state_.jet2 ? 0 : 1;
    state_.jet2 = state_.jet2Speed != 0;
    stateChanged_ = true;
    Serial.printf("[bench] jet2 speed=%u\n", state_.jet2Speed);
    return {CommandStatus::Accepted, "bench jet2 toggled"};
  }
  if (strcmp(action, "light") == 0) {
    state_.light = !state_.light;
    stateChanged_ = true;
    Serial.printf("[bench] light=%s\n", state_.light ? "on" : "off");
    return {CommandStatus::Accepted, "bench light toggled"};
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
  if (strcmp(action, "setFilter1") == 0) {
    const int startHour = command["startHour"] | -1;
    const int durationHour = command["durationHour"] | -1;
    if (startHour < 0 || startHour > 23 || durationHour < 0 || durationHour > 24) {
      return {CommandStatus::InvalidValue, "filter hours must be in range"};
    }
    state_.filterCycle1Start = static_cast<uint8_t>(startHour);
    state_.filterCycle1StartMinute = static_cast<uint8_t>(command["startMinute"] | 0);
    state_.filterCycle1Dur = static_cast<uint8_t>(durationHour);
    state_.filterCycle1DurMinute = static_cast<uint8_t>(command["durationMinute"] | 0);
    publishFilterCyclesRead();
    Serial.printf("[bench] filter1 start=%u:%02u duration=%u:%02u\n", state_.filterCycle1Start,
                  state_.filterCycle1StartMinute, state_.filterCycle1Dur, state_.filterCycle1DurMinute);
    return {CommandStatus::Accepted, "bench filter 1 updated"};
  }
  if (strcmp(action, "readFilterCycles") == 0) {
    publishFilterCyclesRead();
    Serial.println("[bench] filter cycles read");
    return {CommandStatus::Accepted, "bench filter cycles read"};
  }
  if (strcmp(action, "setFilter2") == 0) {
    const bool enabled = command["enabled"] | false;
    const int startHour = command["startHour"] | -1;
    const int durationHour = command["durationHour"] | -1;
    if (enabled && (startHour < 0 || startHour > 23 || durationHour < 0 || durationHour > 24)) {
      return {CommandStatus::InvalidValue, "filter hours must be in range"};
    }
    state_.filterCycle2Enabled = enabled;
    state_.filterCycle2Start = static_cast<uint8_t>(enabled ? startHour : 0);
    state_.filterCycle2StartMinute = static_cast<uint8_t>(command["startMinute"] | 0);
    state_.filterCycle2Dur = static_cast<uint8_t>(enabled ? durationHour : 0);
    state_.filterCycle2DurMinute = static_cast<uint8_t>(command["durationMinute"] | 0);
    publishFilterCyclesRead();
    Serial.printf("[bench] filter2 enabled=%u start=%u:%02u duration=%u:%02u\n", state_.filterCycle2Enabled ? 1 : 0,
                  state_.filterCycle2Start, state_.filterCycle2StartMinute, state_.filterCycle2Dur,
                  state_.filterCycle2DurMinute);
    return {CommandStatus::Accepted, "bench filter 2 updated"};
  }
  return {CommandStatus::InvalidValue, "unknown command action"};
}

void SpaController::processRx() {
  if (!serial_) {
    return;
  }
  while (serial_->available() > 0) {
    const uint8_t byte = static_cast<uint8_t>(serial_->read());
#ifdef BP100G2_RAW_BUS_LOG
    Serial.printf("[bus] raw %02x\n", byte);
#endif
    auto parsed = parser_.ingest(byte);
    if (!parsed.has_value()) {
      continue;
    }
    if (parsed->frame.has_value()) {
      const bool isStatusFrame = parsed->frame->type == MessageStatusUpdate;
      const bool isPollFrame = parsed->frame->type == 0x06 && parsed->frame->payload.empty();
      const bool isDiscoveryFrame = parsed->frame->type == 0x00 && parsed->frame->payload.empty();
      const bool isIdlePanelFrame = parsed->frame->type == MessagePanelCommand && parsed->frame->payload.size() == 2 &&
                                    parsed->frame->payload[0] == 0x00 && parsed->frame->payload[1] == 0x00;
      if (parsed->frame->type == MessagePanelCommand && !isIdlePanelFrame) {
        Serial.printf("[balboa] panel cmd t=%lu src=%02x dst=%02x payload=%s wire=%s\n",
                      static_cast<unsigned long>(millis()), parsed->frame->source, parsed->frame->target,
                      bytesToHex(parsed->frame->payload).c_str(),
                      bytesToHex(encodeFrame(*parsed->frame)).c_str());
        logStatusCapture("panel src=%02x dst=%02x payload=%s wire=%s",
                         parsed->frame->source, parsed->frame->target,
                         bytesToHex(parsed->frame->payload).c_str(),
                         bytesToHex(encodeFrame(*parsed->frame)).c_str());
        rawStatusCaptureCount_ = 4;
      } else if (!isPollFrame && !isDiscoveryFrame && !isIdlePanelFrame && !isStatusFrame) {
        Serial.printf("[balboa] rx nonstatus type=%02x src=%02x dst=%02x payload=%s wire=%s\n",
                      parsed->frame->type, parsed->frame->source, parsed->frame->target,
                      bytesToHex(parsed->frame->payload).c_str(),
                      bytesToHex(encodeFrame(*parsed->frame)).c_str());
        logStatusCapture("nonstatus type=%02x src=%02x dst=%02x payload=%s wire=%s",
                         parsed->frame->type, parsed->frame->source, parsed->frame->target,
                         bytesToHex(parsed->frame->payload).c_str(),
                         bytesToHex(encodeFrame(*parsed->frame)).c_str());
      }
      handleFrame(*parsed->frame);
      // Guarded here because the hex-string arguments allocate even when capture is off.
      if (isStatusFrame && statusCaptureActive_) {
        const uint8_t p0 = parsed->frame->payload.size() > 0 ? parsed->frame->payload[0] : 0;
        const uint8_t p1 = parsed->frame->payload.size() > 1 ? parsed->frame->payload[1] : 0;
        const uint8_t p7 = parsed->frame->payload.size() > 7 ? parsed->frame->payload[7] : 0;
        const uint8_t p8 = parsed->frame->payload.size() > 8 ? parsed->frame->payload[8] : 0;
        const uint8_t p9 = parsed->frame->payload.size() > 9 ? parsed->frame->payload[9] : 0;
        logStatusCapture("status raw=%s payload=%s menu=%02x/%02x selector=%02x edit=%02u:%02u water=%.0f set=%.0f time=%02u:%02u heatMode=%u heating=%u heatPending=%u jets=%u/%u jetSpeed=%u/%u light=%u",
                         bytesToHex(encodeFrame(*parsed->frame)).c_str(),
                         bytesToHex(parsed->frame->payload).c_str(),
                         p0, p1, p9, p7, p8, state_.waterTemp, state_.setTemp,
                         state_.hour, state_.minute, state_.heatMode, state_.heating ? 1 : 0,
                         state_.heatPending ? 1 : 0,
                         state_.jet1 ? 1 : 0, state_.jet2 ? 1 : 0,
                         state_.jet1Speed, state_.jet2Speed, state_.light ? 1 : 0);
      }
      if (isStatusFrame && rawStatusCaptureCount_ > 0) {
        Serial.printf("[balboa] status raw=%s\n", bytesToHex(encodeFrame(*parsed->frame)).c_str());
        if (parsed->frame->payload.size() > 8) {
          Serial.printf("[balboa] edit value hour=%u minute=%u menu=%02x/%02x selector=%02x\n",
                        parsed->frame->payload[7], parsed->frame->payload[8],
                        parsed->frame->payload[0], parsed->frame->payload[1],
                        parsed->frame->payload[9]);
        }
        --rawStatusCaptureCount_;
      }
      if (isStatusFrame && state_.valid && shouldLogStatus(state_)) {
        Serial.printf("[balboa] status water=%.0f set=%.0f time=%02u:%02u heatMode=%u heating=%u heatPending=%u jets=%u/%u jetSpeed=%u/%u light=%u\n",
                      state_.waterTemp, state_.setTemp, state_.hour, state_.minute,
                      state_.heatMode, state_.heating ? 1 : 0, state_.heatPending ? 1 : 0, state_.jet1 ? 1 : 0,
                      state_.jet2 ? 1 : 0, state_.jet1Speed, state_.jet2Speed, state_.light ? 1 : 0);
      }
    } else {
      Serial.printf("[balboa] parse error: %s wire=%s reported_len=%u actual_len=%u expected_crc=%02x actual_crc=%02x\n",
                    parseErrorName(parsed->error), bytesToHex(parsed->wireBytes).c_str(),
                    static_cast<unsigned>(parsed->reportedLength),
                    static_cast<unsigned>(parsed->actualLength), parsed->expectedCrc,
                    parsed->actualCrc);
      logStatusCapture("parse_error name=%s wire=%s reported_len=%u actual_len=%u expected_crc=%02x actual_crc=%02x",
                       parseErrorName(parsed->error), bytesToHex(parsed->wireBytes).c_str(),
                       static_cast<unsigned>(parsed->reportedLength),
                       static_cast<unsigned>(parsed->actualLength), parsed->expectedCrc,
                       parsed->actualCrc);
    }
  }
}

void SpaController::processTx() {
  if (!serial_ || queue_.empty()) {
    return;
  }
  const uint32_t now = millis();
  if (now - lastTxMs_ < queue_.front().minDelayMs) {
    return;
  }

  PendingCommand command = queue_.front();
  queue_.pop_front();
  Serial.printf("[balboa] tx %s %s\n", command.label.c_str(), bytesToHex(command.frame).c_str());
  transmit(command.frame);
  Serial.println("[balboa] tx complete");
  lastTxMs_ = now;
}

void SpaController::handleFrame(const Frame &frame) {
  if (frame.type == MessageStatusUpdate && decodeStatusFrame(frame)) {
    if (!state_.valid && !state_.filterCyclesValid) {
      enqueue("read_filter_cycles_boot", buildFilterCyclesRequest());
    }
    state_.lastUpdateMs = millis();
    state_.valid = true;
    stateChanged_ = true;
  } else if (frame.type == MessageFilterCyclesResponse && decodeFilterCyclesResponse(frame)) {
    publishFilterCyclesRead();
  }
}

bool SpaController::decodeStatusFrame(const Frame &frame) {
  if (frame.payload.size() <= 20) {
    return false;
  }

  // Offsets follow the BWA status layout (github.com/ccutrer/balboa_worldwide_app/wiki, pybalboa).
  state_.hour = frame.payload[3] & 0x1f;
  state_.minute = frame.payload[4] & 0x3f;
  const uint8_t heatMode = frame.payload[5] & 0x03;
  state_.heatMode = heatMode == 3 ? 2 : heatMode;  // packs report ready-in-rest as 2 or 3
  state_.tempRangeHigh = (frame.payload[10] & 0x04) != 0;
  state_.priming = frame.payload[1] == 0x01;
  state_.waterTemp = decodeTemp(frame.payload[2]);
  state_.setTemp = decodeTemp(frame.payload[20]);
  state_.heating = (frame.payload[10] & 0x10) != 0;
  state_.heatPending = !state_.heating && (frame.payload[10] & 0x20) != 0;
  state_.jet1Speed = frame.payload[11] & 0x03;
  state_.jet1 = state_.jet1Speed != 0;
  state_.jet2Speed = 0;
  state_.jet2 = false;
  state_.light = frame.payload[14] != 0;
  menuMajor_ = frame.payload[0];
  menuMinor_ = frame.payload[1];
  filterMenuSelector_ = frame.payload[9];
  if (menuMajor_ == 0x00 && menuMinor_ == 0x00 && filterMenuSelector_ == 0x04) {
    state_.activeFilterCycle = 1;
  } else if (menuMajor_ == 0x00 && menuMinor_ == 0x00 && filterMenuSelector_ == 0x08) {
    state_.activeFilterCycle = 2;
  } else if (menuMajor_ == 0x00 && menuMinor_ == 0x00) {
    state_.activeFilterCycle = 0;
  }
  return true;
}

bool SpaController::decodeFilterCyclesResponse(const Frame &frame) {
  if (frame.payload.size() < 8) {
    return false;
  }

  const uint8_t cycle2StartRaw = frame.payload[4];
  const bool cycle2Enabled = (cycle2StartRaw & 0x80) != 0;
  const uint8_t cycle2Start = cycle2StartRaw & 0x7f;
  const uint8_t cycle1Start = frame.payload[0];
  const uint8_t cycle1StartMinute = frame.payload[1];
  const uint8_t cycle1Duration = frame.payload[2];
  const uint8_t cycle1DurationMinute = frame.payload[3];
  const uint8_t cycle2StartMinute = frame.payload[5];
  const uint8_t cycle2Duration = frame.payload[6];
  const uint8_t cycle2DurationMinute = frame.payload[7];
  if (cycle1Start > 23 || cycle1StartMinute > 45 || cycle1StartMinute % 15 != 0 ||
      cycle1Duration > 24 || cycle1DurationMinute > 45 || cycle1DurationMinute % 15 != 0 ||
      cycle2Start > 23 || cycle2StartMinute > 45 || cycle2StartMinute % 15 != 0 ||
      cycle2Duration > 24 || cycle2DurationMinute > 45 || cycle2DurationMinute % 15 != 0) {
    logDebug("[balboa] invalid filter cycles response payload=%s", bytesToHex(frame.payload).c_str());
    return false;
  }

  state_.filterCycle1Start = cycle1Start;
  state_.filterCycle1StartMinute = cycle1StartMinute;
  state_.filterCycle1Dur = cycle1Duration;
  state_.filterCycle1DurMinute = cycle1DurationMinute;
  state_.filterCycle2Enabled = cycle2Enabled;
  state_.filterCycle2Start = cycle2Start;
  state_.filterCycle2StartMinute = cycle2StartMinute;
  state_.filterCycle2Dur = cycle2Duration;
  state_.filterCycle2DurMinute = cycle2DurationMinute;

  Serial.printf("[balboa] filter cycles response c1=%u:%02u/%u:%02u c2=%s %u:%02u/%u:%02u\n",
                cycle1Start, cycle1StartMinute, cycle1Duration, cycle1DurationMinute,
                cycle2Enabled ? "on" : "off", cycle2Start, cycle2StartMinute,
                cycle2Duration, cycle2DurationMinute);
  return true;
}

CommandResult SpaController::enqueue(const char *label, std::vector<uint8_t> frame, uint32_t minDelayMs) {
  if (queue_.size() >= AppConfig::MaxCommandQueue) {
    return {CommandStatus::QueueFull, "command queue is full"};
  }
  queue_.push_back({std::move(frame), millis(), minDelayMs, label});
  return {CommandStatus::Accepted, "command accepted"};
}

CommandResult SpaController::enqueueButton(const char *label, uint8_t buttonCode, uint32_t minDelayMs) {
  return enqueue(label, buildPanelCommand(buttonCode), minDelayMs);
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
