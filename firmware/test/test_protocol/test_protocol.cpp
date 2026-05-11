#include <unity.h>

#include <vector>

#include "balboa/protocol.h"

using namespace Balboa;

void test_crc_matches_known_status_frame_fixture() {
  const std::vector<uint8_t> bodyWithoutCrc = {
      0x1d, 0xff, 0xaf, 0x13, 0x00, 0x00, 0x64, 0x07, 0x07, 0x00,
      0x00, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_HEX8(0xc2, crc8(bodyWithoutCrc));
}

void test_decode_known_status_frame_fixture() {
  const std::vector<uint8_t> wire = {
      0x7e, 0x1d, 0xff, 0xaf, 0x13, 0x00, 0x00, 0x64, 0x07, 0x07,
      0x00, 0x00, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0xc2,
      0x7e};
  ParseResult result = decodeFrameBytes(wire);
  TEST_ASSERT_EQUAL(ParseError::None, result.error);
  TEST_ASSERT_TRUE(result.frame.has_value());
  TEST_ASSERT_EQUAL_HEX8(0xff, result.frame->source);
  TEST_ASSERT_EQUAL_HEX8(0xaf, result.frame->target);
  TEST_ASSERT_EQUAL_HEX8(MessageStatusUpdate, result.frame->type);
}

void test_encode_round_trips_panel_command() {
  std::vector<uint8_t> wire = buildPanelCommand(0x04);
  ParseResult result = decodeFrameBytes(wire);
  TEST_ASSERT_EQUAL(ParseError::None, result.error);
  TEST_ASSERT_TRUE(result.frame.has_value());
  TEST_ASSERT_EQUAL_HEX8(MessagePanelCommand, result.frame->type);
  TEST_ASSERT_EQUAL_UINT8(1, result.frame->payload.size());
  TEST_ASSERT_EQUAL_HEX8(0x04, result.frame->payload[0]);
}

void test_stream_parser_emits_complete_frame() {
  std::vector<uint8_t> wire = buildSetTimeCommand(14, 32);
  FrameParser parser;
  std::optional<ParseResult> parsed;
  for (uint8_t byte : wire) {
    parsed = parser.ingest(byte);
  }
  TEST_ASSERT_TRUE(parsed.has_value());
  TEST_ASSERT_EQUAL(ParseError::None, parsed->error);
  TEST_ASSERT_TRUE(parsed->frame.has_value());
  TEST_ASSERT_EQUAL_HEX8(MessageSetTime, parsed->frame->type);
  TEST_ASSERT_EQUAL_UINT8(14, parsed->frame->payload[0]);
  TEST_ASSERT_EQUAL_UINT8(32, parsed->frame->payload[1]);
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_crc_matches_known_status_frame_fixture);
  RUN_TEST(test_decode_known_status_frame_fixture);
  RUN_TEST(test_encode_round_trips_panel_command);
  RUN_TEST(test_stream_parser_emits_complete_frame);
  return UNITY_END();
}
