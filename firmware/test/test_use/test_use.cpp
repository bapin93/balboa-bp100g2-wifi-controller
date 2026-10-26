#include <unity.h>

#include "scheduler/use_detector.h"

constexpr uint32_t T0 = 1793000000;

void setUp() {}
void tearDown() {}

void test_obvious_signs() {
  UseDetector detector;
  UseSignals signals;
  TEST_ASSERT_FALSE(detector.update(T0, signals));
  signals.jet1Speed = 2;
  TEST_ASSERT_TRUE(detector.update(T0, signals));
  signals = UseSignals();
  signals.light = true;
  TEST_ASSERT_TRUE(detector.update(T0, signals));
}

void test_low_jets_count_after_a_while() {
  UseDetector detector;
  UseSignals low;
  low.jet1Speed = 1;
  TEST_ASSERT_FALSE(detector.update(T0, low));  // could be the spa checking the temperature
  TEST_ASSERT_FALSE(detector.update(T0 + 9 * 60, low));
  TEST_ASSERT_TRUE(detector.update(T0 + 10 * 60, low));
  TEST_ASSERT_TRUE(detector.inUse());
}

void test_low_jets_for_heating_or_filtering_dont() {
  UseDetector detector;
  UseSignals heating;
  heating.jet1Speed = 1;
  heating.heating = true;
  UseSignals filtering;
  filtering.jet1Speed = 1;
  filtering.filtering = true;
  for (uint32_t s = 0; s < 3 * 3600; s += 60) {
    TEST_ASSERT_FALSE(detector.update(T0 + s, s < 3600 ? heating : filtering));
  }
  // Heating stops but the pump keeps going: the clock starts then.
  UseSignals low;
  low.jet1Speed = 1;
  TEST_ASSERT_FALSE(detector.update(T0 + 3 * 3600, low));
  TEST_ASSERT_TRUE(detector.update(T0 + 3 * 3600 + 10 * 60, low));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_obvious_signs);
  RUN_TEST(test_low_jets_count_after_a_while);
  RUN_TEST(test_low_jets_for_heating_or_filtering_dont);
  return UNITY_END();
}
