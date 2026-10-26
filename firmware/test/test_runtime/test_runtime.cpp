#include <unity.h>

#include "storage/runtime_log.h"

void test_accumulates_by_state() {
  RuntimeLog log;
  RuntimeSample heatingLow;
  heatingLow.heating = true;
  heatingLow.pump1Speed = 1;
  RuntimeSample highFilter;
  highFilter.pump1Speed = 2;
  highFilter.filterRunning = true;

  TEST_ASSERT_TRUE(log.add(20261025, 10, heatingLow));
  TEST_ASSERT_FALSE(log.add(20261025, 5, highFilter));
  TEST_ASSERT_FALSE(log.add(20261025, 3, RuntimeSample()));

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
  log.add(20261025, 60, heating);
  TEST_ASSERT_TRUE(log.add(20261026, 30, heating));
  TEST_ASSERT_EQUAL_UINT32(2, log.days().size());
  TEST_ASSERT_EQUAL_UINT32(60, log.days().front().heatingSec);
  TEST_ASSERT_EQUAL_UINT32(30, log.days().back().heatingSec);
}

void test_history_is_capped() {
  RuntimeLog log;
  for (uint32_t i = 0; i < RuntimeLog::MaxDays + 5; ++i) {
    log.add(20260101 + i, 1, RuntimeSample());
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
  TEST_ASSERT_FALSE(log.add(20261025, 20, heating));
  TEST_ASSERT_EQUAL_UINT32(120, log.days().back().heatingSec);
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_accumulates_by_state);
  RUN_TEST(test_new_day_starts_fresh_and_keeps_history);
  RUN_TEST(test_history_is_capped);
  RUN_TEST(test_load_then_continue_today);
  return UNITY_END();
}
