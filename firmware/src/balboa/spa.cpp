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

constexpr size_t DebugLogMaxLines = 160;
constexpr size_t StatusCaptureMaxLines = 500;
constexpr uint32_t StateFreshMs = 5000;
constexpr uint32_t PumpSettleMs = 1500;  // several status frames after a press
constexpr uint8_t PumpMaxPresses = 4;
constexpr uint8_t PumpMaxIgnoredPresses = 2;
constexpr uint32_t FaultLogResponseTimeoutMs = 2000;
constexpr uint8_t FaultLogMaxRetries = 2;
constexpr size_t FaultLogMaxEntries = 24;

bool isFiniteTemp(float value) {
  return !isnan(value) && value > 30.0f && value < 120.0f;
}

const char *speedName(uint8_t speed) {
  switch (speed) {
    case 0:
      return "Off";
    case 1:
      return "Low";
    case 2:
      return "High";
  }
  return "?";
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
  DeviceConfiguration equipment;
  equipment.pumpSpeeds[0] = 2;
  equipment.light1 = true;
  equipment_ = equipment;
  capabilities_.detected = true;
  capabilities_.pump1Speeds = 2;
  capabilities_.pump2Speeds = 0;
  capabilities_.light = true;
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
  processPumpCommand();
  processFaultLogRead();
}

bool SpaController::hasFreshState() const {
  return state_.valid && millis() - state_.lastUpdateMs <= StateFreshMs;
}

void SpaController::seedLastWaterTemp(float temp, uint32_t takenAt) {
  if (isnan(lastWaterTemp_) && isFiniteTemp(temp)) {
    lastWaterTemp_ = temp;
    lastWaterTempAt_ = takenAt;
  }
}

void SpaController::setUserCapabilities(bool setTime, bool hideJet2) {
  capabilities_.setTime = setTime;
  hideJet2_ = hideJet2;
  applyEquipmentCapabilities();
  stateChanged_ = true;
}

void SpaController::applyEquipmentCapabilities() {
  if (!equipment_) {
    return;  // keep the defaults until the spa reports its equipment
  }
  capabilities_.detected = true;
  capabilities_.pump1Speeds = equipment_->pumpSpeeds[0];
  capabilities_.pump2Speeds = hideJet2_ ? 0 : equipment_->pumpSpeeds[1];
  capabilities_.light = equipment_->light1;
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
  // Only between readings: with a live reading its time is always "now", and sending that would make every
  // status frame look like a change to the WebSocket broadcast.
  if (isnan(state_.waterTemp) && !isnan(lastWaterTemp_)) {
    out["lastWaterTemp"] = lastWaterTemp_;
    out["lastWaterTempAt"] = lastWaterTempAt_;
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
  out["hold"] = state_.hold;
  out["holdMinutes"] = state_.holdMinutes;
  out["reminder"] = state_.reminder;
  out["activeFilterCycle"] = state_.activeFilterCycle;
  if (setupParams_) {
    JsonObject range = out["setTempRange"].to<JsonObject>();
    range["min"] = state_.tempRangeHigh ? setupParams_->highRangeMin : setupParams_->lowRangeMin;
    range["max"] = state_.tempRangeHigh ? setupParams_->highRangeMax : setupParams_->lowRangeMax;
  }
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
  JsonObject pump = out["pumpCommand"].to<JsonObject>();
  pump["active"] = pumpCommand_.active;
  pump["pump"] = pumpCommand_.pump;
  pump["target"] = pumpCommand_.target;
  pump["seq"] = pumpCommand_.seq;
  pump["ok"] = pumpCommand_.ok;
  pump["message"] = pumpCommand_.message;
  out["spaInfoSeq"] = spaInfoSeq_;
  out["faultLogSeq"] = faultLogRead_.seq;
  out["faultLogReading"] = faultLogRead_.active;
  char timeBuffer[6];
  snprintf(timeBuffer, sizeof(timeBuffer), "%02u:%02u", state_.hour, state_.minute);
  out["spaTime"] = timeBuffer;
  out["valid"] = state_.valid;
  out["statusCaptureActive"] = statusCaptureActive_;
  out["statusCaptureLines"] = statusCaptureLog_.size();
}

void SpaController::writeCapabilitiesJson(JsonObject out) const {
  out["detected"] = capabilities_.detected;
  out["pump2Installed"] = equipment_ && equipment_->pumpSpeeds[1] > 0;  // what the spa reports, before hideJet2
  out["jet1"] = capabilities_.pump1Speeds > 0;
  out["jet2"] = capabilities_.pump2Speeds > 0;
  out["pump1Speeds"] = capabilities_.pump1Speeds;
  out["pump2Speeds"] = capabilities_.pump2Speeds;
  out["light"] = capabilities_.light;
  out["setTime"] = capabilities_.setTime;
}

void SpaController::writeSpaInfoJson(JsonObject out) const {
  if (equipment_) {
    JsonObject equipment = out["equipment"].to<JsonObject>();
    JsonArray pumps = equipment["pumps"].to<JsonArray>();
    for (uint8_t speeds : equipment_->pumpSpeeds) {
      pumps.add(speeds);
    }
    equipment["light1"] = equipment_->light1;
    equipment["light2"] = equipment_->light2;
    equipment["blower"] = equipment_->blower;
    equipment["circulationPump"] = equipment_->circulationPump;
    equipment["aux1"] = equipment_->aux1;
    equipment["aux2"] = equipment_->aux2;
    equipment["mister"] = equipment_->mister;
  }
  if (systemInfo_) {
    JsonObject system = out["system"].to<JsonObject>();
    system["model"] = systemInfo_->model;
    system["softwareVersion"] = systemInfo_->softwareVersion;
    system["setupNumber"] = systemInfo_->setupNumber;
    if (systemInfo_->heaterVoltage) {
      system["heaterVoltage"] = systemInfo_->heaterVoltage;
    }
    char dip[17];
    for (int i = 0; i < 16; ++i) {
      dip[i] = (systemInfo_->dipSwitches >> i) & 0x01 ? '1' : '0';  // position 1 first
    }
    dip[16] = '\0';
    system["dipSwitches"] = dip;
  }
  if (setupParams_) {
    JsonObject ranges = out["tempRanges"].to<JsonObject>();
    ranges["lowMin"] = setupParams_->lowRangeMin;
    ranges["lowMax"] = setupParams_->lowRangeMax;
    ranges["highMin"] = setupParams_->highRangeMin;
    ranges["highMax"] = setupParams_->highRangeMax;
  }
  out["seq"] = spaInfoSeq_;
}

void SpaController::writeFaultLogJson(JsonObject out) const {
  out["reading"] = faultLogRead_.active;
  out["seq"] = faultLogRead_.seq;
  out["total"] = faultLogRead_.total;
  JsonArray entries = out["entries"].to<JsonArray>();
  for (const FaultLogEntry &entry : faultLog_) {
    JsonObject item = entries.add<JsonObject>();
    item["entry"] = entry.entryNumber;
    item["code"] = entry.code;
    item["daysAgo"] = entry.daysAgo;
    item["hour"] = entry.hour;
    item["minute"] = entry.minute;
    item["setTemp"] = entry.setTemp;
    item["sensorA"] = entry.sensorA;
    item["sensorB"] = entry.sensorB;
  }
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
    int minTemp = 50;
    int maxTemp = 104;
    if (setupParams_) {
      minTemp = state_.tempRangeHigh ? setupParams_->highRangeMin : setupParams_->lowRangeMin;
      maxTemp = state_.tempRangeHigh ? setupParams_->highRangeMax : setupParams_->lowRangeMax;
    }
    if (value < minTemp || value > maxTemp) {
      snprintf(validationMessage_, sizeof(validationMessage_), "set temperature must be %d-%d F", minTemp, maxTemp);
      return {CommandStatus::InvalidValue, validationMessage_};
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
    if (startMinute < 0 || startMinute > 45 || startMinute % 15 != 0 || durationMinute < 0 ||
        durationMinute > 45 || durationMinute % 15 != 0) {
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

  if (strcmp(action, "setPump") == 0) {
    const int pump = command["pump"] | 0;
    const int value = command["value"] | -1;
    if (pump < 1 || pump > 2 || pumpSpeeds(pump) == 0) {
      return {CommandStatus::UnsupportedFeature, "this spa does not have that pump"};
    }
    if (value < 0 || value > pumpSpeeds(pump)) {
      return {CommandStatus::InvalidValue, pumpSpeeds(pump) == 1 ? "pump is on/off only" : "pump speed must be 0-2"};
    }
    return startPumpCommand(static_cast<uint8_t>(pump), static_cast<uint8_t>(value));
  }

  if (strcmp(action, "togglePump") == 0) {
    // One press, exactly like the panel's jet button: the spa moves the pump to its next speed.
    const int pump = command["pump"] | 0;
    if (pump < 1 || pump > 2 || pumpSpeeds(pump) == 0) {
      return {CommandStatus::UnsupportedFeature, "this spa does not have that pump"};
    }
    if (pumpCommand_.active) {
      return {CommandStatus::QueueFull, "a pump change is already in progress"};
    }
    return enqueueToggle("pump_toggle", static_cast<uint8_t>(TogglePump1 + pump - 1));
  }

  if (strcmp(action, "light") == 0) {
    if (!capabilities_.light) {
      return {CommandStatus::UnsupportedFeature, "light unsupported"};
    }
    // With a value the light is set exactly; without one it toggles.
    if (command["value"].is<bool>() && command["value"].as<bool>() == state_.light) {
      return {CommandStatus::Accepted, state_.light ? "light already on" : "light already off"};
    }
    return enqueueToggle("light_toggle_item", ToggleLight1);
  }

  if (strcmp(action, "hold") == 0) {
    if (command["value"].is<bool>() && command["value"].as<bool>() == state_.hold) {
      return {CommandStatus::Accepted, state_.hold ? "already in hold" : "not in hold"};
    }
    return enqueueToggle("hold_toggle", ToggleHold);
  }

  if (strcmp(action, "tempRange") == 0) {
    const char *value = command["value"] | "";
    if (strcmp(value, "high") != 0 && strcmp(value, "low") != 0) {
      return {CommandStatus::InvalidValue, "range must be high or low"};
    }
    const bool high = strcmp(value, "high") == 0;
    if (high == state_.tempRangeHigh) {
      return {CommandStatus::Accepted, high ? "already in high range" : "already in low range"};
    }
    return enqueueToggle("temp_range_toggle", ToggleTempRange);
  }

  if (strcmp(action, "clearReminder") == 0) {
    return enqueueToggle("clear_notification", ToggleClearNotification);
  }

  if (strcmp(action, "readFaultLog") == 0) {
    return startFaultLogRead();
  }

  if (strcmp(action, "refreshSpaInfo") == 0) {
    requestSpaInfo();
    return {CommandStatus::Accepted, "spa info requested"};
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

uint8_t SpaController::pumpSpeeds(uint8_t pump) const {
  return pump == 1 ? capabilities_.pump1Speeds : pump == 2 ? capabilities_.pump2Speeds : 0;
}

uint8_t SpaController::pumpSpeed(uint8_t pump) const {
  return pump == 1 ? state_.jet1Speed : state_.jet2Speed;
}

CommandResult SpaController::startPumpCommand(uint8_t pump, uint8_t target) {
  if (pumpCommand_.active) {
    return {CommandStatus::QueueFull, "a pump change is already in progress"};
  }
  if (!hasFreshState()) {
    return {CommandStatus::SpaStateUnavailable, "no recent status from the spa"};
  }
  if (pumpSpeed(pump) == target) {
    return {CommandStatus::Accepted, "pump already at that speed"};
  }
  pumpCommand_.active = true;
  pumpCommand_.pump = pump;
  pumpCommand_.target = target;
  pumpCommand_.startSpeed = pumpSpeed(pump);
  pumpCommand_.lastSpeed = pumpCommand_.startSpeed;
  pumpCommand_.presses = 0;
  pumpCommand_.ignoredPresses = 0;
  pumpCommand_.pressQueued = false;
  pumpCommand_.message[0] = '\0';
  stateChanged_ = true;
  logDebug("[balboa] pump%u target=%s from=%s", pump, speedName(target), speedName(pumpCommand_.startSpeed));
  return {CommandStatus::Accepted, "pump change started"};
}

void SpaController::finishPumpCommand(bool ok, const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(pumpCommand_.message, sizeof(pumpCommand_.message), format, args);
  va_end(args);
  pumpCommand_.active = false;
  pumpCommand_.ok = ok;
  ++pumpCommand_.seq;
  stateChanged_ = true;
  logDebug("[balboa] pump%u %s: %s", pumpCommand_.pump, ok ? "done" : "failed", pumpCommand_.message);
}

void SpaController::processPumpCommand() {
  PumpCommand &cmd = pumpCommand_;
  if (!cmd.active || cmd.pressQueued) {
    return;
  }
  const uint32_t now = millis();
  if (cmd.presses > 0 && (now - cmd.pressSentMs < PumpSettleMs || state_.lastUpdateMs <= cmd.pressSentMs)) {
    return;
  }

  const uint8_t current = pumpSpeed(cmd.pump);
  if (current == cmd.target) {
    finishPumpCommand(true, "Jet %u set to %s", cmd.pump, speedName(cmd.target));
    return;
  }
  if (cmd.presses > 0) {
    if (current == cmd.lastSpeed) {
      // The spa ignored the press; it may have been dropped on the bus.
      if (++cmd.ignoredPresses > PumpMaxIgnoredPresses) {
        finishPumpCommand(false, "The spa did not respond to Jet %u", cmd.pump);
        return;
      }
    } else if (current == cmd.startSpeed) {
      // A full cycle without reaching the target: the spa is holding the pump at certain speeds.
      finishPumpCommand(false, "The spa won't set Jet %u to %s right now (it runs pump 1 for heating and filtering)",
                        cmd.pump, speedName(cmd.target));
      return;
    }
    cmd.lastSpeed = current;
  }
  if (cmd.presses >= PumpMaxPresses) {
    finishPumpCommand(false, "Jet %u stopped at %s", cmd.pump, speedName(current));
    return;
  }
  CommandResult result = enqueueToggle("pump_press", static_cast<uint8_t>(TogglePump1 + cmd.pump - 1), 300);
  if (result.status != CommandStatus::Accepted) {
    finishPumpCommand(false, "Command queue is full");
    return;
  }
  cmd.pressQueued = true;
  ++cmd.presses;
}

CommandResult SpaController::startFaultLogRead() {
  if (faultLogRead_.active) {
    return {CommandStatus::Accepted, "fault log read already in progress"};
  }
  faultLog_.clear();
  faultLogRead_.active = true;
  faultLogRead_.awaiting = false;
  faultLogRead_.next = FaultLogLatestEntry;
  faultLogRead_.total = 0;
  faultLogRead_.retries = 0;
  stateChanged_ = true;
  return {CommandStatus::Accepted, "reading fault log"};
}

void SpaController::processFaultLogRead() {
  FaultLogRead &read = faultLogRead_;
  if (!read.active) {
    return;
  }
  const uint32_t now = millis();
  if (read.awaiting) {
    if (now - read.requestMs < FaultLogResponseTimeoutMs) {
      return;
    }
    read.awaiting = false;
    if (++read.retries > FaultLogMaxRetries) {
      logDebug("[balboa] fault log entry %u timed out, skipping", read.next);
      read.retries = 0;
      if (read.next == FaultLogLatestEntry) {
        read.active = false;  // the spa never answered
        ++read.seq;
        stateChanged_ = true;
        return;
      }
      ++read.next;
    }
  }
  if (read.next != FaultLogLatestEntry && read.next >= read.total) {
    if (read.total == 0) {
      faultLog_.clear();  // an empty log still answers the "latest" request with a placeholder
    }
    read.active = false;
    ++read.seq;
    stateChanged_ = true;
    logDebug("[balboa] fault log read complete entries=%u", static_cast<unsigned>(faultLog_.size()));
    return;
  }
  if (enqueue("read_fault_log", buildSettingsRequest(SettingsFaultLog, read.next)).status == CommandStatus::Accepted) {
    read.awaiting = true;
    read.requestMs = now;
  }
}

void SpaController::applyFaultLogEntry(const FaultLogEntry &entry) {
  FaultLogRead &read = faultLogRead_;
  bool replaced = false;
  for (FaultLogEntry &existing : faultLog_) {
    if (existing.entryNumber == entry.entryNumber) {
      existing = entry;
      replaced = true;
      break;
    }
  }
  if (!replaced && faultLog_.size() < FaultLogMaxEntries) {
    faultLog_.push_back(entry);
  }
  if (!read.active || !read.awaiting) {
    return;
  }
  read.awaiting = false;
  read.retries = 0;
  if (read.next == FaultLogLatestEntry) {
    read.total = entry.totalEntries > FaultLogMaxEntries ? FaultLogMaxEntries : entry.totalEntries;
    read.next = 0;
  } else {
    ++read.next;
  }
}

void SpaController::requestSpaInfo() {
  enqueue("read_configuration", buildSettingsRequest(SettingsConfiguration, 0x00, 0x01));
  enqueue("read_information", buildSettingsRequest(SettingsInformation));
  enqueue("read_setup_parameters", buildSettingsRequest(SettingsSetupParameters));
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
  if (strcmp(action, "setPump") == 0) {
    const int value = command["value"] | -1;
    if ((command["pump"] | 0) != 1 || value < 0 || value > 2) {
      return {CommandStatus::InvalidValue, "bench has one two-speed pump"};
    }
    state_.jet1Speed = static_cast<uint8_t>(value);
    state_.jet1 = value != 0;
    ++pumpCommand_.seq;
    pumpCommand_.ok = true;
    snprintf(pumpCommand_.message, sizeof(pumpCommand_.message), "Jet 1 set to %s", speedName(state_.jet1Speed));
    stateChanged_ = true;
    Serial.printf("[bench] jet1 speed=%u\n", state_.jet1Speed);
    return {CommandStatus::Accepted, "bench pump updated"};
  }
  if (strcmp(action, "togglePump") == 0) {
    if ((command["pump"] | 0) != 1) {
      return {CommandStatus::UnsupportedFeature, "bench has one two-speed pump"};
    }
    state_.jet1Speed = (state_.jet1Speed + 1) % 3;
    state_.jet1 = state_.jet1Speed != 0;
    stateChanged_ = true;
    return {CommandStatus::Accepted, "command accepted"};
  }
  if (strcmp(action, "light") == 0) {
    state_.light = command["value"].is<bool>() ? command["value"].as<bool>() : !state_.light;
    stateChanged_ = true;
    Serial.printf("[bench] light=%s\n", state_.light ? "on" : "off");
    return {CommandStatus::Accepted, "bench light updated"};
  }
  if (strcmp(action, "hold") == 0) {
    state_.hold = command["value"].is<bool>() ? command["value"].as<bool>() : !state_.hold;
    state_.holdMinutes = state_.hold ? 60 : 0;
    stateChanged_ = true;
    return {CommandStatus::Accepted, "bench hold updated"};
  }
  if (strcmp(action, "tempRange") == 0) {
    state_.tempRangeHigh = strcmp(command["value"] | "", "low") != 0;
    stateChanged_ = true;
    return {CommandStatus::Accepted, "bench range updated"};
  }
  if (strcmp(action, "clearReminder") == 0) {
    state_.reminder = 0;
    stateChanged_ = true;
    return {CommandStatus::Accepted, "bench command accepted"};
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
  if (strcmp(action, "readFaultLog") == 0 || strcmp(action, "refreshSpaInfo") == 0) {
    ++faultLogRead_.seq;
    ++spaInfoSeq_;
    stateChanged_ = true;
    return {CommandStatus::Accepted, "bench has no fault log"};
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
      const bool isIdlePanelFrame = parsed->frame->type == MessageToggleItem && parsed->frame->payload.size() == 2 &&
                                    parsed->frame->payload[0] == 0x00 && parsed->frame->payload[1] == 0x00;
      if (parsed->frame->type == MessageToggleItem && !isIdlePanelFrame) {
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
        logStatusCapture("status raw=%s payload=%s water=%.0f set=%.0f time=%02u:%02u heatMode=%u heating=%u heatPending=%u hold=%u reminder=%02x jetSpeed=%u/%u light=%u",
                         bytesToHex(encodeFrame(*parsed->frame)).c_str(),
                         bytesToHex(parsed->frame->payload).c_str(),
                         state_.waterTemp, state_.setTemp, state_.hour, state_.minute, state_.heatMode,
                         state_.heating ? 1 : 0, state_.heatPending ? 1 : 0, state_.hold ? 1 : 0, state_.reminder,
                         state_.jet1Speed, state_.jet2Speed, state_.light ? 1 : 0);
      }
      if (isStatusFrame && rawStatusCaptureCount_ > 0) {
        Serial.printf("[balboa] status raw=%s\n", bytesToHex(encodeFrame(*parsed->frame)).c_str());
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
  if (command.label == "pump_press") {
    pumpCommand_.pressQueued = false;
    pumpCommand_.pressSentMs = now;
  }
  lastTxMs_ = now;
}

void SpaController::handleFrame(const Frame &frame) {
  switch (frame.type) {
    case MessageStatusUpdate:
      if (const auto status = decodeStatusUpdate(frame.payload)) {
        if (!state_.valid && !state_.filterCyclesValid) {
          enqueue("read_filter_cycles_boot", buildFilterCyclesRequest());
          requestSpaInfo();
        }
        applyStatus(*status);
      }
      break;
    case MessageFilterCyclesResponse:
      if (decodeFilterCyclesResponse(frame)) {
        publishFilterCyclesRead();
      }
      break;
    case MessageConfigurationResponse:
      if (const auto equipment = decodeDeviceConfiguration(frame.payload)) {
        equipment_ = equipment;
        applyEquipmentCapabilities();
        ++spaInfoSeq_;
        stateChanged_ = true;
        logDebug("[balboa] equipment pumps=%u/%u/%u/%u/%u/%u light1=%u light2=%u blower=%u circ=%u",
                 equipment->pumpSpeeds[0], equipment->pumpSpeeds[1], equipment->pumpSpeeds[2],
                 equipment->pumpSpeeds[3], equipment->pumpSpeeds[4], equipment->pumpSpeeds[5],
                 equipment->light1, equipment->light2, equipment->blower, equipment->circulationPump);
      }
      break;
    case MessageInformationResponse:
      if (const auto info = decodeSystemInformation(frame.payload)) {
        systemInfo_ = info;
        ++spaInfoSeq_;
        stateChanged_ = true;
        logDebug("[balboa] system model=%s software=%s", info->model.c_str(), info->softwareVersion.c_str());
      }
      break;
    case MessageSetupParametersResponse:
      if (const auto params = decodeSetupParameters(frame.payload)) {
        setupParams_ = params;
        ++spaInfoSeq_;
        stateChanged_ = true;
        logDebug("[balboa] temp ranges low=%u-%u high=%u-%u", params->lowRangeMin, params->lowRangeMax,
                 params->highRangeMin, params->highRangeMax);
      }
      break;
    case MessageFaultLogResponse:
      if (const auto entry = decodeFaultLogEntry(frame.payload)) {
        applyFaultLogEntry(*entry);
        stateChanged_ = true;
      }
      break;
  }
}

void SpaController::applyStatus(const StatusUpdate &status) {
  state_.hour = status.hour;
  state_.minute = status.minute;
  state_.heatMode = status.heatMode;
  state_.tempRangeHigh = status.tempRangeHigh;
  state_.priming = status.initMode == 0x01;
  state_.waterTemp = status.waterTemp;
  if (isFiniteTemp(status.waterTemp)) {
    const time_t now = time(nullptr);
    lastWaterTemp_ = status.waterTemp;
    lastWaterTempAt_ = now > 1700000000 ? static_cast<uint32_t>(now) : 0;
  }
  state_.setTemp = status.setTemp;
  state_.heating = status.heating;
  state_.heatPending = status.heatWaiting;
  state_.jet1Speed = status.pumpSpeed[0];
  state_.jet1 = state_.jet1Speed != 0;
  state_.jet2Speed = status.pumpSpeed[1];
  state_.jet2 = state_.jet2Speed != 0;
  state_.blower = status.blower;
  state_.circulationPump = status.circulationPump;
  state_.light = status.light1;
  state_.hold = status.inHold();
  state_.holdMinutes = state_.hold ? status.holdMinutes : 0;
  state_.reminder = status.reminder;
  // Byte 9 doubles as a menu selector while the panel is in a menu, so only trust it at the main screen.
  if (status.atMainScreen()) {
    state_.activeFilterCycle = status.filter1Running ? 1 : status.filter2Running ? 2 : 0;
  }
  state_.lastUpdateMs = millis();
  state_.valid = true;
  stateChanged_ = true;
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

CommandResult SpaController::enqueueToggle(const char *label, uint8_t itemCode, uint32_t minDelayMs) {
  return enqueue(label, buildToggleItemCommand(itemCode), minDelayMs);
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
