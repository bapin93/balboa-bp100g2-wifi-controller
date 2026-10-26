#pragma once

#include <math.h>
#include <stdint.h>

#include <functional>
#include <vector>

#include "scheduler/tou.h"
#include "storage/config_model.h"

// Finds the cheapest way to run the heater over the next day and a half while keeping the water within
// each step's bounds. Water follows the same physics as ThermalModel; cost is heater kWh at each step's
// price, so heating drifts out of the TOU peak and as late as the requirements allow (less time hot means
// less heat lost).
//
// The search is dynamic programming over water temperature in GridStepF steps. Only one row of costs is
// kept at a time plus one bit per (step, temperature) saying whether to heat, so a 36-hour plan fits in a
// few KB on the ESP32.
struct PlanStep {
  float price = 0;  // $/kWh for heat used during this step
  float outside = NAN;  // °F; NAN falls back to ThermalModel::FallbackOutsideF
  float minTemp = NAN;  // water must be at least this by the end of the step; NAN = no requirement
  float maxTemp = 104;  // the heater stops here, as the spa does at its set temp
};

struct PlanInput {
  uint32_t start = 0;  // Unix time the first step begins
  uint32_t stepSec = 900;
  float waterTemp = NAN;
  float heaterKw = 5.5f;
  float heaterRate = 6.0f;  // °F/hr the heater adds before losses
  float lossCoeff = 0.012f;  // per hour
  std::vector<PlanStep> steps;
};

struct PlanResult {
  bool ok = false;  // false when the input couldn't be planned (no water temp, no steps)
  std::vector<float> heat;  // fraction of each step the heater runs, 0-1
  std::vector<float> temp;  // predicted water temp at the end of each step
  float kwh = 0;
  float cost = 0;
  float shortfall = 0;  // worst °F any requirement is missed by; 0 when all are met
};

class HeatPlanner {
 public:
  static constexpr float GridStepF = 0.25f;
  static constexpr float ShortfallCostPerF = 100.0f;  // $ per °F short: requirements beat any price

  static PlanResult plan(const PlanInput &input);
};

// Hours of full heating from `from` to `to`; 0 if already there, INFINITY if losses outrun the heater.
float hoursToReach(float from, float to, float heaterRate, float lossCoeff, float outside);
// The coldest the water can be and still reach `comfort` within `hours` of heating.
float readinessTemp(float comfort, float hours, float heaterRate, float lossCoeff, float outside);

// End of the first step by which full heating reaches `target`, or 0 if it can't within the steps.
uint32_t earliestReady(const PlanInput &input, float target);

// Requires `target` from readyAt (a step boundary at or after it) for `hours`. Steps already holding a
// higher requirement keep it.
void addSoak(PlanInput &input, float target, uint32_t readyAt, float hours);

// $/kWh at a local date and hour. Without rates entered, peak counts double so the plan still avoids it.
float touPrice(const TouSchedule &tou, const EnergyRates &rates, const CivilDate &date, int hour);

// Steps of `stepSec` from `start`, priced by local time (TZ must be set) with the forecast filled in.
// Requirements are left empty for the caller.
std::vector<PlanStep> buildPlanSteps(uint32_t start, uint32_t stepSec, size_t count, const TouSchedule &tou,
                                     const EnergyRates &rates, float maxTemp,
                                     const std::function<float(uint32_t)> &outsideAt);
