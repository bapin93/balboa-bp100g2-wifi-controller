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
  bool blower = false;
  bool light = false;
  uint8_t heatMode = 0;
  bool heating = false;
  bool priming = false;
  bool tempRangeHigh = true;
  uint8_t filterCycle1Start = 0;
  uint8_t filterCycle1Dur = 0;
  uint8_t filterCycle2Start = 0;
  uint8_t filterCycle2Dur = 0;
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

class SpaController {
 public:
  void begin(HardwareSerial &serial, int txEnablePin);
  void loop();
  void enableBenchMode();

  const SpaState &state() const { return state_; }
  const SpaCapabilities &capabilities() const { return capabilities_; }
  void setCapabilities(const SpaCapabilities &capabilities) { capabilities_ = capabilities; }
  void setConfiguredFilterCycles(uint8_t cycle1Start, uint8_t cycle1Duration, bool cycle2Enabled,
                                 uint8_t cycle2Start, uint8_t cycle2Duration);
  bool hasFreshState(uint32_t nowMs) const;

  bool consumeStateChanged();
  void writeStateJson(JsonObject out) const;
  void writeCapabilitiesJson(JsonObject out) const;

  CommandResult handleCommand(JsonVariantConst command);

 private:
  struct PendingCommand {
    std::vector<uint8_t> frame;
    uint32_t queuedAtMs;
    String label;
  };

  void processRx();
  void processTx();
  void updateBenchState();
  CommandResult handleBenchCommand(const char *action, JsonVariantConst command);
  void handleFrame(const Frame &frame);
  bool decodeStatusFrame(const Frame &frame);
  CommandResult enqueue(const char *label, std::vector<uint8_t> frame);
  CommandResult enqueueButton(const char *label, uint8_t buttonCode);
  void transmit(const std::vector<uint8_t> &frame);

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
};

const char *commandStatusName(CommandStatus status);

}  // namespace Balboa
