#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Balboa {

// Message and field layouts follow github.com/ccutrer/balboa_worldwide_app/wiki and pybalboa.
constexpr uint8_t FrameDelimiter = 0x7e;
constexpr uint8_t MessageToggleItem = 0x11;
constexpr uint8_t MessageStatusUpdate = 0x13;
constexpr uint8_t MessageSetTemperature = 0x20;
constexpr uint8_t MessageSetTime = 0x21;
constexpr uint8_t MessageSettingsRequest = 0x22;
constexpr uint8_t MessageFilterCyclesResponse = 0x23;
constexpr uint8_t MessageInformationResponse = 0x24;
constexpr uint8_t MessageSetupParametersResponse = 0x25;
constexpr uint8_t MessageFaultLogResponse = 0x28;
constexpr uint8_t MessageConfigurationResponse = 0x2e;

// Settings request (0x22) codes; each is answered by the matching response message.
constexpr uint8_t SettingsConfiguration = 0x00;
constexpr uint8_t SettingsFilterCycles = 0x01;
constexpr uint8_t SettingsInformation = 0x02;
constexpr uint8_t SettingsSetupParameters = 0x04;
constexpr uint8_t SettingsFaultLog = 0x20;
constexpr uint8_t FaultLogLatestEntry = 0xff;

// Toggle item (0x11) codes. Each press advances the item to its next state.
constexpr uint8_t ToggleClearNotification = 0x03;
constexpr uint8_t TogglePump1 = 0x04;
constexpr uint8_t ToggleLight1 = 0x11;
constexpr uint8_t ToggleHold = 0x3c;

constexpr uint8_t SpaStateHold = 0x05;
constexpr size_t MaxPumps = 6;

struct Frame {
  uint8_t source = 0xff;
  uint8_t target = 0xaf;
  uint8_t type = 0;
  std::vector<uint8_t> payload;
};

enum class ParseError {
  None,
  TooShort,
  MissingDelimiter,
  LengthMismatch,
  ChecksumMismatch,
};

struct ParseResult {
  std::optional<Frame> frame;
  ParseError error = ParseError::None;
  std::vector<uint8_t> wireBytes;
  size_t reportedLength = 0;
  size_t actualLength = 0;
  uint8_t expectedCrc = 0;
  uint8_t actualCrc = 0;
};

class FrameParser {
 public:
  std::optional<ParseResult> ingest(uint8_t byte);
  void reset();

 private:
  bool inFrame_ = false;
  std::vector<uint8_t> buffer_;
};

struct StatusUpdate {
  uint8_t spaState = 0;     // byte 0: 0x00 running, 0x01 initializing, 0x05 hold, 0x17 test
  uint8_t initMode = 0;     // byte 1: 0x01 priming. Bytes 0-1 also change while the panel is in a menu.
  float waterTemp = 0;      // NAN when the spa does not know yet
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t heatMode = 0;     // 0 ready, 1 rest, 2 ready-in-rest
  uint8_t reminder = 0;     // byte 6: 0x04 clean filter, 0x09 check sanitizer, 0x0a check pH
  uint8_t holdMinutes = 0;  // byte 7, only meaningful in hold
  bool filter1Running = false;
  bool filter2Running = false;
  bool panelLocked = false;
  bool tempRangeHigh = false;
  bool heating = false;
  bool heatWaiting = false;
  uint8_t pumpSpeed[MaxPumps] = {};  // 0 off, 1 low, 2 high
  bool circulationPump = false;
  bool blower = false;
  bool light1 = false;
  float setTemp = 0;

  bool inHold() const { return spaState == SpaStateHold; }
  bool atMainScreen() const { return spaState == 0 && initMode == 0; }
};

struct DeviceConfiguration {
  uint8_t pumpSpeeds[MaxPumps] = {};  // 0 not installed, 1 single-speed, 2 two-speed
  bool light1 = false;
  bool light2 = false;
  bool blower = false;
  bool circulationPump = false;
  bool aux1 = false;
  bool aux2 = false;
  bool mister = false;
};

struct SystemInformation {
  std::string softwareVersion;  // e.g. "M100_225 V43.0"
  std::string model;
  uint8_t setupNumber = 0;
  uint16_t heaterVoltage = 0;   // 0 when unknown
  uint16_t dipSwitches = 0;
};

struct SetupParameters {
  uint8_t lowRangeMin = 0;
  uint8_t lowRangeMax = 0;
  uint8_t highRangeMin = 0;
  uint8_t highRangeMax = 0;
};

struct FaultLogEntry {
  uint8_t totalEntries = 0;
  uint8_t entryNumber = 0;
  uint8_t code = 0;
  uint8_t daysAgo = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t setTemp = 0;
  uint8_t sensorA = 0;
  uint8_t sensorB = 0;
};

uint8_t crc8(const uint8_t *data, size_t len);
uint8_t crc8(const std::vector<uint8_t> &data);

std::vector<uint8_t> encodeFrame(const Frame &frame);
ParseResult decodeFrameBytes(const std::vector<uint8_t> &wireBytes);

std::optional<StatusUpdate> decodeStatusUpdate(const std::vector<uint8_t> &payload);
std::optional<DeviceConfiguration> decodeDeviceConfiguration(const std::vector<uint8_t> &payload);
std::optional<SystemInformation> decodeSystemInformation(const std::vector<uint8_t> &payload);
std::optional<SetupParameters> decodeSetupParameters(const std::vector<uint8_t> &payload);
std::optional<FaultLogEntry> decodeFaultLogEntry(const std::vector<uint8_t> &payload);

std::vector<uint8_t> buildToggleItemCommand(uint8_t itemCode);
std::vector<uint8_t> buildSetTemperatureCommand(uint8_t temperature);
std::vector<uint8_t> buildSetTimeCommand(uint8_t hour, uint8_t minute);
std::vector<uint8_t> buildSettingsRequest(uint8_t code, uint8_t arg1 = 0x00, uint8_t arg2 = 0x00);
std::vector<uint8_t> buildFilterCyclesRequest();
std::vector<uint8_t> buildSetFilterCyclesCommand(uint8_t cycle1Start, uint8_t cycle1StartMinute,
                                                 uint8_t cycle1Duration, uint8_t cycle1DurationMinute,
                                                 bool cycle2Enabled, uint8_t cycle2Start,
                                                 uint8_t cycle2StartMinute, uint8_t cycle2Duration,
                                                 uint8_t cycle2DurationMinute);

std::string bytesToHex(const std::vector<uint8_t> &bytes);
const char *parseErrorName(ParseError error);

}  // namespace Balboa
