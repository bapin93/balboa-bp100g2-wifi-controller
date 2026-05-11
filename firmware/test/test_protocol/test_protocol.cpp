#include <unity.h>

#include <vector>

#include "balboa/protocol.h"

using namespace Balboa;

void test_crc_matches_known_status_frame_fixture() {
  const std::vector<uint8_t> bodyWithoutCrc = {
      0x1d, 0xff, 0xaf, 0x13, 0x00, 0x00, 0x64, 0x07, 0x07, 0x00,
      0x00, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_HEX8(0xec, crc8(bodyWithoutCrc));
}

void test_decode_known_status_frame_fixture() {
  const std::vector<uint8_t> wire = {
      0x7e, 0x1d, 0xff, 0xaf, 0x13, 0x00, 0x00, 0x64, 0x07, 0x07,
      0x00, 0x00, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0xec,
      0x7e};
  ParseResult result = decodeFrameBytes(wire);
  TEST_ASSERT_EQUAL(ParseError::None, result.error);
  TEST_ASSERT_TRUE(result.frame.has_value());
  TEST_ASSERT_EQUAL_HEX8(0xff, result.frame->source);
  TEST_ASSERT_EQUAL_HEX8(0xaf, result.frame->target);
  TEST_ASSERT_EQUAL_HEX8(MessageStatusUpdate, result.frame->type);
}

void test_decode_captured_panel_frames() {
  const std::vector<uint8_t> poll = {0x7e, 0x05, 0x10, 0xbf, 0x06, 0x5c, 0x7e};
  ParseResult pollResult = decodeFrameBytes(poll);
  TEST_ASSERT_EQUAL(ParseError::None, pollResult.error);
  TEST_ASSERT_TRUE(pollResult.frame.has_value());
  TEST_ASSERT_EQUAL_HEX8(0x10, pollResult.frame->source);
  TEST_ASSERT_EQUAL_HEX8(0xbf, pollResult.frame->target);
  TEST_ASSERT_EQUAL_HEX8(0x06, pollResult.frame->type);

  const std::vector<uint8_t> command = {0x7e, 0x07, 0x10, 0xbf, 0x11, 0x00, 0x00, 0x3e, 0x7e};
  ParseResult commandResult = decodeFrameBytes(command);
  TEST_ASSERT_EQUAL(ParseError::None, commandResult.error);
  TEST_ASSERT_TRUE(commandResult.frame.has_value());
  TEST_ASSERT_EQUAL_HEX8(MessagePanelCommand, commandResult.frame->type);
  TEST_ASSERT_EQUAL_UINT8(2, commandResult.frame->payload.size());
}

void test_encode_round_trips_panel_command() {
  std::vector<uint8_t> wire = buildPanelCommand(0x91);
  const std::vector<uint8_t> capturedLightCommand = {0x7e, 0x07, 0x10, 0xbf, 0x11, 0x91, 0x00, 0xca, 0x7e};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(capturedLightCommand.data(), wire.data(), capturedLightCommand.size());
  ParseResult result = decodeFrameBytes(wire);
  TEST_ASSERT_EQUAL(ParseError::None, result.error);
  TEST_ASSERT_TRUE(result.frame.has_value());
  TEST_ASSERT_EQUAL_HEX8(MessagePanelCommand, result.frame->type);
  TEST_ASSERT_EQUAL_UINT8(2, result.frame->payload.size());
  TEST_ASSERT_EQUAL_HEX8(0x91, result.frame->payload[0]);
  TEST_ASSERT_EQUAL_HEX8(0x00, result.frame->payload[1]);
}

void test_encode_captured_temperature_commands() {
  const std::vector<uint8_t> warm = buildPanelCommand(0x01);
  const std::vector<uint8_t> capturedWarmCommand = {0x7e, 0x07, 0x10, 0xbf, 0x11, 0x01, 0x00, 0x2b, 0x7e};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(capturedWarmCommand.data(), warm.data(), capturedWarmCommand.size());

  const std::vector<uint8_t> cool = buildPanelCommand(0x02);
  const std::vector<uint8_t> capturedCoolCommand = {0x7e, 0x07, 0x10, 0xbf, 0x11, 0x02, 0x00, 0x14, 0x7e};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(capturedCoolCommand.data(), cool.data(), capturedCoolCommand.size());
}

void test_encode_set_time_command_uses_panel_addressing() {
  const std::vector<uint8_t> wire = buildSetTimeCommand(22, 53);
  const std::vector<uint8_t> expected = {0x7e, 0x07, 0x10, 0xbf, 0x21, 0x16, 0x35, 0x7d, 0x7e};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), wire.data(), expected.size());
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
  RUN_TEST(test_decode_captured_panel_frames);
  RUN_TEST(test_encode_round_trips_panel_command);
  RUN_TEST(test_encode_captured_temperature_commands);
  RUN_TEST(test_encode_set_time_command_uses_panel_addressing);
  RUN_TEST(test_stream_parser_emits_complete_frame);
  return UNITY_END();
}
