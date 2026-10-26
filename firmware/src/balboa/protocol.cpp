#include "balboa/protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace Balboa {
namespace {

constexpr uint8_t CrcPolynomial = 0x07;
constexpr uint8_t CrcInitial = 0x02;
constexpr uint8_t CrcXorOut = 0x02;
constexpr uint8_t PanelSource = 0x10;
constexpr uint8_t PanelTarget = 0xbf;
constexpr size_t MinimumEncodedBodyLength = 5;  // len + src + dst + type + crc

float decodeTemp(uint8_t raw) {
  return raw == 0xff || raw == 0x00 ? NAN : static_cast<float>(raw);
}

ParseResult makeError(ParseError error, const std::vector<uint8_t> &wireBytes,
                      size_t reportedLength = 0, size_t actualLength = 0,
                      uint8_t expectedCrc = 0, uint8_t actualCrc = 0) {
  ParseResult result;
  result.error = error;
  result.wireBytes = wireBytes;
  result.reportedLength = reportedLength;
  result.actualLength = actualLength;
  result.expectedCrc = expectedCrc;
  result.actualCrc = actualCrc;
  return result;
}

std::vector<uint8_t> frameBodyWithoutCrc(const Frame &frame) {
  std::vector<uint8_t> body;
  body.reserve(4 + frame.payload.size() + 1);
  body.push_back(static_cast<uint8_t>(5 + frame.payload.size()));
  body.push_back(frame.source);
  body.push_back(frame.target);
  body.push_back(frame.type);
  body.insert(body.end(), frame.payload.begin(), frame.payload.end());
  return body;
}

}  // namespace

uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = CrcInitial;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ CrcPolynomial)
                         : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc ^ CrcXorOut;
}

uint8_t crc8(const std::vector<uint8_t> &data) {
  return data.empty() ? crc8(nullptr, 0) : crc8(data.data(), data.size());
}

std::vector<uint8_t> encodeFrame(const Frame &frame) {
  std::vector<uint8_t> body = frameBodyWithoutCrc(frame);
  body.push_back(crc8(body));

  std::vector<uint8_t> wire;
  wire.reserve(body.size() + 2);
  wire.push_back(FrameDelimiter);
  wire.insert(wire.end(), body.begin(), body.end());
  wire.push_back(FrameDelimiter);
  return wire;
}

ParseResult decodeFrameBytes(const std::vector<uint8_t> &wireBytes) {
  if (wireBytes.size() < MinimumEncodedBodyLength + 2) {
    return makeError(ParseError::TooShort, wireBytes, 0, wireBytes.size());
  }
  if (wireBytes.front() != FrameDelimiter || wireBytes.back() != FrameDelimiter) {
    return makeError(ParseError::MissingDelimiter, wireBytes);
  }

  std::vector<uint8_t> body(wireBytes.begin() + 1, wireBytes.end() - 1);
  if (body.size() < MinimumEncodedBodyLength) {
    return makeError(ParseError::TooShort, wireBytes, 0, body.size());
  }
  if (body[0] != body.size()) {
    return makeError(ParseError::LengthMismatch, wireBytes, body[0], body.size());
  }

  const uint8_t expected = body.back();
  body.pop_back();
  const uint8_t actual = crc8(body);
  if (actual != expected) {
    return makeError(ParseError::ChecksumMismatch, wireBytes, body[0], body.size() + 1, expected, actual);
  }

  Frame frame;
  frame.source = body[1];
  frame.target = body[2];
  frame.type = body[3];
  frame.payload.assign(body.begin() + 4, body.end());
  ParseResult result;
  result.frame = frame;
  result.error = ParseError::None;
  result.wireBytes = wireBytes;
  result.reportedLength = wireBytes.size() >= 3 ? wireBytes[1] : 0;
  result.actualLength = wireBytes.size() >= 2 ? wireBytes.size() - 2 : 0;
  return result;
}

std::optional<ParseResult> FrameParser::ingest(uint8_t byte) {
  if (byte == FrameDelimiter) {
    if (!inFrame_) {
      inFrame_ = true;
      buffer_.clear();
      buffer_.push_back(byte);
      return std::nullopt;
    }

    if (buffer_.size() == 1) {
      return std::nullopt;
    }

    buffer_.push_back(byte);
    auto result = decodeFrameBytes(buffer_);
    reset();
    return result;
  }

  if (!inFrame_) {
    return std::nullopt;
  }

  buffer_.push_back(byte);
  if (buffer_.size() >= 3) {
    const size_t expectedWireLength = static_cast<size_t>(buffer_[1]) + 2;
    if (buffer_.size() > expectedWireLength) {
      const std::vector<uint8_t> wireBytes = buffer_;
      reset();
      return makeError(ParseError::MissingDelimiter, wireBytes, wireBytes[1], wireBytes.size() - 1);
    }
  }
  if (buffer_.size() > 128) {
    const std::vector<uint8_t> wireBytes = buffer_;
    reset();
    return makeError(ParseError::LengthMismatch, wireBytes, 0, wireBytes.size());
  }
  return std::nullopt;
}

void FrameParser::reset() {
  inFrame_ = false;
  buffer_.clear();
}

std::optional<StatusUpdate> decodeStatusUpdate(const std::vector<uint8_t> &payload) {
  if (payload.size() <= 20) {
    return std::nullopt;
  }
  StatusUpdate status;
  status.spaState = payload[0];
  status.initMode = payload[1];
  status.waterTemp = decodeTemp(payload[2]);
  status.hour = payload[3] & 0x1f;
  status.minute = payload[4] & 0x3f;
  const uint8_t heatMode = payload[5] & 0x03;
  status.heatMode = heatMode == 3 ? 2 : heatMode;  // packs report ready-in-rest as 2 or 3
  status.reminder = payload[6];
  status.holdMinutes = payload[7];
  status.filter1Running = (payload[9] & 0x04) != 0;
  status.filter2Running = (payload[9] & 0x08) != 0;
  status.panelLocked = (payload[9] & 0x20) != 0;
  status.tempRangeHigh = (payload[10] & 0x04) != 0;
  const uint8_t heatState = (payload[10] >> 4) & 0x03;
  status.heating = heatState == 1;
  status.heatWaiting = heatState == 2;
  for (size_t i = 0; i < 4; ++i) {
    status.pumpSpeed[i] = (payload[11] >> (2 * i)) & 0x03;
  }
  status.pumpSpeed[4] = payload[12] & 0x03;
  status.pumpSpeed[5] = (payload[12] >> 2) & 0x03;
  status.circulationPump = (payload[13] & 0x02) != 0;
  status.blower = (payload[13] & 0x0c) != 0;
  status.light1 = (payload[14] & 0x03) != 0;
  status.setTemp = decodeTemp(payload[20]);
  return status;
}

std::optional<DeviceConfiguration> decodeDeviceConfiguration(const std::vector<uint8_t> &payload) {
  if (payload.size() < 5) {
    return std::nullopt;
  }
  DeviceConfiguration config;
  for (size_t i = 0; i < 4; ++i) {
    config.pumpSpeeds[i] = (payload[0] >> (2 * i)) & 0x03;
  }
  config.pumpSpeeds[4] = payload[1] & 0x03;
  config.pumpSpeeds[5] = (payload[1] >> 6) & 0x03;
  config.light1 = (payload[2] & 0x03) != 0;
  config.light2 = (payload[2] & 0x0c) != 0;
  config.blower = (payload[3] & 0x03) != 0;
  config.circulationPump = (payload[3] & 0x80) != 0;
  config.aux1 = (payload[4] & 0x01) != 0;
  config.aux2 = (payload[4] & 0x02) != 0;
  config.mister = (payload[4] & 0x10) != 0;
  return config;
}

std::optional<SystemInformation> decodeSystemInformation(const std::vector<uint8_t> &payload) {
  if (payload.size() < 21) {
    return std::nullopt;
  }
  SystemInformation info;
  char version[24];
  snprintf(version, sizeof(version), "M%u_%u V%u.%u", payload[0], payload[1], payload[2], payload[3]);
  info.softwareVersion = version;
  for (size_t i = 4; i < 12; ++i) {
    if (payload[i] >= 0x20 && payload[i] < 0x7f) {
      info.model += static_cast<char>(payload[i]);
    }
  }
  while (!info.model.empty() && info.model.back() == ' ') {
    info.model.pop_back();
  }
  info.setupNumber = payload[12];
  info.heaterVoltage = payload[17] == 0x01 ? 240 : 0;
  info.dipSwitches = static_cast<uint16_t>(payload[19] | (payload[20] << 8));
  return info;
}

std::optional<SetupParameters> decodeSetupParameters(const std::vector<uint8_t> &payload) {
  if (payload.size() < 6) {
    return std::nullopt;
  }
  SetupParameters params;
  params.lowRangeMin = payload[2];
  params.lowRangeMax = payload[3];
  params.highRangeMin = payload[4];
  params.highRangeMax = payload[5];
  return params;
}

std::optional<FaultLogEntry> decodeFaultLogEntry(const std::vector<uint8_t> &payload) {
  if (payload.size() < 10) {
    return std::nullopt;
  }
  FaultLogEntry entry;
  entry.totalEntries = payload[0];
  entry.entryNumber = payload[1];
  entry.code = payload[2];
  entry.daysAgo = payload[3];
  entry.hour = payload[4];
  entry.minute = payload[5];
  entry.setTemp = payload[7];
  entry.sensorA = payload[8];
  entry.sensorB = payload[9];
  return entry;
}

std::vector<uint8_t> buildToggleItemCommand(uint8_t itemCode) {
  Frame frame;
  frame.source = PanelSource;
  frame.target = PanelTarget;
  frame.type = MessageToggleItem;
  frame.payload = {itemCode, 0x00};
  return encodeFrame(frame);
}

std::vector<uint8_t> buildSetTemperatureCommand(uint8_t temperature) {
  Frame frame;
  frame.source = PanelSource;
  frame.target = PanelTarget;
  frame.type = MessageSetTemperature;
  frame.payload = {temperature};
  return encodeFrame(frame);
}

std::vector<uint8_t> buildSetTimeCommand(uint8_t hour, uint8_t minute) {
  Frame frame;
  frame.source = PanelSource;
  frame.target = PanelTarget;
  frame.type = MessageSetTime;
  frame.payload = {hour, minute};
  return encodeFrame(frame);
}

std::vector<uint8_t> buildSettingsRequest(uint8_t code, uint8_t arg1, uint8_t arg2) {
  Frame frame;
  frame.source = PanelSource;
  frame.target = PanelTarget;
  frame.type = MessageSettingsRequest;
  frame.payload = {code, arg1, arg2};
  return encodeFrame(frame);
}

std::vector<uint8_t> buildFilterCyclesRequest() {
  return buildSettingsRequest(SettingsFilterCycles);
}

std::vector<uint8_t> buildSetFilterCyclesCommand(uint8_t cycle1Start, uint8_t cycle1StartMinute,
                                                 uint8_t cycle1Duration, uint8_t cycle1DurationMinute,
                                                 bool cycle2Enabled, uint8_t cycle2Start,
                                                 uint8_t cycle2StartMinute, uint8_t cycle2Duration,
                                                 uint8_t cycle2DurationMinute) {
  Frame frame;
  frame.source = PanelSource;
  frame.target = PanelTarget;
  frame.type = MessageFilterCyclesResponse;
  const uint8_t cycle2StartEncoded = static_cast<uint8_t>((cycle2Start & 0x7f) | (cycle2Enabled ? 0x80 : 0x00));
  frame.payload = {cycle1Start, cycle1StartMinute, cycle1Duration, cycle1DurationMinute,
                   cycle2StartEncoded, cycle2StartMinute, cycle2Duration, cycle2DurationMinute};
  return encodeFrame(frame);
}

std::string bytesToHex(const std::vector<uint8_t> &bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (uint8_t byte : bytes) {
    out << std::setw(2) << static_cast<int>(byte);
  }
  return out.str();
}

const char *parseErrorName(ParseError error) {
  switch (error) {
    case ParseError::None:
      return "none";
    case ParseError::TooShort:
      return "too_short";
    case ParseError::MissingDelimiter:
      return "missing_delimiter";
    case ParseError::LengthMismatch:
      return "length_mismatch";
    case ParseError::ChecksumMismatch:
      return "checksum_mismatch";
  }
  return "unknown";
}

}  // namespace Balboa
