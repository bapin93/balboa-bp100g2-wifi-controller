#include <unity.h>

#include <math.h>

#include "scheduler/thermal_model.h"

constexpr uint32_t T0 = 1793000000;

// Simulates the tub with known physics and feeds the model whole-degree readings like the spa reports.
struct Tub {
  ThermalModel &model;
  float heater;  // °F/hr
  float loss;    // per hour
  float outside;
  float water;
  uint32_t now = T0;

  void run(uint32_t seconds, bool heating, bool inUse = false, bool forecast = true) {
    for (uint32_t s = 0; s < seconds; s += 5) {
      const float rate = (heating ? heater : 0) - loss * (water - outside);
      water += rate * 5.0f / 3600.0f;
      now += 5;
      ThermalSample sample;
      sample.now = now;
      sample.waterTemp = floorf(water);
      sample.heating = heating;
      sample.inUse = inUse;
      sample.outside = forecast ? outside : NAN;
      model.observe(sample);
    }
  }
};

void test_seed_from_heater_and_volume() {
  ThermalModel model;
  model.setSeeds(5.5f, 200);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 11.25f, model.heaterRate());
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, ThermalModel::DefaultLossCoeff, model.lossCoeff());
  // Twice the water heats half as fast.
  model.setSeeds(5.5f, 400);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 5.63f, model.heaterRate());
}

void test_learns_heater_rate_from_a_heating_run() {
  ThermalModel model;
  model.setSeeds(5.5f, 200);
  Tub tub{model, 6.0f, ThermalModel::DefaultLossCoeff, 40.0f, 95.5f};
  tub.run(90 * 60, true);
  tub.run(60, false);
  TEST_ASSERT_EQUAL_UINT32(1, model.heatRuns().size());
  TEST_ASSERT_FLOAT_WITHIN(0.4f, 6.0f, model.heatRuns().back().value);
  TEST_ASSERT_TRUE(model.dirty());
}

void test_learns_loss_from_a_long_coast() {
  ThermalModel model;
  Tub tub{model, 6.0f, 0.02f, 30.0f, 104.5f};
  tub.run(6 * 3600, false);
  tub.run(60, true);
  TEST_ASSERT_EQUAL_UINT32(1, model.coolRuns().size());
  TEST_ASSERT_FLOAT_WITHIN(0.003f, 0.02f, model.coolRuns().back().value);
}

void test_long_coasts_are_split() {
  ThermalModel model;
  Tub tub{model, 6.0f, 0.02f, 30.0f, 104.5f};
  tub.run(20 * 3600, false);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2, model.coolRuns().size());
}

void test_skips_runs_while_in_use() {
  ThermalModel model;
  Tub tub{model, 6.0f, 0.02f, 30.0f, 100.5f};
  tub.run(4 * 3600, false, true);
  tub.run(60, true);
  TEST_ASSERT_EQUAL_UINT32(0, model.coolRuns().size());
  // Heating right after a soak waits out the grace period too.
  tub.run(25 * 60, true);
  tub.run(60, false);
  TEST_ASSERT_EQUAL_UINT32(0, model.heatRuns().size());
}

void test_skips_cooling_without_forecast() {
  ThermalModel model;
  Tub tub{model, 6.0f, 0.02f, 30.0f, 104.5f};
  tub.run(6 * 3600, false, false, false);
  tub.run(60, true);
  TEST_ASSERT_EQUAL_UINT32(0, model.coolRuns().size());
}

void test_short_runs_are_ignored() {
  ThermalModel model;
  Tub tub{model, 6.0f, 0.02f, 30.0f, 99.5f};
  tub.run(10 * 60, true);
  tub.run(60, false);
  TEST_ASSERT_EQUAL_UINT32(0, model.heatRuns().size());
  TEST_ASSERT_FALSE(model.dirty());
}

void test_short_plan_heat_up_counts() {
  // A 15-minute top-up at 11°F/hr: about 2.75°F, ticks a bit over 10 minutes apart.
  ThermalModel model;
  Tub tub{model, 11.0f, ThermalModel::DefaultLossCoeff, 40.0f, 97.5f};
  tub.run(15 * 60, true);
  tub.run(60, false);
  TEST_ASSERT_EQUAL_UINT32(1, model.heatRuns().size());
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 11.0f, model.heatRuns().back().value);
}

void test_gap_restarts_runs() {
  ThermalModel model;
  Tub tub{model, 6.0f, ThermalModel::DefaultLossCoeff, 40.0f, 95.5f};
  tub.run(30 * 60, true);
  tub.now += 20 * 60;  // controller rebooted mid-run
  tub.run(15 * 60, true);
  tub.run(60, false);
  TEST_ASSERT_EQUAL_UINT32(0, model.heatRuns().size());
}

void test_median_once_enough_runs() {
  ThermalModel model;
  model.setSeeds(5.5f, 200);
  std::deque<ThermalObservation> heat;
  for (float value : {6.0f, 6.2f, 5.8f, 20.0f, 6.1f}) {
    ThermalObservation obs;
    obs.end = T0;
    obs.value = value;
    heat.push_back(obs);
  }
  model.load(heat, {});
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.1f, model.heaterRate());
  heat.pop_back();
  model.load(heat, {});
  // Under five runs the seed still counts as one of them.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, (11.25f + 6.0f + 6.2f + 5.8f + 20.0f) / 5.0f, model.heaterRate());
}

void test_hours_to_heat() {
  ThermalModel model;
  std::deque<ThermalObservation> heat;
  ThermalObservation obs;
  obs.end = T0;
  obs.value = 6.0f;
  for (int i = 0; i < 5; ++i) heat.push_back(obs);
  model.load(heat, {});
  TEST_ASSERT_EQUAL_FLOAT(0, model.hoursToHeat(104, 100, 40));
  const float hours = model.hoursToHeat(95, 104, 40);
  // Net rate is about 6 - 0.012 * 60 = 5.3°F/hr, so roughly 1.7 hours.
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 1.7f, hours);
  TEST_ASSERT_TRUE(isinf(model.hoursToHeat(95, 104, -500)));
}

void test_json_round_trip() {
  ThermalModel model;
  Tub tub{model, 6.0f, ThermalModel::DefaultLossCoeff, 40.0f, 95.5f};
  tub.run(90 * 60, true);
  tub.run(60, false);
  JsonDocument doc;
  model.toJson(doc.to<JsonObject>());
  ThermalModel loaded;
  loaded.fromJson(doc.as<JsonVariantConst>());
  TEST_ASSERT_EQUAL_UINT32(1, loaded.heatRuns().size());
  TEST_ASSERT_EQUAL_FLOAT(model.heatRuns().back().value, loaded.heatRuns().back().value);
  TEST_ASSERT_EQUAL_FLOAT(40.0f, loaded.heatRuns().back().outside);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_seed_from_heater_and_volume);
  RUN_TEST(test_learns_heater_rate_from_a_heating_run);
  RUN_TEST(test_learns_loss_from_a_long_coast);
  RUN_TEST(test_long_coasts_are_split);
  RUN_TEST(test_skips_runs_while_in_use);
  RUN_TEST(test_skips_cooling_without_forecast);
  RUN_TEST(test_short_runs_are_ignored);
  RUN_TEST(test_short_plan_heat_up_counts);
  RUN_TEST(test_gap_restarts_runs);
  RUN_TEST(test_median_once_enough_runs);
  RUN_TEST(test_hours_to_heat);
  RUN_TEST(test_json_round_trip);
  return UNITY_END();
}
