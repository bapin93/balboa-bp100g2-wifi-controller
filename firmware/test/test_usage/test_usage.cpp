#include <unity.h>

#include "scheduler/usage_model.h"

constexpr uint32_t T0 = 1793000000;
constexpr uint32_t Day = 86400;

// Runs one day, with the tub used for half an hour starting at each of `useSlots`.
void runDay(UsageModel &model, uint32_t date, bool weekend, uint32_t dayStart, std::initializer_list<int> useSlots) {
  for (int slot = 0; slot < UsageModel::Slots; ++slot) {
    bool inUse = false;
    for (int use : useSlots) inUse |= use == slot;
    model.observe(dayStart + slot * 1800, date, weekend, slot, inUse, false);
    model.observe(dayStart + slot * 1800 + 900, date, weekend, slot, inUse, false);
  }
}

void setUp() {}
void tearDown() {}

void test_priors_match_what_the_owner_said() {
  UsageModel model;
  TEST_ASSERT_EQUAL_FLOAT(0.5f, model.readyWithinHours(false, 40));  // weekday 8 PM
  TEST_ASSERT_EQUAL_FLOAT(0.5f, model.readyWithinHours(true, 40));
  TEST_ASSERT_EQUAL_FLOAT(2.0f, model.readyWithinHours(false, 24));  // weekday noon: rarely
  TEST_ASSERT_EQUAL_FLOAT(2.0f, model.readyWithinHours(true, 24));   // weekend noon: sometimes
  TEST_ASSERT_TRUE(model.likelihood(true, 24) > model.likelihood(false, 24));
  TEST_ASSERT_EQUAL_FLOAT(4.0f, model.readyWithinHours(false, 6));   // 3 AM
  TEST_ASSERT_EQUAL_FLOAT(4.0f, model.readyWithinHours(false, 16));  // 8 AM
}

void test_learns_a_new_habit_and_forgets_an_old_one() {
  UsageModel model;
  uint32_t date = 20261102;
  for (int day = 0; day < 30; ++day) {
    runDay(model, date + day, false, T0 + day * Day, {14});  // 7 AM soaks, never evenings
  }
  model.observe(T0 + 30 * Day, date + 30, false, 0, false, false);  // the last day gets folded in
  TEST_ASSERT_EQUAL_FLOAT(0.5f, model.readyWithinHours(false, 14));
  TEST_ASSERT_TRUE(model.likelihood(false, 15) > model.likelihood(false, 16));  // neighbours get half
  TEST_ASSERT_TRUE(model.readyWithinHours(false, 40) > 0.5f);
  // Weekends weren't touched.
  TEST_ASSERT_EQUAL_FLOAT(0.5f, model.readyWithinHours(true, 40));
}

void test_one_soak_counts_once() {
  UsageModel model;
  // On for a while, off briefly, back on: still the same soak.
  model.observe(T0, 20261102, false, 40, true, false);
  model.observe(T0 + 600, 20261102, false, 40, false, false);
  model.observe(T0 + 1900, 20261102, false, 41, true, false);
  JsonDocument doc;
  model.toJson(doc.to<JsonObject>());
  TEST_ASSERT_EQUAL_STRING("10000000000", doc["started"]);  // only slot 40
  // A manual raise also marks a start, after a quiet spell.
  model.observe(T0 + 4 * 3600, 20261102, false, 46, false, true);
  doc.clear();
  model.toJson(doc.to<JsonObject>());
  TEST_ASSERT_EQUAL_STRING("410000000000", doc["started"]);
}

void test_json_round_trip() {
  UsageModel model;
  runDay(model, 20261102, false, T0, {14});
  runDay(model, 20261103, false, T0 + Day, {14});
  JsonDocument doc;
  model.toJson(doc.to<JsonObject>());
  UsageModel loaded;
  loaded.fromJson(doc.as<JsonVariantConst>());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, model.likelihood(false, 14), loaded.likelihood(false, 14));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, model.likelihood(true, 30), loaded.likelihood(true, 30));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_priors_match_what_the_owner_said);
  RUN_TEST(test_learns_a_new_habit_and_forgets_an_old_one);
  RUN_TEST(test_one_soak_counts_once);
  RUN_TEST(test_json_round_trip);
  return UNITY_END();
}
