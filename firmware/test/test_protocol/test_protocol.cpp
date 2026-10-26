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
  TEST_ASSERT_EQUAL_HEX8(MessageToggleItem, commandResult.frame->type);
  TEST_ASSERT_EQUAL_UINT8(2, commandResult.frame->payload.size());
}

void test_encode_matches_captured_panel_button() {
  std::vector<uint8_t> wire = buildToggleItemCommand(0x91);
  const std::vector<uint8_t> capturedLightCommand = {0x7e, 0x07, 0x10, 0xbf, 0x11, 0x91, 0x00, 0xca, 0x7e};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(capturedLightCommand.data(), wire.data(), capturedLightCommand.size());
  ParseResult result = decodeFrameBytes(wire);
  TEST_ASSERT_EQUAL(ParseError::None, result.error);
  TEST_ASSERT_TRUE(result.frame.has_value());
  TEST_ASSERT_EQUAL_HEX8(MessageToggleItem, result.frame->type);
  TEST_ASSERT_EQUAL_UINT8(2, result.frame->payload.size());
  TEST_ASSERT_EQUAL_HEX8(0x91, result.frame->payload[0]);
  TEST_ASSERT_EQUAL_HEX8(0x00, result.frame->payload[1]);
}

void test_encode_round_trips_toggle_item_command() {
  std::vector<uint8_t> wire = buildToggleItemCommand(0x11);
  ParseResult result = decodeFrameBytes(wire);
  TEST_ASSERT_EQUAL(ParseError::None, result.error);
  TEST_ASSERT_TRUE(result.frame.has_value());
  TEST_ASSERT_EQUAL_HEX8(MessageToggleItem, result.frame->type);
  TEST_ASSERT_EQUAL_UINT8(2, result.frame->payload.size());
  TEST_ASSERT_EQUAL_HEX8(0x11, result.frame->payload[0]);
  TEST_ASSERT_EQUAL_HEX8(0x00, result.frame->payload[1]);
}

void test_encode_captured_temperature_commands() {
  const std::vector<uint8_t> warm = buildToggleItemCommand(0x01);
  const std::vector<uint8_t> capturedWarmCommand = {0x7e, 0x07, 0x10, 0xbf, 0x11, 0x01, 0x00, 0x2b, 0x7e};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(capturedWarmCommand.data(), warm.data(), capturedWarmCommand.size());

  const std::vector<uint8_t> cool = buildToggleItemCommand(0x02);
  const std::vector<uint8_t> capturedCoolCommand = {0x7e, 0x07, 0x10, 0xbf, 0x11, 0x02, 0x00, 0x14, 0x7e};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(capturedCoolCommand.data(), cool.data(), capturedCoolCommand.size());
}

void test_encode_set_time_command_uses_panel_addressing() {
  const std::vector<uint8_t> wire = buildSetTimeCommand(22, 53);
  const std::vector<uint8_t> expected = {0x7e, 0x07, 0x10, 0xbf, 0x21, 0x16, 0x35, 0x7d, 0x7e};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), wire.data(), expected.size());
}

void test_encode_set_temperature_command_uses_panel_addressing() {
  const std::vector<uint8_t> wire = buildSetTemperatureCommand(100);
  ParseResult result = decodeFrameBytes(wire);
  TEST_ASSERT_EQUAL(ParseError::None, result.error);
  TEST_ASSERT_TRUE(result.frame.has_value());
  TEST_ASSERT_EQUAL_HEX8(0x10, result.frame->source);
  TEST_ASSERT_EQUAL_HEX8(0xbf, result.frame->target);
  TEST_ASSERT_EQUAL_HEX8(MessageSetTemperature, result.frame->type);
  TEST_ASSERT_EQUAL_UINT8(1, result.frame->payload.size());
  TEST_ASSERT_EQUAL_UINT8(100, result.frame->payload[0]);
}

void test_encode_filter_cycles_request_uses_panel_addressing() {
  const std::vector<uint8_t> wire = buildFilterCyclesRequest();
  ParseResult result = decodeFrameBytes(wire);
  TEST_ASSERT_EQUAL(ParseError::None, result.error);
  TEST_ASSERT_TRUE(result.frame.has_value());
  TEST_ASSERT_EQUAL_HEX8(0x10, result.frame->source);
  TEST_ASSERT_EQUAL_HEX8(0xbf, result.frame->target);
  TEST_ASSERT_EQUAL_HEX8(MessageSettingsRequest, result.frame->type);
  TEST_ASSERT_EQUAL_UINT8(3, result.frame->payload.size());
  TEST_ASSERT_EQUAL_HEX8(0x01, result.frame->payload[0]);
  TEST_ASSERT_EQUAL_HEX8(0x00, result.frame->payload[1]);
  TEST_ASSERT_EQUAL_HEX8(0x00, result.frame->payload[2]);
}

void test_encode_set_filter_cycles_command_packs_filter2_enable_bit() {
  const std::vector<uint8_t> wire = buildSetFilterCyclesCommand(8, 0, 2, 30, true, 20, 15, 3, 45);
  ParseResult result = decodeFrameBytes(wire);
  TEST_ASSERT_EQUAL(ParseError::None, result.error);
  TEST_ASSERT_TRUE(result.frame.has_value());
  TEST_ASSERT_EQUAL_HEX8(0x10, result.frame->source);
  TEST_ASSERT_EQUAL_HEX8(0xbf, result.frame->target);
  TEST_ASSERT_EQUAL_HEX8(MessageFilterCyclesResponse, result.frame->type);
  TEST_ASSERT_EQUAL_UINT8(8, result.frame->payload.size());
  TEST_ASSERT_EQUAL_UINT8(8, result.frame->payload[0]);
  TEST_ASSERT_EQUAL_UINT8(0, result.frame->payload[1]);
  TEST_ASSERT_EQUAL_UINT8(2, result.frame->payload[2]);
  TEST_ASSERT_EQUAL_UINT8(30, result.frame->payload[3]);
  TEST_ASSERT_EQUAL_HEX8(0x94, result.frame->payload[4]);
  TEST_ASSERT_EQUAL_UINT8(15, result.frame->payload[5]);
  TEST_ASSERT_EQUAL_UINT8(3, result.frame->payload[6]);
  TEST_ASSERT_EQUAL_UINT8(45, result.frame->payload[7]);
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

// Captured from a BP100G2 at 20:47: water 86F, set 100F, high range, heating, pump 1 low.
const std::vector<uint8_t> CapturedStatusPayload = {
    0x00, 0x00, 0x56, 0x14, 0x2f, 0x00, 0x00, 0x68, 0x00, 0x00, 0x1c, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00};

void test_decode_captured_status_update() {
  const auto status = decodeStatusUpdate(CapturedStatusPayload);
  TEST_ASSERT_TRUE(status.has_value());
  TEST_ASSERT_TRUE(status->atMainScreen());
  TEST_ASSERT_FALSE(status->inHold());
  TEST_ASSERT_EQUAL_FLOAT(86.0f, status->waterTemp);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, status->setTemp);
  TEST_ASSERT_EQUAL_UINT8(20, status->hour);
  TEST_ASSERT_EQUAL_UINT8(47, status->minute);
  TEST_ASSERT_EQUAL_UINT8(0, status->heatMode);
  TEST_ASSERT_TRUE(status->tempRangeHigh);
  TEST_ASSERT_TRUE(status->heating);
  TEST_ASSERT_FALSE(status->heatWaiting);
  TEST_ASSERT_EQUAL_UINT8(1, status->pumpSpeed[0]);
  TEST_ASSERT_EQUAL_UINT8(0, status->pumpSpeed[1]);
  TEST_ASSERT_FALSE(status->light1);
  TEST_ASSERT_FALSE(status->filter1Running);
}

void test_decode_status_fields_do_not_alias_the_clock() {
  // 09:05 has no bits that the old decoder misread as heat mode, temp range or priming.
  std::vector<uint8_t> payload = CapturedStatusPayload;
  payload[3] = 9;
  payload[4] = 5;
  payload[5] = 0x01;   // rest
  payload[10] = 0x20;  // low range, heat waiting
  payload[11] = 0x06;  // pump 1 high, pump 2 low
  payload[14] = 0x03;  // light on
  payload[9] = 0x08;   // filter 2 running
  const auto status = decodeStatusUpdate(payload);
  TEST_ASSERT_TRUE(status.has_value());
  TEST_ASSERT_EQUAL_UINT8(1, status->heatMode);
  TEST_ASSERT_FALSE(status->tempRangeHigh);
  TEST_ASSERT_FALSE(status->heating);
  TEST_ASSERT_TRUE(status->heatWaiting);
  TEST_ASSERT_EQUAL_UINT8(2, status->pumpSpeed[0]);
  TEST_ASSERT_EQUAL_UINT8(1, status->pumpSpeed[1]);
  TEST_ASSERT_TRUE(status->light1);
  TEST_ASSERT_TRUE(status->filter2Running);
  TEST_ASSERT_EQUAL_UINT8(0, status->initMode);
}

void test_decode_hold_and_ready_in_rest() {
  std::vector<uint8_t> payload = CapturedStatusPayload;
  payload[0] = SpaStateHold;
  payload[7] = 42;
  payload[5] = 0x03;
  const auto status = decodeStatusUpdate(payload);
  TEST_ASSERT_TRUE(status->inHold());
  TEST_ASSERT_EQUAL_UINT8(42, status->holdMinutes);
  TEST_ASSERT_EQUAL_UINT8(2, status->heatMode);
  TEST_ASSERT_FALSE(status->atMainScreen());
}

void test_decode_short_status_is_rejected() {
  TEST_ASSERT_FALSE(decodeStatusUpdate(std::vector<uint8_t>(20, 0)).has_value());
}

void test_decode_device_configuration_single_two_speed_pump() {
  // One two-speed pump, light 1, nothing else.
  const auto config = decodeDeviceConfiguration({0x02, 0x00, 0x01, 0x00, 0x00, 0x00});
  TEST_ASSERT_TRUE(config.has_value());
  TEST_ASSERT_EQUAL_UINT8(2, config->pumpSpeeds[0]);
  TEST_ASSERT_EQUAL_UINT8(0, config->pumpSpeeds[1]);
  TEST_ASSERT_TRUE(config->light1);
  TEST_ASSERT_FALSE(config->light2);
  TEST_ASSERT_FALSE(config->blower);
  TEST_ASSERT_FALSE(config->circulationPump);
}

void test_decode_device_configuration_full_spa() {
  const auto config = decodeDeviceConfiguration({0x96, 0x42, 0x05, 0x81, 0x13, 0x00});
  TEST_ASSERT_TRUE(config.has_value());
  TEST_ASSERT_EQUAL_UINT8(2, config->pumpSpeeds[0]);
  TEST_ASSERT_EQUAL_UINT8(1, config->pumpSpeeds[1]);
  TEST_ASSERT_EQUAL_UINT8(1, config->pumpSpeeds[2]);
  TEST_ASSERT_EQUAL_UINT8(2, config->pumpSpeeds[3]);
  TEST_ASSERT_EQUAL_UINT8(2, config->pumpSpeeds[4]);
  TEST_ASSERT_EQUAL_UINT8(1, config->pumpSpeeds[5]);
  TEST_ASSERT_TRUE(config->light1);
  TEST_ASSERT_TRUE(config->light2);
  TEST_ASSERT_TRUE(config->blower);
  TEST_ASSERT_TRUE(config->circulationPump);
  TEST_ASSERT_TRUE(config->aux1);
  TEST_ASSERT_TRUE(config->aux2);
  TEST_ASSERT_TRUE(config->mister);
}

void test_decode_system_information() {
  const std::vector<uint8_t> payload = {100, 225, 43, 0, 'B', 'P', '1', '0', '0', 'G', '2', ' ',
                                        3, 0x11, 0x22, 0x33, 0x44, 0x01, 0x0a, 0x05, 0x80};
  const auto info = decodeSystemInformation(payload);
  TEST_ASSERT_TRUE(info.has_value());
  TEST_ASSERT_EQUAL_STRING("M100_225 V43.0", info->softwareVersion.c_str());
  TEST_ASSERT_EQUAL_STRING("BP100G2", info->model.c_str());
  TEST_ASSERT_EQUAL_UINT8(3, info->setupNumber);
  TEST_ASSERT_EQUAL_UINT16(240, info->heaterVoltage);
  TEST_ASSERT_EQUAL_HEX16(0x8005, info->dipSwitches);
}

void test_decode_setup_parameters() {
  const auto params = decodeSetupParameters({0x00, 0x00, 50, 99, 80, 104, 0x00, 0x01, 0x00});
  TEST_ASSERT_TRUE(params.has_value());
  TEST_ASSERT_EQUAL_UINT8(50, params->lowRangeMin);
  TEST_ASSERT_EQUAL_UINT8(99, params->lowRangeMax);
  TEST_ASSERT_EQUAL_UINT8(80, params->highRangeMin);
  TEST_ASSERT_EQUAL_UINT8(104, params->highRangeMax);
}

void test_decode_fault_log_entry() {
  const auto entry = decodeFaultLogEntry({5, 2, 16, 3, 14, 30, 0x00, 100, 98, 97});
  TEST_ASSERT_TRUE(entry.has_value());
  TEST_ASSERT_EQUAL_UINT8(5, entry->totalEntries);
  TEST_ASSERT_EQUAL_UINT8(2, entry->entryNumber);
  TEST_ASSERT_EQUAL_UINT8(16, entry->code);
  TEST_ASSERT_EQUAL_UINT8(3, entry->daysAgo);
  TEST_ASSERT_EQUAL_UINT8(14, entry->hour);
  TEST_ASSERT_EQUAL_UINT8(30, entry->minute);
  TEST_ASSERT_EQUAL_UINT8(100, entry->setTemp);
  TEST_ASSERT_EQUAL_UINT8(98, entry->sensorA);
  TEST_ASSERT_EQUAL_UINT8(97, entry->sensorB);
  TEST_ASSERT_FALSE(decodeFaultLogEntry({1, 2, 3}).has_value());
}

void test_encode_configuration_request() {
  const ParseResult result = decodeFrameBytes(buildSettingsRequest(SettingsConfiguration, 0x00, 0x01));
  TEST_ASSERT_EQUAL(ParseError::None, result.error);
  TEST_ASSERT_EQUAL_HEX8(MessageSettingsRequest, result.frame->type);
  TEST_ASSERT_EQUAL_HEX8(0x00, result.frame->payload[0]);
  TEST_ASSERT_EQUAL_HEX8(0x00, result.frame->payload[1]);
  TEST_ASSERT_EQUAL_HEX8(0x01, result.frame->payload[2]);
}

void test_stream_parser_keeps_frame_whose_crc_is_the_delimiter() {
  // Setup parameters reply captured from a BP100G2: its CRC byte is 0x7e, the frame delimiter.
  const std::vector<uint8_t> wire = {0x7e, 0x0f, 0x10, 0xbf, 0x25, 0x04, 0x02, 0x32, 0x63,
                                     0x50, 0x68, 0x09, 0x03, 0x41, 0x02, 0x7e, 0x7e};
  FrameParser parser;
  std::optional<ParseResult> parsed;
  int results = 0;
  for (uint8_t byte : wire) {
    auto result = parser.ingest(byte);
    if (result.has_value()) {
      parsed = result;
      ++results;
    }
  }
  TEST_ASSERT_EQUAL(1, results);
  TEST_ASSERT_EQUAL(ParseError::None, parsed->error);
  TEST_ASSERT_EQUAL_HEX8(MessageSetupParametersResponse, parsed->frame->type);
  const auto params = decodeSetupParameters(parsed->frame->payload);
  TEST_ASSERT_TRUE(params.has_value());
  TEST_ASSERT_EQUAL_UINT8(50, params->lowRangeMin);
  TEST_ASSERT_EQUAL_UINT8(104, params->highRangeMax);
}

void test_stream_parser_recovers_after_frame_with_delimiter_crc() {
  // The frame after a 0x7e-CRC frame must still parse.
  std::vector<uint8_t> wire = {0x7e, 0x0f, 0x10, 0xbf, 0x25, 0x04, 0x02, 0x32, 0x63,
                               0x50, 0x68, 0x09, 0x03, 0x41, 0x02, 0x7e, 0x7e};
  const std::vector<uint8_t> next = buildSetTimeCommand(9, 30);
  wire.insert(wire.end(), next.begin(), next.end());
  FrameParser parser;
  std::vector<ParseResult> results;
  for (uint8_t byte : wire) {
    auto result = parser.ingest(byte);
    if (result.has_value()) {
      results.push_back(*result);
    }
  }
  TEST_ASSERT_EQUAL(2, results.size());
  TEST_ASSERT_EQUAL(ParseError::None, results[1].error);
  TEST_ASSERT_EQUAL_HEX8(MessageSetTime, results[1].frame->type);
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_crc_matches_known_status_frame_fixture);
  RUN_TEST(test_decode_known_status_frame_fixture);
  RUN_TEST(test_decode_captured_panel_frames);
  RUN_TEST(test_encode_matches_captured_panel_button);
  RUN_TEST(test_encode_round_trips_toggle_item_command);
  RUN_TEST(test_encode_captured_temperature_commands);
  RUN_TEST(test_encode_set_time_command_uses_panel_addressing);
  RUN_TEST(test_encode_set_temperature_command_uses_panel_addressing);
  RUN_TEST(test_encode_filter_cycles_request_uses_panel_addressing);
  RUN_TEST(test_encode_set_filter_cycles_command_packs_filter2_enable_bit);
  RUN_TEST(test_stream_parser_emits_complete_frame);
  RUN_TEST(test_decode_captured_status_update);
  RUN_TEST(test_decode_status_fields_do_not_alias_the_clock);
  RUN_TEST(test_decode_hold_and_ready_in_rest);
  RUN_TEST(test_decode_short_status_is_rejected);
  RUN_TEST(test_decode_device_configuration_single_two_speed_pump);
  RUN_TEST(test_decode_device_configuration_full_spa);
  RUN_TEST(test_decode_system_information);
  RUN_TEST(test_decode_setup_parameters);
  RUN_TEST(test_decode_fault_log_entry);
  RUN_TEST(test_encode_configuration_request);
  RUN_TEST(test_stream_parser_keeps_frame_whose_crc_is_the_delimiter);
  RUN_TEST(test_stream_parser_recovers_after_frame_with_delimiter_crc);
  return UNITY_END();
}
