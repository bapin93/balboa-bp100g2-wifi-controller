#include "balboa/protocol.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace Balboa {
namespace {

constexpr uint8_t CrcPolynomial = 0x07;
constexpr uint8_t CrcInitial = 0x88;
constexpr size_t MinimumEncodedBodyLength = 5;  // len + src + dst + type + crc

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
  return crc;
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
    return {std::nullopt, ParseError::TooShort};
  }
  if (wireBytes.front() != FrameDelimiter || wireBytes.back() != FrameDelimiter) {
    return {std::nullopt, ParseError::MissingDelimiter};
  }

  std::vector<uint8_t> body(wireBytes.begin() + 1, wireBytes.end() - 1);
  if (body.size() < MinimumEncodedBodyLength) {
    return {std::nullopt, ParseError::TooShort};
  }
  if (body[0] != body.size()) {
    return {std::nullopt, ParseError::LengthMismatch};
  }

  const uint8_t expected = body.back();
  body.pop_back();
  const uint8_t actual = crc8(body);
  if (actual != expected) {
    return {std::nullopt, ParseError::ChecksumMismatch};
  }

  Frame frame;
  frame.source = body[1];
  frame.target = body[2];
  frame.type = body[3];
  frame.payload.assign(body.begin() + 4, body.end());
  return {frame, ParseError::None};
}

std::optional<ParseResult> FrameParser::ingest(uint8_t byte) {
  if (byte == FrameDelimiter) {
    if (!inFrame_) {
      inFrame_ = true;
      buffer_.clear();
      buffer_.push_back(byte);
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
  if (buffer_.size() > 128) {
    reset();
    return ParseResult{std::nullopt, ParseError::LengthMismatch};
  }
  return std::nullopt;
}

void FrameParser::reset() {
  inFrame_ = false;
  buffer_.clear();
}

std::vector<uint8_t> buildPanelCommand(uint8_t buttonCode) {
  Frame frame;
  frame.type = MessagePanelCommand;
  frame.payload = {buttonCode};
  return encodeFrame(frame);
}

std::vector<uint8_t> buildSetTimeCommand(uint8_t hour, uint8_t minute) {
  Frame frame;
  frame.type = MessageSetTime;
  frame.payload = {hour, minute};
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
