#include "balboa/protocol.h"

#include <algorithm>
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

std::vector<uint8_t> buildPanelCommand(uint8_t buttonCode) {
  Frame frame;
  frame.source = PanelSource;
  frame.target = PanelTarget;
  frame.type = MessagePanelCommand;
  frame.payload = {buttonCode, 0x00};
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
