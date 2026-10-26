#include <unity.h>

#include <ArduinoJson.h>

#include "storage/maintenance.h"

constexpr uint32_t Day = 86400;
constexpr uint32_t T0 = 1760000000;

void test_add_and_validate_names() {
  MaintenanceLog log;
  uint16_t id = 0;
  TEST_ASSERT_NULL(log.addFilter("  Blue  ", T0, &id));
  TEST_ASSERT_EQUAL_UINT16(1, id);
  TEST_ASSERT_EQUAL_STRING("Blue", log.filters()[0].name.c_str());
  TEST_ASSERT_NOT_NULL(log.addFilter("   ", T0));
  TEST_ASSERT_NOT_NULL(log.addFilter("this name is far too long to fit", T0));
  TEST_ASSERT_EQUAL_UINT32(1, log.filters().size());
}

void test_swap_moves_pump_time_to_the_new_filter() {
  MaintenanceLog log;
  uint16_t a = 0;
  uint16_t b = 0;
  log.addFilter("A", T0, &a);
  log.addFilter("B", T0, &b);
  TEST_ASSERT_NULL(log.install(a, T0));
  log.addPumpTime(3600);
  TEST_ASSERT_NULL(log.swap(a, b, T0 + 14 * Day));
  log.addPumpTime(600);

  TEST_ASSERT_FALSE(log.filters()[0].inTub());
  TEST_ASSERT_TRUE(log.filters()[1].inTub());
  TEST_ASSERT_EQUAL_UINT32(3600, log.filters()[0].pumpSec);
  TEST_ASSERT_EQUAL_UINT32(600, log.filters()[1].pumpSec);
  TEST_ASSERT_EQUAL_UINT32(T0 + 14 * Day, log.filters()[0].stints.back().out);
  TEST_ASSERT_EQUAL_UINT32(T0 + 14 * Day, log.filters()[1].stints.back().in);
}

void test_clean_resets_only_since_clean_time() {
  MaintenanceLog log;
  uint16_t a = 0;
  log.addFilter("A", T0, &a);
  log.install(a, T0);
  log.addPumpTime(1000);
  TEST_ASSERT_NULL(log.clean(a, T0 + Day));
  log.addPumpTime(50);
  TEST_ASSERT_EQUAL_UINT32(1050, log.filters()[0].pumpSec);
  TEST_ASSERT_EQUAL_UINT32(50, log.filters()[0].pumpSecSinceClean);
  TEST_ASSERT_EQUAL_UINT32(T0 + Day, log.filters()[0].cleanedAt);
}

void test_rejects_impossible_history() {
  MaintenanceLog log;
  uint16_t a = 0;
  uint16_t b = 0;
  log.addFilter("A", T0, &a);
  log.addFilter("B", T0, &b);
  TEST_ASSERT_NOT_NULL(log.takeOut(a, T0));         // not in the tub
  TEST_ASSERT_NULL(log.install(a, T0 + Day));
  TEST_ASSERT_NOT_NULL(log.install(a, T0 + Day));   // already in
  TEST_ASSERT_NOT_NULL(log.takeOut(a, T0));         // before it went in
  TEST_ASSERT_NOT_NULL(log.swap(b, a, T0 + 2 * Day));  // b is not in the tub
  TEST_ASSERT_NOT_NULL(log.clean(a, T0 - Day));     // before it existed
  TEST_ASSERT_NOT_NULL(log.install(99, T0));
}

void test_out_of_tub_filters_do_not_accumulate() {
  MaintenanceLog log;
  log.addFilter("Spare", T0);
  log.clearDirty();
  log.addPumpTime(500);
  TEST_ASSERT_EQUAL_UINT32(0, log.filters()[0].pumpSec);
  TEST_ASSERT_FALSE(log.dirty());
}

void test_json_round_trip_keeps_history_and_ids() {
  MaintenanceLog log;
  uint16_t a = 0;
  uint16_t b = 0;
  log.addFilter("A", T0, &a);
  log.addFilter("B", T0, &b);
  log.install(a, T0);
  log.addPumpTime(42);
  log.clean(b, T0 + Day);
  log.deleteFilter(b, T0 + 2 * Day);

  JsonDocument doc;
  log.toJson(doc.to<JsonObject>());
  MaintenanceLog restored;
  restored.fromJson(doc.as<JsonVariantConst>());

  TEST_ASSERT_EQUAL_UINT32(1, restored.filters().size());
  TEST_ASSERT_TRUE(restored.filters()[0].inTub());
  TEST_ASSERT_EQUAL_UINT32(42, restored.filters()[0].pumpSec);
  TEST_ASSERT_EQUAL_UINT32(log.events().size(), restored.events().size());
  TEST_ASSERT_EQUAL(MaintenanceEventType::Deleted, restored.events().back().type);
  TEST_ASSERT_EQUAL_STRING("B", restored.events().back().name.c_str());
  uint16_t c = 0;
  restored.addFilter("C", T0, &c);
  TEST_ASSERT_EQUAL_UINT16(3, c);  // ids are never reused, even after a delete
}

void test_long_history_keeps_lifetime_in_tub_total() {
  MaintenanceLog log;
  uint16_t a = 0;
  log.addFilter("A", T0, &a);
  uint32_t t = T0;
  for (size_t i = 0; i < MaintenanceLog::MaxStints + 5; ++i) {
    log.install(a, t);
    log.takeOut(a, t + Day);
    t += 2 * Day;
  }
  const FilterRecord &filter = log.filters()[0];
  TEST_ASSERT_EQUAL_UINT32(MaintenanceLog::MaxStints, filter.stints.size());
  uint32_t total = 0;
  for (const FilterStint &stint : filter.stints) {
    total += stint.out - stint.in;
  }
  TEST_ASSERT_EQUAL_UINT32((MaintenanceLog::MaxStints + 5) * Day, total);
}

void test_tasks_track_last_done_and_history() {
  MaintenanceLog log;
  uint16_t id = 0;
  TEST_ASSERT_NULL(log.addTask("Drain and refill", 90, T0, &id));
  TEST_ASSERT_NOT_NULL(log.addTask("Never", 0, T0));
  TEST_ASSERT_NOT_NULL(log.addTask("Too long", 1000, T0));
  TEST_ASSERT_NULL(log.completeTask(id, T0 + 10 * Day));
  TEST_ASSERT_NULL(log.completeTask(id, T0 + 5 * Day));  // backdated entry after a newer one
  TEST_ASSERT_EQUAL_UINT32(T0 + 10 * Day, log.tasks()[0].lastDone);
  TEST_ASSERT_EQUAL_UINT32(2, log.events().size());
  TEST_ASSERT_EQUAL(MaintenanceEventType::Done, log.events().back().type);
  TEST_ASSERT_EQUAL_STRING("Drain and refill", log.events().back().name.c_str());
  TEST_ASSERT_NULL(log.editTask(id, "Water change", 120));
  TEST_ASSERT_EQUAL_UINT16(120, log.tasks()[0].intervalDays);
  TEST_ASSERT_NULL(log.deleteTask(id));
  TEST_ASSERT_TRUE(log.tasks().empty());
}

void test_default_tasks_are_added_once() {
  MaintenanceLog log;
  log.ensureDefaultTasks(T0);
  TEST_ASSERT_EQUAL_UINT32(3, log.tasks().size());
  log.deleteTask(log.tasks()[0].id);

  JsonDocument doc;
  log.toJson(doc.to<JsonObject>());
  MaintenanceLog restored;
  restored.fromJson(doc.as<JsonVariantConst>());
  restored.ensureDefaultTasks(T0);
  TEST_ASSERT_EQUAL_UINT32(2, restored.tasks().size());  // the deleted default stays deleted
}

void test_filter_and_task_ids_do_not_collide() {
  MaintenanceLog log;
  uint16_t filter = 0;
  uint16_t task = 0;
  log.addFilter("Blue", T0, &filter);
  log.addTask("Cover", 30, T0, &task);
  TEST_ASSERT_NOT_EQUAL(filter, task);
  JsonDocument doc;
  log.toJson(doc.to<JsonObject>());
  MaintenanceLog restored;
  restored.fromJson(doc.as<JsonVariantConst>());
  uint16_t next = 0;
  restored.addTask("Next", 7, T0, &next);
  TEST_ASSERT_EQUAL_UINT16(3, next);
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_add_and_validate_names);
  RUN_TEST(test_swap_moves_pump_time_to_the_new_filter);
  RUN_TEST(test_clean_resets_only_since_clean_time);
  RUN_TEST(test_rejects_impossible_history);
  RUN_TEST(test_out_of_tub_filters_do_not_accumulate);
  RUN_TEST(test_json_round_trip_keeps_history_and_ids);
  RUN_TEST(test_long_history_keeps_lifetime_in_tub_total);
  RUN_TEST(test_tasks_track_last_done_and_history);
  RUN_TEST(test_default_tasks_are_added_once);
  RUN_TEST(test_filter_and_task_ids_do_not_collide);
  return UNITY_END();
}
