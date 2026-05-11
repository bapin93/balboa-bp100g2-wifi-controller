#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <deque>
#include <vector>

#include "balboa/protocol.h"

namespace Balboa {

struct SpaCapabilities {
  bool jet1 = true;
  bool jet2 = false;
  bool light = true;
  bool heatMode = true;
  bool setTime = true;
  bool filterCycles = false;
  bool backlight = false;
};

struct SpaState {
  float waterTemp = NAN;
  float setTemp = NAN;
  bool jet1 = false;
  bool jet2 = false;
  uint8_t jet1Speed = 0;
  uint8_t jet2Speed = 0;
  bool blower = false;
  bool light = false;
  uint8_t heatMode = 0;
  bool heating = false;
  bool heatPending = false;
  bool priming = false;
  bool tempRangeHigh = true;
  uint8_t activeFilterCycle = 0;
  uint8_t filterCycle1Start = 0;
  uint8_t filterCycle1StartMinute = 0;
  uint8_t filterCycle1Dur = 0;
  uint8_t filterCycle1DurMinute = 0;
  uint8_t filterCycle2Start = 0;
  uint8_t filterCycle2StartMinute = 0;
  uint8_t filterCycle2Dur = 0;
  uint8_t filterCycle2DurMinute = 0;
  bool filterCycle2Enabled = false;
  uint8_t backlightTimeout = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint32_t lastUpdateMs = 0;
  bool valid = false;
};

enum class CommandStatus {
  Accepted,
  InvalidValue,
  UnsupportedFeature,
  SpaStateUnavailable,
  QueueFull,
};

struct CommandResult {
  CommandStatus status;
  const char *message;
};

struct FilterCycleCache {
  uint8_t cycle = 1;
  bool enabled = true;
  uint8_t startHour = 0;
  uint8_t startMinute = 0;
  uint8_t durationHour = 0;
  uint8_t durationMinute = 0;
};

class SpaController {
 public:
  void begin(HardwareSerial &serial, int txEnablePin);
  void loop();
  void enableBenchMode();

  const SpaState &state() const { return state_; }
  const SpaCapabilities &capabilities() const { return capabilities_; }
  void setCapabilities(const SpaCapabilities &capabilities) { capabilities_ = capabilities; }
  void setConfiguredFilterCycles(uint8_t cycle1Start, uint8_t cycle1StartMinute, uint8_t cycle1Duration,
                                 uint8_t cycle1DurationMinute, bool cycle2Enabled,
                                 uint8_t cycle2Start, uint8_t cycle2StartMinute, uint8_t cycle2Duration,
                                 uint8_t cycle2DurationMinute);
  bool hasFreshState(uint32_t nowMs) const;

  bool consumeStateChanged();
  void writeStateJson(JsonObject out) const;
  void writeCapabilitiesJson(JsonObject out) const;
  String debugLogText() const;
  void clearDebugLog();
  void startStatusCapture();
  void stopStatusCapture();
  void clearStatusCapture();
  String statusCaptureText() const;
  bool statusCaptureActive() const { return statusCaptureActive_; }
  size_t statusCaptureSize() const { return statusCaptureLog_.size(); }
  bool consumeFilterCycleCache(FilterCycleCache &out);

  CommandResult handleCommand(JsonVariantConst command);

 private:
  struct PendingCommand {
    std::vector<uint8_t> frame;
    uint32_t queuedAtMs;
    uint32_t minDelayMs;
    String label;
  };

  void processRx();
  void processTx();
  void processTargetTemp();
  void processFilterProgram();
  void updateBenchState();
  CommandResult handleBenchCommand(const char *action, JsonVariantConst command);
  void handleFrame(const Frame &frame);
  bool decodeStatusFrame(const Frame &frame);
  CommandResult enqueue(const char *label, std::vector<uint8_t> frame, uint32_t minDelayMs = 550);
  CommandResult enqueueButton(const char *label, uint8_t buttonCode, uint32_t minDelayMs = 550);
  void transmit(const std::vector<uint8_t> &frame);
  void logDebug(const char *format, ...);
  void logStatusCapture(const char *format, ...);
  void startCommandTrace(const char *name);
  void appendCommandTrace(const String &line);
  void finishCommandTrace(bool success, const char *reason);
  void saveCommandTraceFailure(const char *reason);
  String commandTraceTimestamp() const;

  HardwareSerial *serial_ = nullptr;
  int txEnablePin_ = -1;
  FrameParser parser_;
  SpaState state_;
  SpaCapabilities capabilities_;
  std::deque<PendingCommand> queue_;
  bool stateChanged_ = false;
  uint32_t lastTxMs_ = 0;
  bool benchMode_ = false;
  uint32_t lastBenchUpdateMs_ = 0;
  bool targetTempActive_ = false;
  bool targetTempWaitingForStatus_ = false;
  uint8_t targetTempPhase_ = 0;
  int targetSetTemp_ = 0;
  uint32_t targetTempStatusMs_ = 0;
  uint32_t targetTempCommandMs_ = 0;
  uint8_t rawStatusCaptureCount_ = 0;
  uint8_t editValueHour_ = 0;
  uint8_t editValueMinute_ = 0;
  uint8_t menuMajor_ = 0;
  uint8_t menuMinor_ = 0;
  uint8_t filterMenuSelector_ = 0;
  bool filterProgramActive_ = false;
  bool filterProgramWaiting_ = false;
  bool filterProgramQueued_ = false;
  bool filterProgramReadOnly_ = false;
  bool filterProgramHomeLightSent_ = false;
  uint8_t filterProgramPhase_ = 0;
  uint8_t filterProgramCycle_ = 1;
  bool filterProgramEnabled_ = true;
  uint8_t filterProgramStartHour_ = 0;
  uint8_t filterProgramStartMinute_ = 0;
  uint8_t filterProgramDurationHour_ = 0;
  uint8_t filterProgramDurationMinute_ = 0;
  uint8_t filterProgramStepCount_ = 0;
  uint32_t filterProgramStatusMs_ = 0;
  uint32_t filterProgramCommandMs_ = 0;
  bool filterProgramLastFailed_ = false;
  String filterProgramLastError_;
  bool filterCycleCachePending_ = false;
  FilterCycleCache filterCycleCache_;
  std::deque<String> debugLog_;
  bool statusCaptureActive_ = false;
  std::deque<String> statusCaptureLog_;
  bool commandTraceActive_ = false;
  String commandTraceName_;
  uint32_t commandTraceStartedMs_ = 0;
  std::deque<String> commandTraceLog_;
};

const char *commandStatusName(CommandStatus status);

}  // namespace Balboa
