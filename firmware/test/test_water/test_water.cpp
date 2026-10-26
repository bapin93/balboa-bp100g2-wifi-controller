#include <unity.h>

#include <ArduinoJson.h>

#include "storage/water_log.h"

constexpr uint32_t Day = 86400;
constexpr uint32_t T0 = 1760000000;

void test_reading_keeps_only_pads_that_were_read() {
  WaterLog log;
  TEST_ASSERT_NULL(log.addReading(T0, 1.0f, 7.4f, NAN, NAN, "  after rain "));
  const WaterEntry &entry = log.entries().back();
  TEST_ASSERT_EQUAL_FLOAT(1.0f, entry.sanitizer);
  TEST_ASSERT_EQUAL_FLOAT(7.4f, entry.ph);
  TEST_ASSERT_TRUE(isnan(entry.alkalinity));
  TEST_ASSERT_EQUAL_STRING("after rain", entry.note.c_str());

  JsonDocument doc;
  log.toJson(doc.to<JsonObject>());
  JsonObjectConst item = doc["entries"][0];
  TEST_ASSERT_FALSE(item["ta"].is<float>());
  TEST_ASSERT_FALSE(item["th"].is<float>());
}

void test_rejects_bad_readings_and_doses() {
  WaterLog log;
  TEST_ASSERT_NOT_NULL(log.addReading(T0, NAN, NAN, NAN, NAN, ""));  // nothing read
  TEST_ASSERT_NOT_NULL(log.addReading(T0, NAN, 10.0f, NAN, NAN, ""));
  TEST_ASSERT_NOT_NULL(log.addReading(T0, -1.0f, NAN, NAN, NAN, ""));
  TEST_ASSERT_NOT_NULL(log.addDose(T0, "", 1, "oz", ""));
  TEST_ASSERT_NOT_NULL(log.addDose(T0, "pH Down", 0, "oz", ""));
  TEST_ASSERT_NOT_NULL(log.addDose(T0, "pH Down", 1, "", ""));
  TEST_ASSERT_EQUAL_UINT32(0, log.entries().size());
  TEST_ASSERT_NULL(log.addDose(T0, " pH Down ", 2, "tbsp", ""));
  TEST_ASSERT_EQUAL_STRING("pH Down", log.entries().back().chemical.c_str());
}

void test_replacing_a_cartridge_resets_its_pump_time_only() {
  WaterLog log;
  TEST_ASSERT_NULL(log.replaceCartridge(CartridgeSlot::Sanitizer, T0, 3));
  TEST_ASSERT_NULL(log.replaceCartridge(CartridgeSlot::Mineral, T0, -1));
  log.addPumpTime(3600);
  TEST_ASSERT_NULL(log.replaceCartridge(CartridgeSlot::Sanitizer, T0 + 28 * Day, -1));
  log.addPumpTime(60);
  TEST_ASSERT_EQUAL_UINT32(60, log.cartridge(CartridgeSlot::Sanitizer).pumpSec);
  TEST_ASSERT_EQUAL_UINT32(3660, log.cartridge(CartridgeSlot::Mineral).pumpSec);
  TEST_ASSERT_EQUAL_UINT8(3, log.cartridge(CartridgeSlot::Sanitizer).dial);  // -1 keeps the dial
  TEST_ASSERT_EQUAL_UINT8(3, log.entries().back().dial);
  TEST_ASSERT_NOT_NULL(log.replaceCartridge(CartridgeSlot::Sanitizer, T0, 7));
}

void test_pump_time_only_counts_for_frog() {
  WaterLog log;
  log.addPumpTime(100);  // nothing installed
  TEST_ASSERT_EQUAL_UINT32(0, log.cartridge(CartridgeSlot::Mineral).pumpSec);
  log.replaceCartridge(CartridgeSlot::Mineral, T0, 0);
  TEST_ASSERT_NULL(log.setSystem("chlorine"));
  log.clearDirty();
  log.addPumpTime(100);
  TEST_ASSERT_EQUAL_UINT32(0, log.cartridge(CartridgeSlot::Mineral).pumpSec);
  TEST_ASSERT_FALSE(log.dirty());
  TEST_ASSERT_NOT_NULL(log.setSystem("salt"));
  TEST_ASSERT_EQUAL(SanitizerSystem::Chlorine, log.system());
}

void test_dial_and_life_are_validated() {
  WaterLog log;
  TEST_ASSERT_EQUAL_UINT8(DialUnknown, log.cartridge(CartridgeSlot::Sanitizer).dial);
  TEST_ASSERT_NOT_NULL(log.setDial(T0, 7));
  TEST_ASSERT_NOT_NULL(log.setDial(T0, -1));
  TEST_ASSERT_NULL(log.setDial(T0, 5));
  TEST_ASSERT_EQUAL_UINT8(5, log.cartridge(CartridgeSlot::Sanitizer).dial);
  TEST_ASSERT_NULL(log.setDial(T0, 0));  // closed
  TEST_ASSERT_EQUAL_UINT8(0, log.cartridge(CartridgeSlot::Sanitizer).dial);

  // Closed survives a save and reload; never set stays unknown.
  log.replaceCartridge(CartridgeSlot::Sanitizer, T0 + Day, 0);
  JsonDocument doc;
  log.toJson(doc.to<JsonObject>());
  WaterLog restored;
  restored.fromJson(doc.as<JsonVariantConst>());
  TEST_ASSERT_EQUAL_UINT8(0, restored.cartridge(CartridgeSlot::Sanitizer).dial);
  TEST_ASSERT_EQUAL_UINT8(0, restored.entries().back().dial);
  TEST_ASSERT_EQUAL_UINT8(DialUnknown, WaterLog().cartridge(CartridgeSlot::Sanitizer).dial);
  TEST_ASSERT_EQUAL_UINT16(WaterLog::DefaultMineralLifeDays, log.cartridge(CartridgeSlot::Mineral).lifeDays);
  TEST_ASSERT_NOT_NULL(log.setLifeDays(CartridgeSlot::Mineral, 0));
  TEST_ASSERT_NULL(log.setLifeDays(CartridgeSlot::Mineral, 90));
  TEST_ASSERT_EQUAL_UINT16(90, log.cartridge(CartridgeSlot::Mineral).lifeDays);
}

void test_json_round_trip_and_delete() {
  WaterLog log;
  log.setSystem("bromine");
  log.replaceCartridge(CartridgeSlot::Sanitizer, T0, 4);
  log.addPumpTime(500);
  log.addReading(T0 + Day, 3.0f, 7.6f, 100, 150, "");
  log.addDose(T0 + Day, "Alkalinity Up", 1.5f, "oz", "per strip");
  log.setLifeDays(CartridgeSlot::Sanitizer, 35);

  JsonDocument doc;
  log.toJson(doc.to<JsonObject>());
  WaterLog restored;
  restored.fromJson(doc.as<JsonVariantConst>());
  TEST_ASSERT_EQUAL(SanitizerSystem::Bromine, restored.system());
  TEST_ASSERT_EQUAL_UINT32(T0, restored.cartridge(CartridgeSlot::Sanitizer).installedAt);
  TEST_ASSERT_EQUAL_UINT32(0, restored.cartridge(CartridgeSlot::Sanitizer).pumpSec);  // bromine: no credit
  TEST_ASSERT_EQUAL_UINT16(35, restored.cartridge(CartridgeSlot::Sanitizer).lifeDays);
  TEST_ASSERT_EQUAL_UINT16(WaterLog::DefaultMineralLifeDays, restored.cartridge(CartridgeSlot::Mineral).lifeDays);
  TEST_ASSERT_EQUAL_UINT32(3, restored.entries().size());
  const WaterEntry &dose = restored.entries()[2];
  TEST_ASSERT_EQUAL(WaterEntryType::Dose, dose.type);
  TEST_ASSERT_EQUAL_FLOAT(1.5f, dose.amount);
  TEST_ASSERT_EQUAL_STRING("oz", dose.unit.c_str());
  TEST_ASSERT_EQUAL_STRING("per strip", dose.note.c_str());
  TEST_ASSERT_EQUAL_FLOAT(150.0f, restored.entries()[1].hardness);

  const uint16_t readingId = restored.entries()[1].id;
  TEST_ASSERT_NULL(restored.deleteEntry(readingId));
  TEST_ASSERT_NOT_NULL(restored.deleteEntry(readingId));
  TEST_ASSERT_EQUAL_UINT32(2, restored.entries().size());
  restored.addReading(T0 + 2 * Day, NAN, 7.4f, NAN, NAN, "");
  TEST_ASSERT_EQUAL_UINT16(4, restored.entries().back().id);  // ids never reused
}

void test_history_drops_oldest_past_the_cap() {
  WaterLog log;
  for (size_t i = 0; i < WaterLog::MaxEntries + 5; ++i) {
    log.addReading(T0 + i, NAN, 7.4f, NAN, NAN, "");
  }
  TEST_ASSERT_EQUAL_UINT32(WaterLog::MaxEntries, log.entries().size());
  TEST_ASSERT_EQUAL_UINT32(T0 + 5, log.entries().front().ts);
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_reading_keeps_only_pads_that_were_read);
  RUN_TEST(test_rejects_bad_readings_and_doses);
  RUN_TEST(test_replacing_a_cartridge_resets_its_pump_time_only);
  RUN_TEST(test_pump_time_only_counts_for_frog);
  RUN_TEST(test_dial_and_life_are_validated);
  RUN_TEST(test_json_round_trip_and_delete);
  RUN_TEST(test_history_drops_oldest_past_the_cap);
  return UNITY_END();
}
