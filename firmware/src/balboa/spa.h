#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <deque>
#include <optional>
#include <vector>

#include "balboa/protocol.h"

namespace Balboa {

// What the UI should offer. Equipment comes from the spa's configuration response once it arrives.
struct SpaCapabilities {
  bool detected = false;
  uint8_t pump1Speeds = 2;  // 0 none, 1 single-speed, 2 two-speed
  uint8_t pump2Speeds = 0;
  bool light = true;
  bool setTime = true;
};

struct SpaState {
  float waterTemp = NAN;
  float setTemp = NAN;
  bool jet1 = false;
  bool jet2 = false;
  uint8_t jet1Speed = 0;
  uint8_t jet2Speed = 0;
  bool blower = false;
  bool circulationPump = false;
  bool light = false;
  uint8_t heatMode = 0;  // 0=ready, 1=rest, 2=ready-in-rest (read-only; the spa manages it)
  bool heating = false;
  bool heatPending = false;
  bool priming = false;
  bool tempRangeHigh = true;
  bool hold = false;
  uint8_t holdMinutes = 0;
  uint8_t reminder = 0;
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
  bool filterCyclesValid = false;
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

class SpaController {
 public:
  void begin(HardwareSerial &serial, int txEnablePin);
  void loop();
  void enableBenchMode();

  const SpaState &state() const { return state_; }
  bool hasFreshState() const;
  void setUserCapabilities(bool setTime, bool hideJet2);
  void setConfiguredFilterCycles(uint8_t cycle1Start, uint8_t cycle1StartMinute, uint8_t cycle1Duration,
                                 uint8_t cycle1DurationMinute, bool cycle2Enabled,
                                 uint8_t cycle2Start, uint8_t cycle2StartMinute, uint8_t cycle2Duration,
                                 uint8_t cycle2DurationMinute);

  bool consumeStateChanged();
  void writeStateJson(JsonObject out) const;
  void writeCapabilitiesJson(JsonObject out) const;
  void writeSpaInfoJson(JsonObject out) const;
  void writeFaultLogJson(JsonObject out) const;
  String debugLogText() const;
  void clearDebugLog();
  void startStatusCapture();
  void stopStatusCapture();
  void clearStatusCapture();
  String statusCaptureText() const;
  bool statusCaptureActive() const { return statusCaptureActive_; }
  size_t statusCaptureSize() const { return statusCaptureLog_.size(); }
  // True once after each filter cycle read from the spa; values are in state().
  bool consumeFilterCyclesRead();

  CommandResult handleCommand(JsonVariantConst command);

 private:
  struct PendingCommand {
    std::vector<uint8_t> frame;
    uint32_t queuedAtMs;
    uint32_t minDelayMs;
    String label;
  };

  // Drives a pump to an exact speed with single presses, checking the status after each one.
  struct PumpCommand {
    bool active = false;
    uint8_t pump = 0;  // 1-based
    uint8_t target = 0;
    uint8_t startSpeed = 0;
    uint8_t lastSpeed = 0;
    uint8_t presses = 0;
    uint8_t ignoredPresses = 0;
    bool pressQueued = false;
    uint32_t pressSentMs = 0;
    uint32_t seq = 0;  // increments when a command finishes
    bool ok = true;
    char message[96] = "";
  };

  struct FaultLogRead {
    bool active = false;
    bool awaiting = false;
    uint8_t next = FaultLogLatestEntry;
    uint8_t total = 0;
    uint8_t retries = 0;
    uint32_t requestMs = 0;
    uint32_t seq = 0;  // increments when a read finishes
  };

  void processRx();
  void processTx();
  void processPumpCommand();
  void processFaultLogRead();
  void updateBenchState();
  CommandResult handleBenchCommand(const char *action, JsonVariantConst command);
  CommandResult startPumpCommand(uint8_t pump, uint8_t target);
  void finishPumpCommand(bool ok, const char *format, ...);
  CommandResult startFaultLogRead();
  void requestSpaInfo();
  void handleFrame(const Frame &frame);
  void applyEquipmentCapabilities();
  void applyStatus(const StatusUpdate &status);
  void applyFaultLogEntry(const FaultLogEntry &entry);
  void publishFilterCyclesRead();
  bool decodeFilterCyclesResponse(const Frame &frame);
  uint8_t pumpSpeeds(uint8_t pump) const;
  uint8_t pumpSpeed(uint8_t pump) const;
  CommandResult enqueue(const char *label, std::vector<uint8_t> frame, uint32_t minDelayMs = 550);
  CommandResult enqueueToggle(const char *label, uint8_t itemCode, uint32_t minDelayMs = 550);
  void transmit(const std::vector<uint8_t> &frame);
  void logDebug(const char *format, ...);
  void logStatusCapture(const char *format, ...);

  HardwareSerial *serial_ = nullptr;
  int txEnablePin_ = -1;
  FrameParser parser_;
  SpaState state_;
  SpaCapabilities capabilities_;
  std::optional<DeviceConfiguration> equipment_;
  bool hideJet2_ = false;
  std::optional<SystemInformation> systemInfo_;
  std::optional<SetupParameters> setupParams_;
  uint32_t spaInfoSeq_ = 0;
  std::vector<FaultLogEntry> faultLog_;
  FaultLogRead faultLogRead_;
  PumpCommand pumpCommand_;
  char validationMessage_[64] = "";
  std::deque<PendingCommand> queue_;
  bool stateChanged_ = false;
  uint32_t lastTxMs_ = 0;
  bool benchMode_ = false;
  uint32_t lastBenchUpdateMs_ = 0;
  uint8_t rawStatusCaptureCount_ = 0;
  bool filterCyclesReadPending_ = false;
  uint32_t filterCyclesSeq_ = 0;
  std::deque<String> debugLog_;
  bool statusCaptureActive_ = false;
  std::deque<String> statusCaptureLog_;
};

const char *commandStatusName(CommandStatus status);

}  // namespace Balboa
