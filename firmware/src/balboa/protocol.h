#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Balboa {

constexpr uint8_t FrameDelimiter = 0x7e;
constexpr uint8_t MessageStatusUpdate = 0x13;
constexpr uint8_t MessagePanelCommand = 0x11;
constexpr uint8_t MessageSetTemperature = 0x20;
constexpr uint8_t MessageSetTime = 0x21;
constexpr uint8_t MessageSettingsRequest = 0x22;
constexpr uint8_t MessageFilterCyclesResponse = 0x23;

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

uint8_t crc8(const uint8_t *data, size_t len);
uint8_t crc8(const std::vector<uint8_t> &data);

std::vector<uint8_t> encodeFrame(const Frame &frame);
ParseResult decodeFrameBytes(const std::vector<uint8_t> &wireBytes);

std::vector<uint8_t> buildPanelCommand(uint8_t buttonCode);
std::vector<uint8_t> buildToggleItemCommand(uint8_t itemCode);
std::vector<uint8_t> buildSetTemperatureCommand(uint8_t temperature);
std::vector<uint8_t> buildSetTimeCommand(uint8_t hour, uint8_t minute);
std::vector<uint8_t> buildFilterCyclesRequest();

std::string bytesToHex(const std::vector<uint8_t> &bytes);
const char *parseErrorName(ParseError error);

}  // namespace Balboa
