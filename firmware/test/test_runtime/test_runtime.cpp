#include <unity.h>

#include "storage/runtime_log.h"

constexpr uint32_t H0 = 1761400800;  // a top-of-hour Unix time

void test_accumulates_by_state() {
  RuntimeLog log;
  RuntimeSample heatingLow;
  heatingLow.heating = true;
  heatingLow.pump1Speed = 1;
  RuntimeSample highFilter;
  highFilter.pump1Speed = 2;
  highFilter.filterRunning = true;

  TEST_ASSERT_TRUE(log.add(20261025, H0, 10, heatingLow));
  TEST_ASSERT_FALSE(log.add(20261025, H0, 5, highFilter));
  TEST_ASSERT_FALSE(log.add(20261025, H0, 3, RuntimeSample()));

  TEST_ASSERT_EQUAL_UINT32(1, log.days().size());
  const RuntimeDay &day = log.days().back();
  TEST_ASSERT_EQUAL_UINT32(10, day.heatingSec);
  TEST_ASSERT_EQUAL_UINT32(10, day.pumpLowSec);
  TEST_ASSERT_EQUAL_UINT32(5, day.pumpHighSec);
  TEST_ASSERT_EQUAL_UINT32(5, day.filterSec);
}

void test_new_day_starts_fresh_and_keeps_history() {
  RuntimeLog log;
  RuntimeSample heating;
  heating.heating = true;
  log.add(20261025, H0, 60, heating);
  TEST_ASSERT_TRUE(log.add(20261026, H0, 30, heating));
  TEST_ASSERT_EQUAL_UINT32(2, log.days().size());
  TEST_ASSERT_EQUAL_UINT32(60, log.days().front().heatingSec);
  TEST_ASSERT_EQUAL_UINT32(30, log.days().back().heatingSec);
}

void test_history_is_capped() {
  RuntimeLog log;
  for (uint32_t i = 0; i < RuntimeLog::MaxDays + 5; ++i) {
    log.add(20260101 + i, H0, 1, RuntimeSample());
  }
  TEST_ASSERT_EQUAL_UINT32(RuntimeLog::MaxDays, log.days().size());
  TEST_ASSERT_EQUAL_UINT32(20260106, log.days().front().date);
}

void test_load_then_continue_today() {
  RuntimeDay saved;
  saved.date = 20261025;
  saved.heatingSec = 100;
  RuntimeLog log;
  log.load({saved});
  RuntimeSample heating;
  heating.heating = true;
  TEST_ASSERT_FALSE(log.add(20261025, H0, 20, heating));
  TEST_ASSERT_EQUAL_UINT32(120, log.days().back().heatingSec);
}

void test_water_temp_is_time_weighted() {
  RuntimeLog log;
  RuntimeSample sample;
  sample.waterTemp = 90.0f;
  log.add(20261025, H0, 30, sample);
  sample.waterTemp = 100.0f;
  log.add(20261025, H0, 10, sample);
  sample.waterTemp = NAN;  // unknown temps don't drag the average
  log.add(20261025, H0, 50, sample);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 92.5f, log.days().back().waterTempAvg);
  TEST_ASSERT_EQUAL_UINT32(40, log.days().back().waterTempSec);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 92.5f, log.hours().back().waterTempAvg);
}

void test_set_temp_is_time_weighted() {
  RuntimeLog log;
  RuntimeSample sample;
  sample.setTemp = 100.0f;
  log.add(20261025, H0, 45 * 60, sample);
  sample.setTemp = 96.0f;  // turned down for the last 15 minutes of the hour
  log.add(20261025, H0, 15 * 60, sample);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 99.0f, log.hours().back().setTempAvg);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 99.0f, log.days().back().setTempAvg);
  TEST_ASSERT_TRUE(isnan(log.days().back().waterTempAvg));  // independent of the water average
}

void test_hours_roll_and_cap() {
  RuntimeLog log;
  RuntimeSample heating;
  heating.heating = true;
  heating.waterTemp = 99.0f;
  for (uint32_t i = 0; i < RuntimeLog::MaxHours + 3; ++i) {
    log.add(20261025, H0 + i * 3600, 60, heating);
  }
  TEST_ASSERT_EQUAL_UINT32(RuntimeLog::MaxHours, log.hours().size());
  TEST_ASSERT_EQUAL_UINT32(H0 + 3 * 3600, log.hours().front().start);
  TEST_ASSERT_EQUAL_UINT32(60, log.hours().back().heatingSec);
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_accumulates_by_state);
  RUN_TEST(test_new_day_starts_fresh_and_keeps_history);
  RUN_TEST(test_history_is_capped);
  RUN_TEST(test_load_then_continue_today);
  RUN_TEST(test_water_temp_is_time_weighted);
  RUN_TEST(test_hours_roll_and_cap);
  RUN_TEST(test_set_temp_is_time_weighted);
  return UNITY_END();
}
