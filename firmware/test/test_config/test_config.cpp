#include <unity.h>

#include <ArduinoJson.h>

#include "storage/config_model.h"

void test_valid_config_updates_capabilities() {
  DeviceConfig current;
  DeviceConfig next;
  const char *error = nullptr;

  JsonDocument doc;
  doc["timezone"] = "UTC0";
  JsonObject caps = doc["capabilities"].to<JsonObject>();
  caps["setTime"] = false;
  caps["hideJet2"] = true;

  TEST_ASSERT_TRUE(validateConfigJson(doc.as<JsonVariantConst>(), current, next, &error));
  TEST_ASSERT_NULL(error);
  TEST_ASSERT_EQUAL_STRING("UTC0", next.timezone);
  TEST_ASSERT_FALSE(next.setTime);
  TEST_ASSERT_TRUE(next.hideJet2);
}

void test_valid_config_updates_filter_cycles() {
  DeviceConfig current;
  DeviceConfig next;
  const char *error = nullptr;

  JsonDocument doc;
  JsonObject caps = doc["capabilities"].to<JsonObject>();
  JsonObject filters = doc["filterCycles"].to<JsonObject>();
  filters["cycle1Start"] = 6;
  filters["cycle1StartMinute"] = 15;
  filters["cycle1Duration"] = 3;
  filters["cycle1DurationMinute"] = 30;
  filters["cycle2Enabled"] = true;
  filters["cycle2Start"] = 18;
  filters["cycle2StartMinute"] = 45;
  filters["cycle2Duration"] = 2;
  filters["cycle2DurationMinute"] = 15;

  TEST_ASSERT_TRUE(validateConfigJson(doc.as<JsonVariantConst>(), current, next, &error));
  TEST_ASSERT_NULL(error);
  TEST_ASSERT_EQUAL_UINT8(6, next.filterCycle1Start);
  TEST_ASSERT_EQUAL_UINT8(15, next.filterCycle1StartMinute);
  TEST_ASSERT_EQUAL_UINT8(3, next.filterCycle1Duration);
  TEST_ASSERT_EQUAL_UINT8(30, next.filterCycle1DurationMinute);
  TEST_ASSERT_TRUE(next.filterCycle2Enabled);
  TEST_ASSERT_EQUAL_UINT8(18, next.filterCycle2Start);
  TEST_ASSERT_EQUAL_UINT8(45, next.filterCycle2StartMinute);
  TEST_ASSERT_EQUAL_UINT8(2, next.filterCycle2Duration);
  TEST_ASSERT_EQUAL_UINT8(15, next.filterCycle2DurationMinute);
}

void test_rejects_bad_filter_cycle_minute() {
  DeviceConfig current;
  DeviceConfig next;
  const char *error = nullptr;

  JsonDocument doc;
  JsonObject filters = doc["filterCycles"].to<JsonObject>();
  filters["cycle1StartMinute"] = 10;

  TEST_ASSERT_FALSE(validateConfigJson(doc.as<JsonVariantConst>(), current, next, &error));
  TEST_ASSERT_NOT_NULL(error);
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

void test_location_set_clear_and_validate() {
  DeviceConfig current;
  DeviceConfig next;
  const char *error = nullptr;

  JsonDocument set;
  set["location"]["lat"] = 39.74;
  set["location"]["lon"] = -104.99;
  TEST_ASSERT_TRUE(validateConfigJson(set.as<JsonVariantConst>(), current, next, &error));
  TEST_ASSERT_TRUE(next.hasLocation);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -104.99f, next.longitude);

  JsonDocument out;
  configToJson(next, out.to<JsonObject>());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 39.74f, out["location"]["lat"].as<float>());

  JsonDocument bad;
  bad["location"]["lat"] = 120;
  bad["location"]["lon"] = 0;
  TEST_ASSERT_FALSE(validateConfigJson(bad.as<JsonVariantConst>(), next, current, &error));

  JsonDocument clear;
  clear["location"].to<JsonObject>();
  DeviceConfig cleared;
  TEST_ASSERT_TRUE(validateConfigJson(clear.as<JsonVariantConst>(), next, cleared, &error));
  TEST_ASSERT_FALSE(cleared.hasLocation);

  JsonDocument untouched;
  untouched["timezone"] = "UTC0";
  DeviceConfig kept;
  TEST_ASSERT_TRUE(validateConfigJson(untouched.as<JsonVariantConst>(), next, kept, &error));
  TEST_ASSERT_TRUE(kept.hasLocation);
}

void test_tou_rates_and_power() {
  DeviceConfig current;
  DeviceConfig next;
  const char *error = nullptr;

  JsonDocument doc;
  doc["tou"]["holidays"] = "nerc";
  doc["tou"]["weekendsOffPeak"] = false;
  doc["tou"]["rates"]["summerPeak"] = 0.21277;
  doc["power"]["heaterKw"] = 5.5;
  doc["power"]["standbyW"] = 40;
  doc["power"]["tubGallons"] = 250;
  TEST_ASSERT_TRUE(validateConfigJson(doc.as<JsonVariantConst>(), current, next, &error));
  TEST_ASSERT_EQUAL(HolidaySet::Nerc, next.tou.holidays);
  TEST_ASSERT_FALSE(next.tou.weekendsOffPeak);
  TEST_ASSERT_EQUAL_UINT8(17, next.tou.peakStart);  // untouched
  TEST_ASSERT_EQUAL_FLOAT(0.21277f, next.rates.summerPeak);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, next.rates.winterPeak);
  TEST_ASSERT_EQUAL_FLOAT(5.5f, next.power.heaterKw);
  TEST_ASSERT_EQUAL_UINT16(40, next.power.standbyW);
  TEST_ASSERT_EQUAL_UINT16(250, next.power.tubGallons);

  // Survives the save/load round trip.
  JsonDocument saved;
  configToJson(next, saved.to<JsonObject>());
  DeviceConfig loaded;
  configFromJson(saved.as<JsonVariantConst>(), loaded);
  TEST_ASSERT_EQUAL(HolidaySet::Nerc, loaded.tou.holidays);
  TEST_ASSERT_EQUAL_FLOAT(0.21277f, loaded.rates.summerPeak);
  TEST_ASSERT_EQUAL_FLOAT(5.5f, loaded.power.heaterKw);
  TEST_ASSERT_EQUAL_UINT16(250, loaded.power.tubGallons);

  JsonDocument tiny;
  tiny["power"]["tubGallons"] = 10;
  TEST_ASSERT_FALSE(validateConfigJson(tiny.as<JsonVariantConst>(), current, next, &error));

  JsonDocument bad;
  bad["tou"]["peakStart"] = 24;
  TEST_ASSERT_FALSE(validateConfigJson(bad.as<JsonVariantConst>(), current, next, &error));
  JsonDocument badRate;
  badRate["tou"]["rates"]["winterOffPeak"] = -0.1;
  TEST_ASSERT_FALSE(validateConfigJson(badRate.as<JsonVariantConst>(), current, next, &error));
  JsonDocument badHoliday;
  badHoliday["tou"]["holidays"] = "bank";
  TEST_ASSERT_FALSE(validateConfigJson(badHoliday.as<JsonVariantConst>(), current, next, &error));
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_config_updates_capabilities);
  RUN_TEST(test_valid_config_updates_filter_cycles);
  RUN_TEST(test_rejects_bad_filter_cycle_start);
  RUN_TEST(test_rejects_bad_filter_cycle_minute);
  RUN_TEST(test_rejects_long_timezone);
  RUN_TEST(test_location_set_clear_and_validate);
  RUN_TEST(test_tou_rates_and_power);
  return UNITY_END();
}
