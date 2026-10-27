#include "scheduler/heat_planner.h"

#include <time.h>

#include <algorithm>

#include "scheduler/thermal_model.h"

namespace {

struct Transition {
  float next;  // water temp at the end of the step
  float fraction;  // of the step the heater runs
};

// The per-step constants of the physics, worked out once per plan: the search simulates a step hundreds of
// thousands of times, and expf() dominated it on the ESP32.
struct Physics {
  bool lossy;  // lossCoeff > 0
  float decay;  // expf(-lossCoeff * hours): how much of the gap to the settling level remains after a step
  float heaterLift;  // heaterRate / lossCoeff: how far above the air heating levels off
  float heaterStep;  // heaterRate * hours, for a lossless tub

  Physics(const PlanInput &in, float stepHours)
      : lossy(in.lossCoeff > 0),
        decay(lossy ? expf(-in.lossCoeff * stepHours) : 1.0f),
        heaterLift(lossy ? in.heaterRate / in.lossCoeff : 0.0f),
        heaterStep(in.heaterRate * stepHours) {}
};

// Exponential approach to `level`: the exact solution of dT/dt = loss * (level - T) over one step.
float settle(float temp, float level, float decay) {
  return level + (temp - level) * decay;
}

Transition simulate(const Physics &physics, const PlanStep &step, float temp, bool heat) {
  const float air = isnan(step.outside) ? ThermalModel::FallbackOutsideF : step.outside;
  const float coast = physics.lossy ? settle(temp, air, physics.decay) : temp;
  if (!heat || coast >= step.maxTemp) {
    return {coast, 0};
  }
  const float full = physics.lossy ? settle(temp, air + physics.heaterLift, physics.decay) : temp + physics.heaterStep;
  if (full <= step.maxTemp) {
    return {full, 1};
  }
  // The spa stops at its set temp partway through the step.
  return {step.maxTemp, (step.maxTemp - coast) / (full - coast)};
}

float shortfall(const PlanStep &step, float temp) {
  return isnan(step.minTemp) ? 0 : std::max(0.0f, std::min(step.minTemp, step.maxTemp) - temp);
}

struct Followed {
  PlanResult result;
  float total = 0;  // cost plus shortfall penalties, to compare plans by
};

// Runs the heater as `heat` says from the actual water temp.
Followed follow(const PlanInput &in, const Physics &physics, float kwhPerStep, const std::vector<bool> &heat) {
  Followed out;
  PlanResult &result = out.result;
  result.heat.reserve(heat.size());
  result.temp.reserve(heat.size());
  float temp = in.waterTemp;
  for (size_t s = 0; s < heat.size(); ++s) {
    const PlanStep &step = in.steps[s];
    const Transition move = simulate(physics, step, temp, heat[s]);
    temp = move.next;
    const float missed = shortfall(step, temp);
    result.heat.push_back(move.fraction);
    result.temp.push_back(temp);
    result.kwh += move.fraction * kwhPerStep;
    result.cost += move.fraction * kwhPerStep * step.price;
    result.shortfall = std::max(result.shortfall, missed);
    out.total += missed * HeatPlanner::ShortfallCostPerF;
  }
  out.total += result.cost;
  result.ok = true;
  return out;
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
  const Physics physics(in, hours);

  // The water can't get colder than it would coasting the whole way, so that bounds the grid from below.
  float lo = in.waterTemp;
  float hi = in.waterTemp;
  float coasting = in.waterTemp;
  for (const PlanStep &step : in.steps) {
    coasting = simulate(physics, step, coasting, false).next;
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
      const Transition off = simulate(physics, step, temp, false);
      const Transition on = simulate(physics, step, temp, true);
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
  std::vector<bool> heat(steps);
  float temp = in.waterTemp;
  for (size_t s = 0; s < steps; ++s) {
    const float pos = std::min(std::max((temp - lo) / GridStepF, 0.0f), static_cast<float>(points - 1));
    heat[s] = heatHere[s * points + static_cast<size_t>(pos)];
    temp = simulate(physics, in.steps[s], temp, heat[s]).next;
  }
  std::vector<bool>().swap(heatHere);
  Followed best = follow(in, physics, kwhPerStep, heat);

  // Taking the colder point's choice can start heating a step early, which then shows up as a pause
  // before the deadline. Slide each short pause to the front of its heating run whenever that costs no
  // more and still meets every requirement: heating later loses less heat, and the heater runs once,
  // finishing just in time. Each move only shifts heat later, so this ends.
  for (bool moved = true; moved;) {
    moved = false;
    const std::vector<float> &on = best.result.heat;
    for (size_t first = 1; first + 1 < steps; ++first) {
      if (on[first] > 0 || on[first - 1] == 0 || heat[first]) {
        continue;
      }
      size_t last = first;  // the pause is first..last
      while (last + 1 < steps && on[last + 1] == 0 && !heat[last + 1] && last - first + 1 < MaxPauseSteps) {
        ++last;
      }
      if (last + 1 >= steps || on[last + 1] == 0) {
        continue;
      }
      size_t runStart = first - 1;
      while (runStart > 0 && on[runStart - 1] > 0) {
        --runStart;
      }
      std::vector<bool> trial = heat;
      for (size_t s = runStart; s <= last; ++s) {
        trial[s] = s >= runStart + (last - first + 1);
      }
      Followed shifted = follow(in, physics, kwhPerStep, trial);
      if (shifted.total <= best.total + 1e-4f) {
        heat.swap(trial);
        best = std::move(shifted);
        moved = true;
        break;
      }
    }
  }
  return best.result;
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
  const Physics physics(in, static_cast<float>(in.stepSec) / 3600.0f);
  float temp = in.waterTemp;
  for (size_t s = 0; s < in.steps.size(); ++s) {
    if (temp >= target - HeatPlanner::GridStepF) {
      return in.start + s * in.stepSec;
    }
    temp = simulate(physics, in.steps[s], temp, true).next;
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
