#include <unity.h>

#include <ArduinoJson.h>

#include "storage/config_model.h"

void test_valid_config_updates_capabilities() {
  DeviceConfig current;
  DeviceConfig next;
  const char *error = nullptr;

  JsonDocument doc;
  doc["timezone"] = "UTC0";
  doc["backlightTimeout"] = 5;
  JsonObject caps = doc["capabilities"].to<JsonObject>();
  caps["jet2"] = true;
  caps["light"] = false;

  TEST_ASSERT_TRUE(validateConfigJson(doc.as<JsonVariantConst>(), current, next, &error));
  TEST_ASSERT_NULL(error);
  TEST_ASSERT_EQUAL_STRING("UTC0", next.timezone);
  TEST_ASSERT_EQUAL_UINT8(5, next.backlightTimeout);
  TEST_ASSERT_TRUE(next.jet2);
  TEST_ASSERT_FALSE(next.light);
}

void test_rejects_bad_backlight_timeout() {
  DeviceConfig current;
  DeviceConfig next;
  const char *error = nullptr;

  JsonDocument doc;
  doc["backlightTimeout"] = 99;

  TEST_ASSERT_FALSE(validateConfigJson(doc.as<JsonVariantConst>(), current, next, &error));
  TEST_ASSERT_NOT_NULL(error);
}

void test_valid_config_updates_filter_cycles() {
  DeviceConfig current;
  DeviceConfig next;
  const char *error = nullptr;

  JsonDocument doc;
  JsonObject caps = doc["capabilities"].to<JsonObject>();
  caps["filterCycles"] = true;
  JsonObject filters = doc["filterCycles"].to<JsonObject>();
  filters["cycle1Start"] = 6;
  filters["cycle1Duration"] = 3;
  filters["cycle2Enabled"] = true;
  filters["cycle2Start"] = 18;
  filters["cycle2Duration"] = 2;

  TEST_ASSERT_TRUE(validateConfigJson(doc.as<JsonVariantConst>(), current, next, &error));
  TEST_ASSERT_NULL(error);
  TEST_ASSERT_TRUE(next.filterCycles);
  TEST_ASSERT_EQUAL_UINT8(6, next.filterCycle1Start);
  TEST_ASSERT_EQUAL_UINT8(3, next.filterCycle1Duration);
  TEST_ASSERT_TRUE(next.filterCycle2Enabled);
  TEST_ASSERT_EQUAL_UINT8(18, next.filterCycle2Start);
  TEST_ASSERT_EQUAL_UINT8(2, next.filterCycle2Duration);
}

void test_rejects_bad_filter_cycle_start() {
  DeviceConfig current;
  DeviceConfig next;
  const char *error = nullptr;

  JsonDocument doc;
  JsonObject filters = doc["filterCycles"].to<JsonObject>();
  filters["cycle1Start"] = 24;

  TEST_ASSERT_FALSE(validateConfigJson(doc.as<JsonVariantConst>(), current, next, &error));
  TEST_ASSERT_NOT_NULL(error);
}

void test_rejects_long_timezone() {
  DeviceConfig current;
  DeviceConfig next;
  const char *error = nullptr;

  JsonDocument doc;
  doc["timezone"] =
      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";

  TEST_ASSERT_FALSE(validateConfigJson(doc.as<JsonVariantConst>(), current, next, &error));
  TEST_ASSERT_NOT_NULL(error);
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_config_updates_capabilities);
  RUN_TEST(test_rejects_bad_backlight_timeout);
  RUN_TEST(test_valid_config_updates_filter_cycles);
  RUN_TEST(test_rejects_bad_filter_cycle_start);
  RUN_TEST(test_rejects_long_timezone);
  return UNITY_END();
}
