#include "scheduler/heat_planner.h"

#include <time.h>

#include <algorithm>

#include "scheduler/thermal_model.h"

namespace {

struct Transition {
  float next;  // water temp at the end of the step
  float fraction;  // of the step the heater runs
};

// Exponential approach to `level`: the exact solution of dT/dt = loss * (level - T).
float settle(float temp, float level, float loss, float hours) {
  return level + (temp - level) * expf(-loss * hours);
}

Transition simulate(const PlanInput &in, const PlanStep &step, float temp, bool heat, float hours) {
  const float air = isnan(step.outside) ? ThermalModel::FallbackOutsideF : step.outside;
  const float coast = in.lossCoeff > 0 ? settle(temp, air, in.lossCoeff, hours) : temp;
  if (!heat || coast >= step.maxTemp) {
    return {coast, 0};
  }
  const float full = in.lossCoeff > 0 ? settle(temp, air + in.heaterRate / in.lossCoeff, in.lossCoeff, hours)
                                      : temp + in.heaterRate * hours;
  if (full <= step.maxTemp) {
    return {full, 1};
  }
  // The spa stops at its set temp partway through the step.
  return {step.maxTemp, (step.maxTemp - coast) / (full - coast)};
}

float shortfall(const PlanStep &step, float temp) {
  return isnan(step.minTemp) ? 0 : std::max(0.0f, std::min(step.minTemp, step.maxTemp) - temp);
}

}  // namespace

PlanResult HeatPlanner::plan(const PlanInput &in) {
  PlanResult result;
  const size_t steps = in.steps.size();
  if (steps == 0 || isnan(in.waterTemp) || in.heaterRate <= 0 || in.stepSec == 0) {
    return result;
  }
  const float hours = static_cast<float>(in.stepSec) / 3600.0f;
  const float kwhPerStep = in.heaterKw * hours;

  // The water can't get colder than it would coasting the whole way, so that bounds the grid from below.
  float lo = in.waterTemp;
  float hi = in.waterTemp;
  float coasting = in.waterTemp;
  for (const PlanStep &step : in.steps) {
    coasting = simulate(in, step, coasting, false, hours).next;
    lo = std::min(lo, coasting);
    hi = std::max(hi, step.maxTemp);
  }
  lo -= GridStepF;
  hi += GridStepF;
  const size_t points = static_cast<size_t>(ceilf((hi - lo) / GridStepF)) + 1;
  const auto costAt = [&](const std::vector<float> &row, float temp) {
    const float pos = std::min(std::max((temp - lo) / GridStepF, 0.0f), static_cast<float>(points - 1));
    const size_t i = std::min(static_cast<size_t>(pos), points - 2);
    const float t = pos - static_cast<float>(i);
    return row[i] * (1 - t) + row[i + 1] * t;
  };

  // Backward: cost-to-go from each temperature, remembering whether heating was the cheaper choice.
  std::vector<bool> heatHere(steps * points);
  std::vector<float> later(points, 0.0f);
  std::vector<float> now(points);
  for (size_t s = steps; s-- > 0;) {
    const PlanStep &step = in.steps[s];
    for (size_t i = 0; i < points; ++i) {
      const float temp = lo + static_cast<float>(i) * GridStepF;
      const Transition off = simulate(in, step, temp, false, hours);
      const Transition on = simulate(in, step, temp, true, hours);
      const float offCost = shortfall(step, off.next) * ShortfallCostPerF + costAt(later, off.next);
      const float onCost = on.fraction * kwhPerStep * step.price + shortfall(step, on.next) * ShortfallCostPerF +
                           costAt(later, on.next);
      const bool heat = onCost < offCost - 1e-6f;
      heatHere[s * points + i] = heat;
      now[i] = heat ? onCost : offCost;
    }
    std::swap(later, now);
  }

  // Forward: follow the choices from the actual water temp. Between grid points, take the colder one's
  // choice; it heats whenever the warmer one would, so requirements are still met.
  result.heat.reserve(steps);
  result.temp.reserve(steps);
  float temp = in.waterTemp;
  for (size_t s = 0; s < steps; ++s) {
    const PlanStep &step = in.steps[s];
    const float pos = std::min(std::max((temp - lo) / GridStepF, 0.0f), static_cast<float>(points - 1));
    const Transition move = simulate(in, step, temp, heatHere[s * points + static_cast<size_t>(pos)], hours);
    temp = move.next;
    result.heat.push_back(move.fraction);
    result.temp.push_back(temp);
    result.kwh += move.fraction * kwhPerStep;
    result.cost += move.fraction * kwhPerStep * step.price;
    result.shortfall = std::max(result.shortfall, shortfall(step, temp));
  }
  result.ok = true;
  return result;
}

float hoursToReach(float from, float to, float heaterRate, float lossCoeff, float outside) {
  if (!(to > from)) {
    return 0;
  }
  const float air = isnan(outside) ? ThermalModel::FallbackOutsideF : outside;
  if (lossCoeff <= 0) {
    return (to - from) / heaterRate;
  }
  const float level = air + heaterRate / lossCoeff;  // where heating levels off
  if (to >= level) {
    return INFINITY;
  }
  return logf((level - from) / (level - to)) / lossCoeff;
}

float readinessTemp(float comfort, float hours, float heaterRate, float lossCoeff, float outside) {
  const float air = isnan(outside) ? ThermalModel::FallbackOutsideF : outside;
  if (lossCoeff <= 0) {
    return comfort - heaterRate * hours;
  }
  const float level = air + heaterRate / lossCoeff;
  if (level <= comfort) {
    return comfort;  // can't get there at all; keep it as close as possible
  }
  return level - (level - comfort) * expf(lossCoeff * hours);
}

uint32_t earliestReady(const PlanInput &in, float target) {
  if (isnan(in.waterTemp) || in.stepSec == 0) {
    return 0;
  }
  const float hours = static_cast<float>(in.stepSec) / 3600.0f;
  float temp = in.waterTemp;
  for (size_t s = 0; s < in.steps.size(); ++s) {
    if (temp >= target - HeatPlanner::GridStepF) {
      return in.start + s * in.stepSec;
    }
    temp = simulate(in, in.steps[s], temp, true, hours).next;
  }
  return temp >= target - HeatPlanner::GridStepF ? in.start + in.steps.size() * in.stepSec : 0;
}

void addSoak(PlanInput &in, float target, uint32_t readyAt, float hours) {
  if (in.stepSec == 0) {
    return;
  }
  const uint32_t end = readyAt + static_cast<uint32_t>(hours * 3600.0f);
  for (size_t s = 0; s < in.steps.size(); ++s) {
    const uint32_t stepEnd = in.start + (s + 1) * in.stepSec;
    if (stepEnd >= readyAt && stepEnd < end && !(in.steps[s].minTemp >= target)) {
      in.steps[s].minTemp = target;
    }
  }
}

float touPrice(const TouSchedule &tou, const EnergyRates &rates, const CivilDate &date, int hour) {
  const bool peak = isPeakTime(tou, date, hour);
  if (rates.summerPeak <= 0 && rates.summerOffPeak <= 0 && rates.winterPeak <= 0 && rates.winterOffPeak <= 0) {
    return peak ? 2.0f : 1.0f;
  }
  const int from = rates.summerStartMonth;
  const int to = rates.summerEndMonth;
  const bool summer = from <= to ? date.month >= from && date.month <= to : date.month >= from || date.month <= to;
  if (summer) {
    return peak ? rates.summerPeak : rates.summerOffPeak;
  }
  return peak ? rates.winterPeak : rates.winterOffPeak;
}

std::vector<PlanStep> buildPlanSteps(uint32_t start, uint32_t stepSec, size_t count, const TouSchedule &tou,
                                     const EnergyRates &rates, float maxTemp,
                                     const std::function<float(uint32_t)> &outsideAt) {
  std::vector<PlanStep> steps(count);
  for (size_t s = 0; s < count; ++s) {
    const time_t at = static_cast<time_t>(start + s * stepSec);
    tm local;
    localtime_r(&at, &local);
    PlanStep &step = steps[s];
    step.price = touPrice(tou, rates, {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday}, local.tm_hour);
    step.outside = outsideAt(static_cast<uint32_t>(at) + stepSec / 2);
    step.maxTemp = maxTemp;
  }
  return steps;
}
