#include <unity.h>

#include <stdlib.h>
#include <time.h>

#include "scheduler/heat_planner.h"

constexpr uint32_t Monday8am = 1793023200;    // 2026-10-26 08:00 MDT
constexpr uint32_t Saturday8am = 1793455200;  // 2026-10-31 08:00 MDT
constexpr uint32_t Step = 900;
constexpr size_t Steps = 144;  // 36 hours
constexpr size_t PeakStart = 36;  // 17:00 is 9 hours after 08:00
constexpr size_t PeakEnd = 52;  // 21:00
constexpr size_t ReadyStep = 45;  // the step ending at 19:30

EnergyRates winterRates() {
  EnergyRates rates;
  rates.winterPeak = 0.18331f;
  rates.winterOffPeak = 0.06792f;
  rates.summerPeak = 0.21277f;
  rates.summerOffPeak = 0.07884f;
  return rates;
}

PlanInput soakAt730(uint32_t start, float water, float outside) {
  PlanInput in;
  in.start = start;
  in.stepSec = Step;
  in.waterTemp = water;
  in.heaterKw = 5.5f;
  in.heaterRate = 6.0f;
  in.lossCoeff = 0.012f;
  in.steps = buildPlanSteps(start, Step, Steps, TouSchedule(), winterRates(), 104,
                            [outside](uint32_t) { return outside; });
  for (size_t s = ReadyStep; s < ReadyStep + 8; ++s) {
    in.steps[s].minTemp = 104;  // two hours in the tub
  }
  return in;
}

size_t firstHeatStep(const PlanResult &plan) {
  for (size_t s = 0; s < plan.heat.size(); ++s) {
    if (plan.heat[s] > 0) return s;
  }
  return plan.heat.size();
}

float heaterHours(const PlanResult &plan, size_t from, size_t to) {
  float hours = 0;
  for (size_t s = from; s < to; ++s) hours += plan.heat[s] * Step / 3600.0f;
  return hours;
}

void setUp() {
  setenv("TZ", "MST7MDT,M3.2.0/2,M11.1.0/2", 1);
  tzset();
}

void tearDown() {}

void test_weekday_soak_heats_before_peak() {
  const PlanResult plan = HeatPlanner::plan(soakAt730(Monday8am, 100, 45));
  TEST_ASSERT_TRUE(plan.ok);
  TEST_ASSERT_FLOAT_WITHIN(0.3f, 0, plan.shortfall);
  // Coasts through the morning, then heats in the afternoon...
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(24, firstHeatStep(plan));  // not before 2 PM
  TEST_ASSERT_TRUE(plan.temp[PeakStart - 1] >= 103.5f);  // ...to be hot by 5 PM
  // ...and only tops up during the peak: losses at 104°F are under a degree an hour.
  const float peakHours = heaterHours(plan, PeakStart, PeakEnd);
  TEST_ASSERT_TRUE(peakHours < 0.6f);
  TEST_ASSERT_TRUE(heaterHours(plan, 0, PeakStart) > 1.0f);
}

void test_weekend_soak_heats_just_in_time() {
  // Saturday has no peak, so the cheapest plan heats as late as possible to lose the least heat.
  const PlanResult plan = HeatPlanner::plan(soakAt730(Saturday8am, 100, 45));
  TEST_ASSERT_FLOAT_WITHIN(0.3f, 0, plan.shortfall);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(PeakStart, firstHeatStep(plan));  // after 5 PM
  TEST_ASSERT_TRUE(plan.temp[ReadyStep - 2] < 104.0f);  // still heating just before 7:30
  TEST_ASSERT_TRUE(heaterHours(plan, PeakStart, PeakEnd) > 1.0f);
}

void test_weekend_costs_less_than_weekday() {
  const PlanResult weekday = HeatPlanner::plan(soakAt730(Monday8am, 100, 45));
  const PlanResult weekend = HeatPlanner::plan(soakAt730(Saturday8am, 100, 45));
  TEST_ASSERT_TRUE(weekend.cost < weekday.cost);
  TEST_ASSERT_TRUE(weekend.kwh < weekday.kwh);  // and less heat lost, from heating later
}

void test_colder_weather_starts_sooner() {
  const PlanResult mild = HeatPlanner::plan(soakAt730(Saturday8am, 100, 60));
  const PlanResult cold = HeatPlanner::plan(soakAt730(Saturday8am, 100, 10));
  TEST_ASSERT_TRUE(firstHeatStep(cold) < firstHeatStep(mild));
  TEST_ASSERT_TRUE(cold.kwh > mild.kwh);
}

void test_impossible_target_heats_straight_away() {
  PlanInput in = soakAt730(Monday8am, 95, 45);
  for (PlanStep &step : in.steps) step.minTemp = NAN;
  in.steps[1].minTemp = 104;  // in half an hour
  const PlanResult plan = HeatPlanner::plan(in);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, plan.heat[0]);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, plan.heat[1]);
  TEST_ASSERT_TRUE(plan.shortfall > 5.0f);
}

void test_no_requirements_never_heats() {
  PlanInput in = soakAt730(Monday8am, 100, 45);
  for (PlanStep &step : in.steps) step.minTemp = NAN;
  const PlanResult plan = HeatPlanner::plan(in);
  TEST_ASSERT_EQUAL_FLOAT(0, plan.kwh);
  TEST_ASSERT_TRUE(plan.temp.back() < 100.0f);
}

void test_never_exceeds_max() {
  PlanInput in = soakAt730(Monday8am, 100, 45);
  for (PlanStep &step : in.steps) step.maxTemp = 102;  // below the 104 requirement
  const PlanResult plan = HeatPlanner::plan(in);
  for (float temp : plan.temp) TEST_ASSERT_TRUE(temp <= 102.001f);
  TEST_ASSERT_FLOAT_WITHIN(0.3f, 0, plan.shortfall);  // a requirement above the max is capped to it
}

void test_holds_a_standby_floor_cheaply() {
  PlanInput in = soakAt730(Monday8am, 96, 30);
  for (PlanStep &step : in.steps) step.minTemp = 95;
  const PlanResult plan = HeatPlanner::plan(in);
  TEST_ASSERT_FLOAT_WITHIN(0.3f, 0, plan.shortfall);
  // Holding 95 means short bursts whenever it's needed (a 15-minute step adds about 1.3°F)...
  for (size_t s = 0; s < 28; ++s) TEST_ASSERT_TRUE(plan.temp[s] < 97.0f);
  // ...except banking a few degrees before 5 PM so it can coast through the peak.
  TEST_ASSERT_TRUE(plan.temp[PeakStart - 1] > 98.0f);
  TEST_ASSERT_TRUE(heaterHours(plan, PeakStart, PeakEnd) < 0.1f);
}

void test_missing_water_temp_is_not_planned() {
  PlanInput in = soakAt730(Monday8am, NAN, 45);
  TEST_ASSERT_FALSE(HeatPlanner::plan(in).ok);
}

void test_soak_helpers() {
  PlanInput in = soakAt730(Monday8am, 100, 45);
  for (PlanStep &step : in.steps) step.minTemp = NAN;
  addSoak(in, 104, Monday8am + 11 * 3600 + 1800, 2);
  TEST_ASSERT_TRUE(isnan(in.steps[ReadyStep - 1].minTemp));
  TEST_ASSERT_EQUAL_FLOAT(104, in.steps[ReadyStep].minTemp);
  TEST_ASSERT_EQUAL_FLOAT(104, in.steps[ReadyStep + 7].minTemp);
  TEST_ASSERT_TRUE(isnan(in.steps[ReadyStep + 8].minTemp));

  // 4°F at ~5.4°F/hr net is three quarter-hour steps.
  TEST_ASSERT_EQUAL_UINT32(Monday8am + 3 * Step, earliestReady(in, 104));
  TEST_ASSERT_EQUAL_UINT32(Monday8am, earliestReady(in, 99));
  TEST_ASSERT_EQUAL_UINT32(0, earliestReady(in, 200));
}

void test_tou_price() {
  const EnergyRates rates = winterRates();
  const TouSchedule tou;
  TEST_ASSERT_EQUAL_FLOAT(0.18331f, touPrice(tou, rates, {2026, 10, 26}, 18));
  TEST_ASSERT_EQUAL_FLOAT(0.06792f, touPrice(tou, rates, {2026, 10, 26}, 12));
  TEST_ASSERT_EQUAL_FLOAT(0.06792f, touPrice(tou, rates, {2026, 10, 31}, 18));  // Saturday
  TEST_ASSERT_EQUAL_FLOAT(0.21277f, touPrice(tou, rates, {2026, 7, 15}, 18));
  // No rates entered: peak still costs more.
  TEST_ASSERT_EQUAL_FLOAT(2.0f, touPrice(tou, EnergyRates(), {2026, 10, 26}, 18));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, touPrice(tou, EnergyRates(), {2026, 10, 26}, 12));
}

void test_heats_in_one_run_up_to_ready() {
  // A real overnight plan: 102°F at 6 PM Monday, 104 ready at 7:30 AM Tuesday for an hour, nearly 4 steps
  // of heat. Where to put the heat is a near tie the grid can't resolve; it should go in one run that ends
  // at the ready time, not pause partway.
  constexpr uint32_t Monday6pm = Monday8am + 10 * 3600;
  constexpr uint32_t ReadyAt = Monday6pm + 54 * Step;  // Tuesday 7:30 AM
  static const float outside[] = {47, 46, 45, 45, 44, 44, 44, 43, 43, 44, 44, 44, 45, 44, 44, 44, 44, 44, 44,
                                  44, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 42, 42, 42,
                                  42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 41, 41, 41, 41};
  PlanInput in;
  in.start = Monday6pm;
  in.waterTemp = 102;
  in.heaterKw = 5.5f;
  in.heaterRate = 11.25106f;
  in.lossCoeff = 0.012f;
  in.steps = buildPlanSteps(Monday6pm, Step, Steps, TouSchedule(), winterRates(), 104, [](uint32_t at) {
    const size_t s = (at - Monday6pm) / Step;
    return s < sizeof(outside) / sizeof(outside[0]) ? outside[s] : 41.0f;
  });
  addSoak(in, 104, ReadyAt, 1);
  const PlanResult plan = HeatPlanner::plan(in);
  TEST_ASSERT_FLOAT_WITHIN(0.3f, 0, plan.shortfall);
  constexpr size_t Ready = 54;
  const size_t first = firstHeatStep(plan);
  TEST_ASSERT_TRUE(first < Ready);
  for (size_t s = first; s < Ready; ++s) TEST_ASSERT_TRUE(plan.heat[s] > 0);  // no pause before ready
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_weekday_soak_heats_before_peak);
  RUN_TEST(test_weekend_soak_heats_just_in_time);
  RUN_TEST(test_weekend_costs_less_than_weekday);
  RUN_TEST(test_colder_weather_starts_sooner);
  RUN_TEST(test_impossible_target_heats_straight_away);
  RUN_TEST(test_no_requirements_never_heats);
  RUN_TEST(test_never_exceeds_max);
  RUN_TEST(test_holds_a_standby_floor_cheaply);
  RUN_TEST(test_missing_water_temp_is_not_planned);
  RUN_TEST(test_soak_helpers);
  RUN_TEST(test_tou_price);
  RUN_TEST(test_heats_in_one_run_up_to_ready);
  return UNITY_END();
}
