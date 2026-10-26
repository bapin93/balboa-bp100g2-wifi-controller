#include <unity.h>

#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "scheduler/heat_controller.h"

constexpr uint32_t Monday8am = 1793023200;  // 2026-10-26 08:00 MDT
constexpr uint32_t Hour = 3600;
constexpr uint32_t Ready730pm = Monday8am + 11 * Hour + 1800;

PlanContext context() {
  PlanContext ctx;
  ctx.rates.winterPeak = 0.18331f;
  ctx.rates.winterOffPeak = 0.06792f;
  ctx.heaterRate = 6.0f;
  ctx.lossCoeff = 0.012f;
  ctx.minTemp = 90;
  ctx.outsideAt = [](uint32_t) { return 45.0f; };
  return ctx;
}

// A spa that heats toward its set temp like the real one and applies set temps the controller sends.
struct Spa {
  HeatController &controller;
  PlanContext ctx = context();
  float setTemp = 100;
  float water = 95;
  uint32_t now = Monday8am;
  bool inUse = false;
  int commands = 0;
  int heatedMinutes[48] = {};  // [h] covers the hour starting h hours after 8 AM Monday

  ControllerTick tick() const {
    ControllerTick t;
    t.now = now;
    t.setTemp = setTemp;
    t.waterTemp = water;
    t.inUse = inUse;
    return t;
  }

  // Runs `seconds` a minute at a time.
  void run(uint32_t seconds) {
    for (uint32_t s = 0; s < seconds; s += 60) {
      const float sent = controller.tick(tick(), ctx);
      if (!isnan(sent)) {
        controller.commandSent(sent, now);
        setTemp = sent;
        ++commands;
      }
      const bool heating = water < setTemp - 0.5f;
      if (heating && (now - Monday8am) / Hour < 48) ++heatedMinutes[(now - Monday8am) / Hour];
      water += ((heating ? ctx.heaterRate : 0) - ctx.lossCoeff * (water - 45)) / 60.0f;
      now += 60;
    }
  }
};

void setUp() {
  setenv("TZ", "MST7MDT,M3.2.0/2,M11.1.0/2", 1);
  tzset();
}

void tearDown() {}

void test_soak_follows_the_plan_and_restores() {
  HeatController controller;
  Spa spa{controller};
  TEST_ASSERT_NULL(controller.scheduleSoak(104, Ready730pm, 2, spa.tick(), spa.ctx));
  spa.run(60);
  TEST_ASSERT_TRUE(spa.setTemp < 95);  // coasting through the morning
  TEST_ASSERT_TRUE(spa.setTemp >= 90);  // but never under the floor
  spa.run(5 * Hour);  // 1 PM
  TEST_ASSERT_TRUE(spa.setTemp < 95);
  spa.run(4 * Hour);  // 5 PM, the start of the peak
  TEST_ASSERT_TRUE(spa.water > 103);
  spa.run(2 * Hour + 1800);  // 7:30 PM
  TEST_ASSERT_TRUE(spa.water > 103);
  TEST_ASSERT_EQUAL_FLOAT(104, spa.setTemp);
  TEST_ASSERT_TRUE(controller.soakActive());
  TEST_ASSERT_FALSE(controller.manualActive());  // its own commands aren't mistaken for someone else's
  spa.run(2 * Hour + 120);  // the soak is over
  TEST_ASSERT_FALSE(controller.soakActive());
  TEST_ASSERT_EQUAL_FLOAT(100, spa.setTemp);  // back where it started
  TEST_ASSERT_TRUE(spa.commands < 20);  // a handful of changes, not churn
}

void test_soak_runs_on_while_in_use() {
  HeatController controller;
  Spa spa{controller};
  spa.water = 104;
  controller.scheduleSoak(104, spa.now + Hour, 1, spa.tick(), spa.ctx);
  spa.run(Hour + 600);
  spa.inUse = true;
  spa.run(2 * Hour);  // still in after the scheduled end
  TEST_ASSERT_TRUE(controller.soakActive());
  TEST_ASSERT_EQUAL_FLOAT(104, spa.setTemp);
  spa.inUse = false;
  spa.run(20 * 60);
  TEST_ASSERT_TRUE(controller.soakActive());  // 30 minutes' grace for getting out
  spa.run(15 * 60);
  TEST_ASSERT_FALSE(controller.soakActive());
  TEST_ASSERT_EQUAL_FLOAT(100, spa.setTemp);
}

void test_panel_change_takes_over() {
  HeatController controller;
  Spa spa{controller};
  controller.scheduleSoak(104, Ready730pm, 2, spa.tick(), spa.ctx);
  spa.run(Hour);
  spa.setTemp = 102;  // someone at the spa panel
  spa.run(60);
  TEST_ASSERT_FALSE(controller.soakActive());
  TEST_ASSERT_TRUE(controller.manualActive());
  TEST_ASSERT_EQUAL_FLOAT(102, controller.comfortTemp());
  const int before = spa.commands;
  spa.run(2 * Hour);
  TEST_ASSERT_EQUAL_INT(before, spa.commands);  // hands off
  TEST_ASSERT_EQUAL_FLOAT(102, spa.setTemp);
}

void test_manual_hold_ends_an_hour_after_use() {
  HeatController controller;
  Spa spa{controller};
  controller.scheduleSoak(104, Ready730pm, 2, spa.tick(), spa.ctx);
  spa.run(60);
  spa.setTemp = 103;
  spa.inUse = true;
  spa.run(Hour);
  spa.inUse = false;
  spa.run(50 * 60);
  TEST_ASSERT_TRUE(controller.manualActive());
  spa.run(15 * 60);
  TEST_ASSERT_FALSE(controller.manualActive());
}

void test_manual_hold_lasts_four_hours_if_unused() {
  HeatController controller;
  Spa spa{controller};
  controller.scheduleSoak(104, Ready730pm, 2, spa.tick(), spa.ctx);
  spa.run(60);
  spa.setTemp = 103;
  spa.run(3 * Hour + 50 * 60);
  TEST_ASSERT_TRUE(controller.manualActive());
  spa.run(20 * 60);
  TEST_ASSERT_FALSE(controller.manualActive());
}

void test_cancel_restores_unless_in_use() {
  HeatController controller;
  Spa spa{controller};
  controller.scheduleSoak(104, 0, 2, spa.tick(), spa.ctx);
  spa.run(120);
  TEST_ASSERT_EQUAL_FLOAT(104, spa.setTemp);  // ASAP heats right away
  controller.cancelSoak(spa.tick());
  spa.run(120);
  TEST_ASSERT_EQUAL_FLOAT(100, spa.setTemp);

  controller.scheduleSoak(104, 0, 2, spa.tick(), spa.ctx);
  spa.run(120);
  spa.inUse = true;
  controller.cancelSoak(spa.tick());
  spa.run(Hour);
  TEST_ASSERT_EQUAL_FLOAT(104, spa.setTemp);  // left alone with someone in
  TEST_ASSERT_TRUE(controller.manualActive());
}

void test_asap_ready_time() {
  HeatController controller;
  Spa spa{controller};
  controller.scheduleSoak(104, 0, 2, spa.tick(), spa.ctx);
  JsonDocument doc;
  controller.writeStateJson(doc.to<JsonObject>(), spa.now);
  const uint32_t readyAt = doc["soak"]["readyAt"];
  // 9°F at a bit over 5°F/hr net.
  TEST_ASSERT_TRUE(readyAt > spa.now + 90 * 60);
  TEST_ASSERT_TRUE(readyAt < spa.now + 2 * Hour + 1800);
  TEST_ASSERT_TRUE(doc["soak"]["asap"].as<bool>());
}

void test_rejects_bad_requests() {
  HeatController controller;
  Spa spa{controller};
  TEST_ASSERT_NOT_NULL(controller.scheduleSoak(110, 0, 2, spa.tick(), spa.ctx));
  TEST_ASSERT_NOT_NULL(controller.scheduleSoak(104, spa.now - 60, 2, spa.tick(), spa.ctx));
  TEST_ASSERT_NOT_NULL(controller.scheduleSoak(104, spa.now + 40 * Hour, 2, spa.tick(), spa.ctx));
  TEST_ASSERT_NOT_NULL(controller.scheduleSoak(104, 0, 10, spa.tick(), spa.ctx));
  TEST_ASSERT_FALSE(controller.soakActive());
}

void test_state_survives_a_reboot() {
  HeatController controller;
  Spa spa{controller};
  controller.scheduleSoak(104, Ready730pm, 2, spa.tick(), spa.ctx);
  TEST_ASSERT_TRUE(controller.dirty());
  JsonDocument doc;
  controller.toJson(doc.to<JsonObject>());

  HeatController rebooted;
  rebooted.fromJson(doc.as<JsonVariantConst>());
  TEST_ASSERT_TRUE(rebooted.soakActive());
  Spa after{rebooted};
  after.now = spa.now + 600;
  after.run(8 * Hour + 50 * 60);  // 5 PM
  TEST_ASSERT_TRUE(after.water > 103);
  after.run(5 * Hour);
  TEST_ASSERT_FALSE(rebooted.soakActive());
  TEST_ASSERT_EQUAL_FLOAT(100, after.setTemp);  // restore temp survived too
}

// Evenings are likely (ready within half an hour); otherwise four hours is fine.
PlanContext autoContext() {
  PlanContext ctx = context();
  ctx.autoEnabled = true;
  ctx.readyWithinHours = [](uint32_t at) {
    const time_t t = at;
    tm local;
    localtime_r(&t, &local);
    return local.tm_hour >= 18 && local.tm_hour < 23 ? 0.5f : 4.0f;
  };
  return ctx;
}

int heated(const Spa &spa, uint32_t fromHour, uint32_t toHour) {
  int minutes = 0;
  for (uint32_t h = fromHour; h < toHour; ++h) minutes += spa.heatedMinutes[h];
  return minutes;
}

void test_auto_sets_back_and_is_ready_for_the_evening() {
  HeatController controller;
  Spa spa{controller};
  spa.ctx = autoContext();
  spa.setTemp = 102;  // the comfort temp as found
  spa.water = 101;
  spa.run(Hour);
  TEST_ASSERT_EQUAL_FLOAT(102, controller.comfortTemp());
  TEST_ASSERT_TRUE(spa.setTemp < 98);  // set back for the workday
  spa.run(9 * Hour);  // 6 PM
  // Within half an hour of 102: about 99.3°F or warmer.
  TEST_ASSERT_TRUE(spa.water > 99.0f);
  for (int i = 0; i < 10; ++i) {
    spa.run(1800);
    TEST_ASSERT_TRUE(spa.water > 99.0f);  // all evening
  }
  // The heat for the evening went in before the 5 PM peak; the peak only covers losses.
  TEST_ASSERT_TRUE(heated(spa, 9, 13) < 30);  // 5-9 PM
  TEST_ASSERT_TRUE(heated(spa, 0, 9) > 45);   // before 5 PM
  JsonDocument doc;
  controller.writePlanJson(doc.to<JsonObject>());
  // Standing by costs less than holding 102 all the time over the same hours.
  JsonVariantConst compare = doc["plan"]["compare"];
  TEST_ASSERT_TRUE(compare["smartCost"].as<float>() < compare["holdCost"].as<float>());
  TEST_ASSERT_TRUE(compare["smartCost"].as<float>() >= doc["plan"]["cost"].as<float>());  // ending warm costs more
  TEST_ASSERT_EQUAL_UINT32(144, doc["plan"]["steps"].size());
  spa.run(8 * Hour);  // 7 AM Tuesday
  TEST_ASSERT_TRUE(spa.water < 97.0f);  // let it cool overnight
  TEST_ASSERT_TRUE(spa.water >= 89.5f);
}

void test_auto_pauses_for_a_manual_raise() {
  HeatController controller;
  Spa spa{controller};
  spa.ctx = autoContext();
  spa.setTemp = 102;
  spa.water = 101;
  spa.run(Hour);
  spa.setTemp = 104;  // turned up by hand
  spa.run(60);
  TEST_ASSERT_TRUE(controller.manualActive());
  TEST_ASSERT_EQUAL_FLOAT(104, controller.comfortTemp());  // the new comfort temp
  TEST_ASSERT_TRUE(controller.lastIntentAt() > 0);
  spa.inUse = true;
  spa.run(2 * Hour);
  spa.inUse = false;
  spa.run(30 * 60);
  TEST_ASSERT_EQUAL_FLOAT(104, spa.setTemp);
  spa.run(45 * 60);  // an hour after getting out
  TEST_ASSERT_FALSE(controller.manualActive());
  TEST_ASSERT_TRUE(spa.setTemp < 104);  // Auto is back
}

void test_turning_auto_off_puts_comfort_back() {
  HeatController controller;
  Spa spa{controller};
  spa.ctx = autoContext();
  spa.setTemp = 102;
  spa.water = 101;
  spa.run(Hour);
  TEST_ASSERT_TRUE(spa.setTemp < 102);
  spa.ctx.autoEnabled = false;
  spa.run(120);
  TEST_ASSERT_EQUAL_FLOAT(102, spa.setTemp);
  const int before = spa.commands;
  spa.run(5 * Hour);
  TEST_ASSERT_EQUAL_INT(before, spa.commands);
}

void test_auto_takes_over_after_a_soak() {
  HeatController controller;
  Spa spa{controller};
  spa.ctx = autoContext();
  spa.setTemp = 100;
  spa.water = 103;
  spa.run(60);
  controller.scheduleSoak(104, 0, 1, spa.tick(), spa.ctx);
  spa.run(30 * 60);
  TEST_ASSERT_EQUAL_FLOAT(104, spa.setTemp);
  spa.run(2 * Hour);
  TEST_ASSERT_FALSE(controller.soakActive());
  TEST_ASSERT_TRUE(spa.setTemp < 100);  // Auto's standby, not the old set temp
}

void test_low_range_stands_aside() {
  HeatController controller;
  Spa spa{controller};
  spa.ctx = autoContext();
  spa.setTemp = 102;
  spa.water = 101;
  spa.run(Hour);
  TEST_ASSERT_TRUE(spa.setTemp < 102);
  // Switched to low: the spa shows the low range's own set temp.
  spa.ctx.lowRange = true;
  spa.setTemp = 80;
  const int before = spa.commands;
  spa.run(24 * Hour);
  TEST_ASSERT_EQUAL_INT(before, spa.commands);  // no 90°F floor, nothing sent
  TEST_ASSERT_TRUE(spa.water < 90);
  TEST_ASSERT_FALSE(controller.manualActive());
  TEST_ASSERT_EQUAL_FLOAT(102, controller.comfortTemp());
  JsonDocument doc;
  controller.writeStateJson(doc.to<JsonObject>(), spa.now);
  TEST_ASSERT_EQUAL_STRING("lowRange", doc["mode"]);
  // Back to high, where the spa remembers its old set temp: Auto picks up, nothing counts as manual.
  spa.ctx.lowRange = false;
  spa.setTemp = 94;
  spa.run(Hour);
  TEST_ASSERT_FALSE(controller.manualActive());
  TEST_ASSERT_EQUAL_FLOAT(102, controller.comfortTemp());
  TEST_ASSERT_TRUE(spa.commands > before);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_soak_follows_the_plan_and_restores);
  RUN_TEST(test_soak_runs_on_while_in_use);
  RUN_TEST(test_panel_change_takes_over);
  RUN_TEST(test_manual_hold_ends_an_hour_after_use);
  RUN_TEST(test_manual_hold_lasts_four_hours_if_unused);
  RUN_TEST(test_cancel_restores_unless_in_use);
  RUN_TEST(test_asap_ready_time);
  RUN_TEST(test_rejects_bad_requests);
  RUN_TEST(test_state_survives_a_reboot);
  RUN_TEST(test_auto_sets_back_and_is_ready_for_the_evening);
  RUN_TEST(test_auto_pauses_for_a_manual_raise);
  RUN_TEST(test_turning_auto_off_puts_comfort_back);
  RUN_TEST(test_auto_takes_over_after_a_soak);
  RUN_TEST(test_low_range_stands_aside);
  return UNITY_END();
}
