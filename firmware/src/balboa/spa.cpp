#include "balboa/spa.h"

#include <cmath>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <utility>

#include <LittleFS.h>

#include "config.h"

namespace Balboa {
namespace {

constexpr uint8_t ButtonTempUp = 0x01;
constexpr uint8_t ButtonTempDown = 0x02;
constexpr uint8_t ButtonJet1 = 0x04;
constexpr uint8_t ButtonJet2 = 0x05;
constexpr uint8_t ButtonLight = 0x91;
constexpr uint8_t ButtonHeatMode = 0x51;
constexpr uint8_t ToggleLight1 = 0x11;
constexpr uint32_t FilterProgramDebugDelayMs = 650;
constexpr uint32_t FilterProgramEntryDelayMs = 650;
constexpr uint8_t FilterProgramMaxSteps = 80;
constexpr size_t DebugLogMaxLines = 160;
constexpr size_t StatusCaptureMaxLines = 500;
constexpr size_t CommandTraceMaxLines = 420;

enum TargetTempPhase : uint8_t {
  TargetTempIdle = 0,
  TargetTempActivateFlash,
  TargetTempAdjust,
};

enum FilterProgramPhase : uint8_t {
  FilterProgramIdle = 0,
  FilterProgramStartTempFlash,
  FilterProgramNavigate,
  FilterProgramSelectEnable,
  FilterProgramSetEnable,
  FilterProgramSelectBegin,
  FilterProgramAdjustStartHour,
  FilterProgramSelectStartMinute,
  FilterProgramAdjustStartMinute,
  FilterProgramSelectRunHours,
  FilterProgramAdjustRunHour,
  FilterProgramSelectRunMinute,
  FilterProgramAdjustRunMinute,
  FilterProgramShowEnd,
  FilterProgramSave,
};

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

const char *filterPhaseName(uint8_t phase) {
  switch (phase) {
    case FilterProgramIdle:
      return "idle";
    case FilterProgramStartTempFlash:
      return "start_temp_flash";
    case FilterProgramNavigate:
      return "navigate_to_filter";
    case FilterProgramSelectEnable:
      return "select_enable";
    case FilterProgramSetEnable:
      return "set_enable";
    case FilterProgramSelectBegin:
      return "select_begin";
    case FilterProgramAdjustStartHour:
      return "adjust_start_hour";
    case FilterProgramSelectStartMinute:
      return "select_start_minute";
    case FilterProgramAdjustStartMinute:
      return "adjust_start_minute";
    case FilterProgramSelectRunHours:
      return "select_run_hours";
    case FilterProgramAdjustRunHour:
      return "adjust_run_hour";
    case FilterProgramSelectRunMinute:
      return "select_run_minute";
    case FilterProgramAdjustRunMinute:
      return "adjust_run_minute";
    case FilterProgramShowEnd:
      return "show_f1_ends";
    case FilterProgramSave:
      return "save";
  }
  return "unknown";
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
  processTargetTemp();
  processFilterProgram();
}

bool SpaController::hasFreshState(uint32_t nowMs) const {
  return state_.valid && nowMs - state_.lastUpdateMs <= AppConfig::StateStaleMs;
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
  state_.lastUpdateMs = millis();
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
  char timeBuffer[6];
  snprintf(timeBuffer, sizeof(timeBuffer), "%02u:%02u", state_.hour, state_.minute);
  out["spaTime"] = timeBuffer;
  out["lastUpdateMs"] = state_.lastUpdateMs;
  out["valid"] = state_.valid;
  out["filterProgramActive"] = filterProgramActive_;
  out["filterProgramReadOnly"] = filterProgramReadOnly_;
  out["filterProgramFailed"] = filterProgramLastFailed_;
  out["filterProgramError"] = filterProgramLastError_;
  out["statusCaptureActive"] = statusCaptureActive_;
  out["statusCaptureLines"] = statusCaptureLog_.size();
  if (targetTempActive_) {
    out["targetSetTemp"] = targetSetTemp_;
  } else {
    out["targetSetTemp"] = nullptr;
  }
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

bool SpaController::consumeFilterCycleCache(FilterCycleCache &out) {
  if (!filterCycleCachePending_) {
    return false;
  }
  out = filterCycleCache_;
  filterCycleCachePending_ = false;
  return true;
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
  appendCommandTrace(line);
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

void SpaController::startCommandTrace(const char *name) {
  commandTraceActive_ = true;
  commandTraceName_ = name;
  commandTraceStartedMs_ = millis();
  commandTraceLog_.clear();
  filterProgramLastFailed_ = false;
  filterProgramLastError_ = "";
  String line = String(commandTraceStartedMs_) + " command trace started name=" + commandTraceName_;
  commandTraceLog_.push_back(line);
}

void SpaController::appendCommandTrace(const String &line) {
  if (!commandTraceActive_) {
    return;
  }
  commandTraceLog_.push_back(line);
  while (commandTraceLog_.size() > CommandTraceMaxLines) {
    commandTraceLog_.pop_front();
  }
}

String SpaController::commandTraceTimestamp() const {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm localTime;
    localtime_r(&now, &localTime);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &localTime);
    return String(timestamp);
  }
  return String("uptime-") + String(commandTraceStartedMs_);
}

void SpaController::saveCommandTraceFailure(const char *reason) {
  if (!LittleFS.exists("/command-logs")) {
    LittleFS.mkdir("/command-logs");
  }
  const String path = String("/command-logs/") + commandTraceName_ + "-" + commandTraceTimestamp() + ".log";
  File file = LittleFS.open(path, "w");
  if (!file) {
    Serial.printf("[balboa] command trace save failed path=%s\n", path.c_str());
    return;
  }
  file.printf("name=%s\nreason=%s\nstarted_ms=%lu\nsaved_ms=%lu\n\n",
              commandTraceName_.c_str(), reason, static_cast<unsigned long>(commandTraceStartedMs_),
              static_cast<unsigned long>(millis()));
  for (const String &line : commandTraceLog_) {
    file.println(line);
  }
  file.close();
  Serial.printf("[balboa] command trace saved path=%s reason=%s\n", path.c_str(), reason);
}

void SpaController::finishCommandTrace(bool success, const char *reason) {
  if (!commandTraceActive_) {
    return;
  }
  const String line = String(millis()) + " command trace finished status=" + (success ? "success" : "failure") +
                      " reason=" + reason;
  commandTraceLog_.push_back(line);
  if (!success) {
    saveCommandTraceFailure(reason);
  }
  filterProgramLastFailed_ = !success;
  filterProgramLastError_ = success ? "" : reason;
  commandTraceActive_ = false;
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

  if (strcmp(action, "tempUp") == 0) {
    targetTempActive_ = false;
    targetTempWaitingForStatus_ = false;
    targetTempPhase_ = TargetTempIdle;
    filterProgramActive_ = false;
    filterProgramWaiting_ = false;
    filterProgramQueued_ = false;
    filterProgramWaitForEditChange_ = false;
    filterProgramReadOnly_ = false;
    stateChanged_ = true;
    return enqueueButton("temp_up", ButtonTempUp);
  }
  if (strcmp(action, "tempDown") == 0) {
    targetTempActive_ = false;
    targetTempWaitingForStatus_ = false;
    targetTempPhase_ = TargetTempIdle;
    filterProgramActive_ = false;
    filterProgramWaiting_ = false;
    filterProgramQueued_ = false;
    filterProgramWaitForEditChange_ = false;
    filterProgramReadOnly_ = false;
    stateChanged_ = true;
    return enqueueButton("temp_down", ButtonTempDown);
  }

  if (strcmp(action, "setTemp") == 0) {
    const int value = command["value"] | -1;
    if (value < 50 || value > 104) {
      return {CommandStatus::InvalidValue, "set temperature must be 50-104 F"};
    }
    targetTempActive_ = false;
    targetTempWaitingForStatus_ = false;
    targetTempPhase_ = TargetTempIdle;
    filterProgramActive_ = false;
    filterProgramWaiting_ = false;
    filterProgramQueued_ = false;
    filterProgramWaitForEditChange_ = false;
    filterProgramReadOnly_ = false;
    stateChanged_ = true;
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
    if (filterProgramActive_) {
      return {CommandStatus::QueueFull, "filter programming already active"};
    }
    if (!queue_.empty()) {
      return {CommandStatus::QueueFull, "command queue must be empty before filter programming"};
    }

    targetTempActive_ = false;
    targetTempWaitingForStatus_ = false;
    targetTempPhase_ = TargetTempIdle;
    filterProgramReadOnly_ = false;
    filterProgramCycle_ = isFilter2 ? 2 : 1;
    filterProgramEnabled_ = enabled;
    filterProgramStartHour_ = static_cast<uint8_t>(enabled ? startHour : 0);
    filterProgramStartMinute_ = static_cast<uint8_t>(startMinute);
    filterProgramDurationHour_ = static_cast<uint8_t>(enabled ? durationHour : 0);
    filterProgramDurationMinute_ = static_cast<uint8_t>(durationMinute);
    filterProgramActive_ = true;
    filterProgramWaiting_ = false;
    filterProgramQueued_ = false;
    filterProgramWaitForEditChange_ = false;
    filterProgramHomeLightSent_ = false;
    filterProgramPhase_ = FilterProgramStartTempFlash;
    filterProgramStepCount_ = 0;
    stateChanged_ = true;
    startCommandTrace(isFilter2 ? "SetFilter2" : "SetFilter1");
    logDebug("[balboa] filter%u target enabled=%u start=%02u:%02u duration=%02u:%02u", filterProgramCycle_,
             filterProgramEnabled_ ? 1 : 0, filterProgramStartHour_, filterProgramStartMinute_,
             filterProgramDurationHour_, filterProgramDurationMinute_);
    return {CommandStatus::Accepted, isFilter2 ? "filter 2 programming started" : "filter 1 programming started"};
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
  if (strcmp(action, "tempUp") == 0) {
    if (isFiniteTemp(state_.setTemp) && state_.setTemp < 104.0f) {
      state_.setTemp += 1.0f;
    }
    stateChanged_ = true;
    Serial.printf("[bench] temp_up setTemp=%.0f\n", state_.setTemp);
    return {CommandStatus::Accepted, "bench temperature raised"};
  }
  if (strcmp(action, "tempDown") == 0) {
    if (isFiniteTemp(state_.setTemp) && state_.setTemp > 50.0f) {
      state_.setTemp -= 1.0f;
    }
    stateChanged_ = true;
    Serial.printf("[bench] temp_down setTemp=%.0f\n", state_.setTemp);
    return {CommandStatus::Accepted, "bench temperature lowered"};
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
    stateChanged_ = true;
    Serial.printf("[bench] filter1 start=%u:%02u duration=%u:%02u\n", state_.filterCycle1Start,
                  state_.filterCycle1StartMinute, state_.filterCycle1Dur, state_.filterCycle1DurMinute);
    return {CommandStatus::Accepted, "bench filter 1 updated"};
  }
  if (strcmp(action, "readFilterCycles") == 0) {
    filterCycleCache_.all = true;
    filterCycleCache_.cycle1Start = state_.filterCycle1Start;
    filterCycleCache_.cycle1StartMinute = state_.filterCycle1StartMinute;
    filterCycleCache_.cycle1Duration = state_.filterCycle1Dur;
    filterCycleCache_.cycle1DurationMinute = state_.filterCycle1DurMinute;
    filterCycleCache_.cycle2Enabled = state_.filterCycle2Enabled;
    filterCycleCache_.cycle2Start = state_.filterCycle2Start;
    filterCycleCache_.cycle2StartMinute = state_.filterCycle2StartMinute;
    filterCycleCache_.cycle2Duration = state_.filterCycle2Dur;
    filterCycleCache_.cycle2DurationMinute = state_.filterCycle2DurMinute;
    filterCycleCachePending_ = true;
    state_.filterCyclesValid = true;
    stateChanged_ = true;
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
    stateChanged_ = true;
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
        if (!filterProgramActive_) {
          rawStatusCaptureCount_ = 4;
        }
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
      if (isStatusFrame) {
        const uint8_t p0 = parsed->frame->payload.size() > 0 ? parsed->frame->payload[0] : 0;
        const uint8_t p1 = parsed->frame->payload.size() > 1 ? parsed->frame->payload[1] : 0;
        const uint8_t p7 = parsed->frame->payload.size() > 7 ? parsed->frame->payload[7] : 0;
        const uint8_t p8 = parsed->frame->payload.size() > 8 ? parsed->frame->payload[8] : 0;
        const uint8_t p9 = parsed->frame->payload.size() > 9 ? parsed->frame->payload[9] : 0;
        if (commandTraceActive_) {
          const std::string raw = bytesToHex(encodeFrame(*parsed->frame));
          const std::string payload = bytesToHex(parsed->frame->payload);
          String line = String(millis()) + " status raw=" + raw.c_str() + " payload=" + payload.c_str();
          appendCommandTrace(line);
        }
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
  if (command.label.startsWith("target_")) {
    targetTempWaitingForStatus_ = true;
    targetTempStatusMs_ = state_.lastUpdateMs;
    targetTempCommandMs_ = now;
  }
  if (command.label.startsWith("filter_")) {
    filterProgramQueued_ = false;
    filterProgramWaiting_ = true;
    filterProgramStatusMs_ = state_.lastUpdateMs;
    filterProgramCommandMs_ = now;
    filterProgramWaitForEditChange_ = command.label == "filter_start_hour" ||
                                      command.label == "filter_start_minute" ||
                                      command.label == "filter_run_hour" ||
                                      command.label == "filter_run_minute";
    filterProgramWaitHour_ = editValueHour_;
    filterProgramWaitMinute_ = editValueMinute_;
  }
  lastTxMs_ = now;
}

void SpaController::processFilterProgram() {
  if (!filterProgramActive_) {
    return;
  }

  const uint32_t now = millis();
  if (filterProgramQueued_) {
    return;
  }

  uint32_t waitTimeoutMs = 1800;
  if (filterProgramPhase_ == FilterProgramStartTempFlash) {
    waitTimeoutMs = 250;
  }
  const uint8_t targetSelector = filterProgramCycle_ == 2 ? 0x08 : 0x04;
  const bool onStartHourPrompt = menuMajor_ == 0x06 && menuMinor_ == 0x0b;
  const bool onStartHourEdit = menuMajor_ == 0x04 && menuMinor_ == 0x07;
  const bool onStartMinuteEdit = menuMajor_ == 0x04 && menuMinor_ == 0x08;
  const bool onRunHoursPrompt = menuMajor_ == 0x06 && menuMinor_ == 0x0d;
  const bool onFilterEndPrompt = menuMajor_ == 0x06 && menuMinor_ == 0x0c;
  const bool onRunHourEdit = menuMajor_ == 0x07 && menuMinor_ == 0x07;
  const bool onRunMinuteEdit = menuMajor_ == 0x07 && menuMinor_ == 0x08;
  const bool onFilterMenu = menuMajor_ == 0x03 && menuMinor_ == 0x17 && filterMenuSelector_ == targetSelector;
  const bool onFilter2EnableOff = filterProgramCycle_ == 2 && menuMajor_ == 0x06 && menuMinor_ == 0x0e &&
                                  filterMenuSelector_ == targetSelector;
  const bool onFilter2EnableOn = filterProgramCycle_ == 2 && menuMajor_ == 0x06 && menuMinor_ == 0x0f &&
                                 filterMenuSelector_ == targetSelector;
  const bool onFilter2EnableScreen = onFilter2EnableOff || onFilter2EnableOn;
  const bool filter2EnableMatched = filterProgramEnabled_ ? onFilter2EnableOn : onFilter2EnableOff;
  const auto filterPhaseReachedExpectedScreen = [&]() {
    switch (filterProgramPhase_) {
      case FilterProgramNavigate:
        return onFilterMenu;
      case FilterProgramSelectEnable:
        return onFilter2EnableScreen;
      case FilterProgramSetEnable:
        return filter2EnableMatched;
      case FilterProgramSelectBegin:
        return onStartHourPrompt || onStartHourEdit;
      case FilterProgramSelectStartMinute:
        return onStartMinuteEdit;
      case FilterProgramSelectRunHours:
        return onRunHoursPrompt || onRunHourEdit;
      case FilterProgramSelectRunMinute:
        return onRunMinuteEdit;
      case FilterProgramShowEnd:
        return onFilterEndPrompt;
      default:
        return true;
    }
  };
  if (filterProgramWaiting_) {
    const bool statusChanged = state_.lastUpdateMs != filterProgramStatusMs_;
    const bool editChanged = !filterProgramWaitForEditChange_ ||
                             editValueHour_ != filterProgramWaitHour_ ||
                             editValueMinute_ != filterProgramWaitMinute_;
    const bool timedOut = now - filterProgramCommandMs_ >= waitTimeoutMs;
    if ((!statusChanged || !filterPhaseReachedExpectedScreen() || !editChanged) && !timedOut) {
      return;
    }
    logDebug("[balboa] filter%u phase complete phase=%s menu=%02x/%02x selector=%02x edit=%02u:%02u statusChanged=%u editChanged=%u elapsed=%lu",
             filterProgramCycle_, filterPhaseName(filterProgramPhase_), menuMajor_, menuMinor_, filterMenuSelector_,
             editValueHour_, editValueMinute_, statusChanged ? 1 : 0, editChanged ? 1 : 0,
             static_cast<unsigned long>(now - filterProgramCommandMs_));
    filterProgramWaiting_ = false;
    filterProgramWaitForEditChange_ = false;
    logDebug("[balboa] filter%u screen flags targetSelector=%02x filterMenu=%u f2Enable=%u f2Off=%u f2On=%u startPrompt=%u startHour=%u startMin=%u runPrompt=%u endPrompt=%u runHour=%u runMin=%u targetEnabled=%u readOnly=%u",
             filterProgramCycle_, targetSelector, onFilterMenu ? 1 : 0, onFilter2EnableScreen ? 1 : 0,
             onFilter2EnableOff ? 1 : 0, onFilter2EnableOn ? 1 : 0,
             onStartHourPrompt ? 1 : 0, onStartHourEdit ? 1 : 0, onStartMinuteEdit ? 1 : 0,
             onRunHoursPrompt ? 1 : 0, onFilterEndPrompt ? 1 : 0, onRunHourEdit ? 1 : 0, onRunMinuteEdit ? 1 : 0,
             filterProgramEnabled_ ? 1 : 0, filterProgramReadOnly_ ? 1 : 0);

    if (filterProgramPhase_ == FilterProgramStartTempFlash) {
      filterProgramPhase_ = FilterProgramNavigate;
    } else if (filterProgramPhase_ == FilterProgramNavigate) {
      if (onFilterMenu) {
        filterProgramPhase_ = filterProgramCycle_ == 2 ? FilterProgramSelectEnable : FilterProgramSelectBegin;
      }
    } else if (filterProgramPhase_ == FilterProgramSelectEnable) {
      if (onFilter2EnableScreen) {
        filterProgramPhase_ = FilterProgramSetEnable;
      }
    } else if (filterProgramPhase_ == FilterProgramSetEnable) {
      if (filter2EnableMatched && !filterProgramEnabled_) {
        filterProgramPhase_ = FilterProgramSave;
      } else if (filter2EnableMatched) {
        filterProgramPhase_ = FilterProgramSelectBegin;
      }
    } else if (filterProgramPhase_ == FilterProgramSelectBegin) {
      if (onStartHourPrompt || onStartHourEdit) {
        filterProgramPhase_ = FilterProgramAdjustStartHour;
      }
    } else if (filterProgramPhase_ == FilterProgramSelectStartMinute) {
      if (onStartMinuteEdit) {
        filterProgramPhase_ = FilterProgramAdjustStartMinute;
      }
    } else if (filterProgramPhase_ == FilterProgramSelectRunHours) {
      if (onRunHoursPrompt || onRunHourEdit) {
        filterProgramPhase_ = FilterProgramAdjustRunHour;
      }
    } else if (filterProgramPhase_ == FilterProgramSelectRunMinute) {
      if (onRunMinuteEdit) {
        filterProgramPhase_ = FilterProgramAdjustRunMinute;
      }
    } else if (filterProgramPhase_ == FilterProgramShowEnd) {
      if (onFilterEndPrompt) {
        filterProgramPhase_ = FilterProgramSave;
      }
    } else if (filterProgramPhase_ == FilterProgramSave) {
      filterProgramActive_ = false;
      filterProgramReadOnly_ = false;
      filterProgramHomeLightSent_ = false;
      filterProgramWaitForEditChange_ = false;
      stateChanged_ = true;
      logDebug("[balboa] filter%u programming complete", filterProgramCycle_);
      finishCommandTrace(true, "complete");
      return;
    }
    logDebug("[balboa] filter%u next phase=%s menu=%02x/%02x selector=%02x edit=%02u:%02u",
             filterProgramCycle_, filterPhaseName(filterProgramPhase_), menuMajor_, menuMinor_, filterMenuSelector_,
             editValueHour_, editValueMinute_);
  }

  if (!filterProgramActive_ || !queue_.empty()) {
    return;
  }

  if (filterProgramStepCount_ > FilterProgramMaxSteps) {
    logDebug("[balboa] filter%u abort: step limit phase=%s menu=%02x/%02x selector=%02x edit=%02u:%02u",
             filterProgramCycle_, filterPhaseName(filterProgramPhase_), menuMajor_, menuMinor_, filterMenuSelector_,
             editValueHour_, editValueMinute_);
    filterProgramActive_ = false;
    filterProgramWaiting_ = false;
    filterProgramQueued_ = false;
    filterProgramWaitForEditChange_ = false;
    filterProgramReadOnly_ = false;
    filterProgramHomeLightSent_ = false;
    stateChanged_ = true;
    finishCommandTrace(false, "step limit");
    return;
  }

  const auto abortFilterProgram = [this](const char *reason) {
    logDebug("[balboa] filter%u abort: %s phase=%s menu=%02x/%02x selector=%02x edit=%02u:%02u",
             filterProgramCycle_, reason, filterPhaseName(filterProgramPhase_), menuMajor_, menuMinor_,
             filterMenuSelector_, editValueHour_, editValueMinute_);
    filterProgramActive_ = false;
    filterProgramWaiting_ = false;
    filterProgramQueued_ = false;
    filterProgramWaitForEditChange_ = false;
    filterProgramReadOnly_ = false;
    filterProgramHomeLightSent_ = false;
    stateChanged_ = true;
    finishCommandTrace(false, reason);
  };

  const auto sendFilterButton = [this, now](const char *label, uint8_t button, uint32_t minDelayMs = FilterProgramDebugDelayMs) {
    logDebug("[balboa] filter%u send phase=%s label=%s button=%02x menu=%02x/%02x selector=%02x edit=%02u:%02u targetEnabled=%u readOnly=%u targetStart=%02u:%02u targetRun=%02u:%02u delay=%lu",
             filterProgramCycle_, filterPhaseName(filterProgramPhase_), label, button, menuMajor_, menuMinor_,
             filterMenuSelector_, editValueHour_, editValueMinute_, filterProgramEnabled_ ? 1 : 0,
             filterProgramReadOnly_ ? 1 : 0,
             filterProgramStartHour_, filterProgramStartMinute_, filterProgramDurationHour_,
             filterProgramDurationMinute_, static_cast<unsigned long>(minDelayMs));
    CommandResult result = enqueueButton(label, button, minDelayMs);
    if (result.status == CommandStatus::Accepted) {
      filterProgramQueued_ = true;
      ++filterProgramStepCount_;
    } else {
      logDebug("[balboa] filter%u enqueue failed status=%s", filterProgramCycle_, commandStatusName(result.status));
    }
  };

  switch (filterProgramPhase_) {
    case FilterProgramStartTempFlash:
      sendFilterButton("filter_temp_flash", ButtonTempDown, FilterProgramEntryDelayMs);
      break;
    case FilterProgramNavigate:
      if (menuMajor_ == 0x03 && menuMinor_ == 0x17 &&
          filterMenuSelector_ == (filterProgramCycle_ == 2 ? 0x08 : 0x04)) {
        filterProgramHomeLightSent_ = false;
        filterProgramPhase_ = filterProgramCycle_ == 2 ? FilterProgramSelectEnable : FilterProgramSelectBegin;
      } else if (menuMajor_ == 0x00 && menuMinor_ == 0x00) {
        if (filterProgramHomeLightSent_) {
          filterProgramHomeLightSent_ = false;
          sendFilterButton("filter_temp_flash_retry", ButtonTempDown, FilterProgramEntryDelayMs);
        } else {
          filterProgramHomeLightSent_ = true;
          sendFilterButton("filter_nav_from_home", ButtonLight, FilterProgramDebugDelayMs);
        }
      } else {
        filterProgramHomeLightSent_ = false;
        const uint32_t delay = menuMajor_ == 0x03 && menuMinor_ == 0x10 ? FilterProgramEntryDelayMs : FilterProgramDebugDelayMs;
        sendFilterButton("filter_nav", ButtonLight, delay);
      }
      break;
    case FilterProgramSelectEnable:
      logDebug("[balboa] filter2 select enable screen menu=%02x/%02x selector=%02x edit=%02u:%02u",
               menuMajor_, menuMinor_, filterMenuSelector_, editValueHour_, editValueMinute_);
      sendFilterButton("filter_enable_select", ButtonTempUp);
      break;
    case FilterProgramSetEnable:
      logDebug("[balboa] filter2 set enable desired=%u menu=%02x/%02x selector=%02x edit=%02u:%02u",
               filterProgramEnabled_ ? 1 : 0, menuMajor_, menuMinor_, filterMenuSelector_, editValueHour_,
               editValueMinute_);
      if (filterProgramReadOnly_ && menuMajor_ == 0x06 && filterMenuSelector_ == 0x08 &&
          (menuMinor_ == 0x0e || menuMinor_ == 0x0f)) {
        filterCycleCache_.cycle = 2;
        filterCycleCache_.enabled = menuMinor_ == 0x0f;
        state_.filterCycle2Enabled = filterCycleCache_.enabled;
        if (!filterCycleCache_.enabled) {
          filterCycleCache_.startHour = state_.filterCycle2Start;
          filterCycleCache_.startMinute = state_.filterCycle2StartMinute;
          filterCycleCache_.durationHour = state_.filterCycle2Dur;
          filterCycleCache_.durationMinute = state_.filterCycle2DurMinute;
          filterCycleCachePending_ = true;
          stateChanged_ = true;
          logDebug("[balboa] filter2 read complete enabled=0");
          filterProgramPhase_ = FilterProgramSave;
        } else {
          logDebug("[balboa] filter2 read enabled=1");
          filterProgramPhase_ = FilterProgramSelectBegin;
        }
        break;
      }
      if (menuMajor_ == 0x06 && filterMenuSelector_ == 0x08 &&
          ((filterProgramEnabled_ && menuMinor_ == 0x0f) || (!filterProgramEnabled_ && menuMinor_ == 0x0e))) {
        logDebug("[balboa] filter2 enable already matched desired=%u", filterProgramEnabled_ ? 1 : 0);
        filterProgramPhase_ = filterProgramEnabled_ ? FilterProgramSelectBegin : FilterProgramSave;
      } else if (filterProgramEnabled_) {
        sendFilterButton("filter_enable_on", ButtonTempUp);
      } else {
        sendFilterButton("filter_enable_off", ButtonTempDown);
      }
      break;
    case FilterProgramSelectBegin:
      sendFilterButton("filter_begin", filterProgramCycle_ == 2 ? ButtonLight : ButtonTempUp);
      break;
    case FilterProgramAdjustStartHour:
      if (menuMajor_ == 0x06 && menuMinor_ == 0x0b) {
        sendFilterButton("filter_start_hour_enter", ButtonTempUp);
        break;
      }
      if (menuMajor_ != 0x04 || menuMinor_ != 0x07) {
        abortFilterProgram("expected start-hour edit screen");
        break;
      }
      if (filterProgramReadOnly_) {
        filterCycleCache_.cycle = filterProgramCycle_;
        filterCycleCache_.enabled = true;
        filterCycleCache_.startHour = editValueHour_;
        logDebug("[balboa] filter%u read start hour=%u", filterProgramCycle_, filterCycleCache_.startHour);
        filterProgramPhase_ = FilterProgramSelectStartMinute;
        break;
      }
      if (editValueHour_ == filterProgramStartHour_) {
        logDebug("[balboa] filter%u matched start hour=%u", filterProgramCycle_, editValueHour_);
        filterProgramPhase_ = FilterProgramSelectStartMinute;
      } else {
        sendFilterButton("filter_start_hour", editValueHour_ < filterProgramStartHour_ ? ButtonTempUp : ButtonTempDown);
      }
      break;
    case FilterProgramSelectStartMinute:
      sendFilterButton("filter_start_minute_select", ButtonLight);
      break;
    case FilterProgramAdjustStartMinute:
      if (menuMajor_ != 0x04 || menuMinor_ != 0x08) {
        abortFilterProgram("expected start-minute edit screen");
        break;
      }
      if (filterProgramReadOnly_) {
        filterCycleCache_.startMinute = editValueMinute_;
        logDebug("[balboa] filter%u read start minute=%u", filterProgramCycle_, filterCycleCache_.startMinute);
        filterProgramPhase_ = FilterProgramSelectRunHours;
        break;
      }
      if (editValueMinute_ == filterProgramStartMinute_) {
        logDebug("[balboa] filter%u matched start minute=%u", filterProgramCycle_, editValueMinute_);
        filterProgramPhase_ = FilterProgramSelectRunHours;
      } else {
        sendFilterButton("filter_start_minute",
                         editValueMinute_ < filterProgramStartMinute_ ? ButtonTempUp : ButtonTempDown);
      }
      break;
    case FilterProgramSelectRunHours:
      sendFilterButton("filter_run_hour_select", ButtonLight);
      break;
    case FilterProgramAdjustRunHour:
      if (menuMajor_ == 0x06 && menuMinor_ == 0x0d) {
        sendFilterButton("filter_run_hour_enter", ButtonTempUp);
        break;
      }
      if (menuMajor_ != 0x07 || menuMinor_ != 0x07) {
        abortFilterProgram("expected run-hour edit screen");
        break;
      }
      if (filterProgramReadOnly_) {
        filterCycleCache_.durationHour = editValueHour_;
        logDebug("[balboa] filter%u read run hour=%u", filterProgramCycle_, filterCycleCache_.durationHour);
        filterProgramPhase_ = FilterProgramSelectRunMinute;
        break;
      }
      if (editValueHour_ == filterProgramDurationHour_) {
        logDebug("[balboa] filter%u matched run hour=%u", filterProgramCycle_, editValueHour_);
        filterProgramPhase_ = FilterProgramSelectRunMinute;
      } else {
        sendFilterButton("filter_run_hour", editValueHour_ < filterProgramDurationHour_ ? ButtonTempUp : ButtonTempDown);
      }
      break;
    case FilterProgramSelectRunMinute:
      sendFilterButton("filter_run_minute_select", ButtonLight);
      break;
    case FilterProgramAdjustRunMinute:
      if (menuMajor_ != 0x07 || menuMinor_ != 0x08) {
        abortFilterProgram("expected run-minute edit screen");
        break;
      }
      if (filterProgramReadOnly_) {
        filterCycleCache_.durationMinute = editValueMinute_;
        if (filterProgramCycle_ == 1) {
          state_.filterCycle1Start = filterCycleCache_.startHour;
          state_.filterCycle1StartMinute = filterCycleCache_.startMinute;
          state_.filterCycle1Dur = filterCycleCache_.durationHour;
          state_.filterCycle1DurMinute = filterCycleCache_.durationMinute;
        } else {
          state_.filterCycle2Enabled = true;
          state_.filterCycle2Start = filterCycleCache_.startHour;
          state_.filterCycle2StartMinute = filterCycleCache_.startMinute;
          state_.filterCycle2Dur = filterCycleCache_.durationHour;
          state_.filterCycle2DurMinute = filterCycleCache_.durationMinute;
        }
        filterCycleCachePending_ = true;
        stateChanged_ = true;
        logDebug("[balboa] filter%u read complete enabled=%u start=%02u:%02u run=%02u:%02u",
                 filterProgramCycle_, filterCycleCache_.enabled ? 1 : 0,
                 filterCycleCache_.startHour, filterCycleCache_.startMinute,
                 filterCycleCache_.durationHour, filterCycleCache_.durationMinute);
        filterProgramPhase_ = FilterProgramShowEnd;
        break;
      }
      if (editValueMinute_ == filterProgramDurationMinute_) {
        logDebug("[balboa] filter%u matched run minute=%u", filterProgramCycle_, editValueMinute_);
        filterProgramPhase_ = FilterProgramShowEnd;
      } else {
        sendFilterButton("filter_run_minute",
                         editValueMinute_ < filterProgramDurationMinute_ ? ButtonTempUp : ButtonTempDown);
      }
      break;
    case FilterProgramShowEnd:
      sendFilterButton("filter_show_end", ButtonLight);
      break;
    case FilterProgramSave:
      sendFilterButton("filter_save", ButtonLight);
      break;
    default:
      filterProgramActive_ = false;
      filterProgramReadOnly_ = false;
      filterProgramWaitForEditChange_ = false;
      stateChanged_ = true;
      finishCommandTrace(false, "unknown phase");
      break;
  }
}

void SpaController::processTargetTemp() {
  if (!targetTempActive_) {
    return;
  }
  const uint32_t now = millis();
  if (!hasFreshState(now) || !isFiniteTemp(state_.setTemp)) {
    return;
  }

  const int current = static_cast<int>(roundf(state_.setTemp));
  if (current == targetSetTemp_) {
    targetTempActive_ = false;
    targetTempWaitingForStatus_ = false;
    targetTempPhase_ = TargetTempIdle;
    stateChanged_ = true;
    Serial.printf("[balboa] target temp reached=%d\n", targetSetTemp_);
    return;
  }

  if (!queue_.empty()) {
    return;
  }

  if (targetTempWaitingForStatus_) {
    if (state_.lastUpdateMs == targetTempStatusMs_ && now - targetTempCommandMs_ < 2500) {
      return;
    }
    targetTempWaitingForStatus_ = false;
  }

  const bool raise = targetSetTemp_ > current;
  const bool activateFlash = targetTempPhase_ == TargetTempActivateFlash;
  const char *label = activateFlash ? (raise ? "target_temp_flash_up" : "target_temp_flash_down")
                                    : (raise ? "target_temp_up" : "target_temp_down");
  const uint8_t button = raise ? ButtonTempUp : ButtonTempDown;
  CommandResult result = enqueueButton(label, button);
  if (result.status == CommandStatus::Accepted) {
    if (activateFlash) {
      targetTempPhase_ = TargetTempAdjust;
      Serial.printf("[balboa] target temp flash current=%d target=%d direction=%s\n", current, targetSetTemp_,
                    raise ? "up" : "down");
    } else {
      Serial.printf("[balboa] target step current=%d target=%d direction=%s\n", current, targetSetTemp_,
                    raise ? "up" : "down");
    }
  }
}

void SpaController::handleFrame(const Frame &frame) {
  if (frame.type == MessageStatusUpdate && decodeStatusFrame(frame)) {
    state_.lastUpdateMs = millis();
    state_.valid = true;
    stateChanged_ = true;
  } else if (frame.type == MessageFilterCyclesResponse && decodeFilterCyclesResponse(frame)) {
    stateChanged_ = true;
  }
}

bool SpaController::decodeStatusFrame(const Frame &frame) {
  if (frame.payload.size() <= 20) {
    return false;
  }

  // These offsets match the common BP-series status frame layout used by open
  // Balboa clients. Serial logs expose raw frames so a real pack can adjust v2.
  state_.hour = frame.payload[3] & 0x1f;
  state_.minute = frame.payload[4] & 0x3f;
  state_.heatMode = frame.payload[3] & 0x03;
  state_.tempRangeHigh = (frame.payload[3] & 0x04) != 0;
  state_.priming = (frame.payload[4] & 0x01) != 0;
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
  editValueHour_ = frame.payload[7];
  editValueMinute_ = frame.payload[8];
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
  state_.filterCyclesValid = true;

  filterCycleCache_.all = true;
  filterCycleCache_.cycle1Start = cycle1Start;
  filterCycleCache_.cycle1StartMinute = cycle1StartMinute;
  filterCycleCache_.cycle1Duration = cycle1Duration;
  filterCycleCache_.cycle1DurationMinute = cycle1DurationMinute;
  filterCycleCache_.cycle2Enabled = cycle2Enabled;
  filterCycleCache_.cycle2Start = cycle2Start;
  filterCycleCache_.cycle2StartMinute = cycle2StartMinute;
  filterCycleCache_.cycle2Duration = cycle2Duration;
  filterCycleCache_.cycle2DurationMinute = cycle2DurationMinute;
  filterCycleCachePending_ = true;

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
