#include "scheduler/thermal_model.h"

#include <algorithm>
#include <vector>

namespace {

constexpr uint32_t ClockValidAfter = 1700000000;
constexpr float BtuPerKwh = 3412.14f;
constexpr float PoundsPerGallon = 8.34f;
constexpr size_t MinRunsForMedian = 5;

// The seed until a few runs are in, then the median of what was measured so one odd run can't skew it.
float estimate(const std::deque<ThermalObservation> &runs, float seed) {
  if (runs.empty()) {
    return seed;
  }
  if (runs.size() < MinRunsForMedian) {
    float sum = seed;
    for (const ThermalObservation &run : runs) {
      sum += run.value;
    }
    return sum / static_cast<float>(runs.size() + 1);
  }
  std::vector<float> values;
  values.reserve(runs.size());
  for (const ThermalObservation &run : runs) {
    values.push_back(run.value);
  }
  std::sort(values.begin(), values.end());
  const size_t mid = values.size() / 2;
  return values.size() % 2 ? values[mid] : (values[mid - 1] + values[mid]) / 2.0f;
}

void observationsToJson(const std::deque<ThermalObservation> &runs, JsonArray out) {
  for (const ThermalObservation &run : runs) {
    JsonObject item = out.add<JsonObject>();
    item["t"] = run.end;
    item["v"] = run.value;
    item["a"] = run.startTemp;
    item["b"] = run.endTemp;
    if (!isnan(run.outside)) {
      item["o"] = run.outside;
    }
    item["m"] = run.minutes;
  }
}

std::deque<ThermalObservation> observationsFromJson(JsonArrayConst in) {
  std::deque<ThermalObservation> runs;
  for (JsonObjectConst item : in) {
    ThermalObservation run;
    run.end = item["t"] | 0;
    run.value = item["v"] | NAN;
    run.startTemp = item["a"] | NAN;
    run.endTemp = item["b"] | NAN;
    run.outside = item["o"] | NAN;
    run.minutes = item["m"] | 0;
    if (run.end && !isnan(run.value)) {
      runs.push_back(run);
    }
  }
  return runs;
}

}  // namespace

void ThermalModel::setSeeds(float heaterKw, uint16_t gallons) {
  if (heaterKw > 0 && gallons > 0) {
    heaterRateSeed_ = heaterKw * BtuPerKwh / (static_cast<float>(gallons) * PoundsPerGallon);
  }
}

void ThermalModel::observe(const ThermalSample &sample) {
  const uint32_t now = sample.now;
  if (now < ClockValidAfter) {
    return;
  }
  if (lastSampleAt_ && (now < lastSampleAt_ || now - lastSampleAt_ > MaxSampleGapSec)) {
    heatRun_.active = false;  // can't tell what happened in the gap
    coolRun_.active = false;
    lastTemp_ = NAN;
  }
  lastSampleAt_ = now;
  if (sample.inUse) {
    lastUseAt_ = now;
  }
  const bool quiet = lastUseAt_ == 0 || now - lastUseAt_ >= UseGraceSec;
  const float water = sample.waterTemp;
  const bool changed = !isnan(water) && !isnan(lastTemp_) && water != lastTemp_;

  const bool heating = sample.heating && quiet;
  if (heatRun_.active && !heating) {
    finishHeatRun();
  }
  if (!heatRun_.active && heating) {
    startRun(heatRun_, now);
  }
  if (heatRun_.active) {
    if (!isnan(sample.outside)) {
      heatRun_.outsideSum += sample.outside;
      ++heatRun_.outsideCount;
    }
    if (changed && water > lastTemp_) {
      addEdge(heatRun_, now, water);
    }
  }

  const bool coasting = !sample.heating && quiet;
  if (coolRun_.active && !coasting) {
    finishCoolRun(false);
  }
  if (!coolRun_.active && coasting) {
    startRun(coolRun_, now);
  }
  if (coolRun_.active) {
    if (!isnan(sample.outside)) {
      coolRun_.outsideSum += sample.outside;
      ++coolRun_.outsideCount;
    }
    if (changed && water < lastTemp_) {
      addEdge(coolRun_, now, water);
      if (coolRun_.last.at - coolRun_.first.at >= MaxCoolRunSec) {
        finishCoolRun(true);
      }
    }
  }

  if (!isnan(water)) {
    lastTemp_ = water;
  }
}

float ThermalModel::heaterRate() const {
  return estimate(heat_, heaterRateSeed_);
}

float ThermalModel::lossCoeff() const {
  return estimate(cool_, DefaultLossCoeff);
}

float ThermalModel::heatingRate(float water, float outside) const {
  return heaterRate() - coolingRate(water, outside);
}

float ThermalModel::coolingRate(float water, float outside) const {
  return lossCoeff() * (water - (isnan(outside) ? FallbackOutsideF : outside));
}

float ThermalModel::hoursToHeat(float from, float to, float outside) const {
  if (!(to > from)) {
    return 0;
  }
  const float heater = heaterRate();
  const float loss = lossCoeff();
  const float air = isnan(outside) ? FallbackOutsideF : outside;
  if (loss <= 0) {
    return (to - from) / heater;
  }
  // Heating levels off where losses match the heater; solve the exponential approach to that level.
  const float level = air + heater / loss;
  if (to >= level) {
    return INFINITY;
  }
  return logf((level - from) / (level - to)) / loss;
}

void ThermalModel::load(const std::deque<ThermalObservation> &heat, const std::deque<ThermalObservation> &cool) {
  heat_ = heat;
  cool_ = cool;
  while (heat_.size() > MaxObservations) {
    heat_.pop_front();
  }
  while (cool_.size() > MaxObservations) {
    cool_.pop_front();
  }
}

void ThermalModel::toJson(JsonObject out) const {
  observationsToJson(heat_, out["heat"].to<JsonArray>());
  observationsToJson(cool_, out["cool"].to<JsonArray>());
}

void ThermalModel::fromJson(JsonVariantConst in) {
  load(observationsFromJson(in["heat"]), observationsFromJson(in["cool"]));
}

void ThermalModel::startRun(Run &run, uint32_t now) {
  run = Run();
  run.active = true;
  run.startedAt = now;
}

void ThermalModel::addEdge(Run &run, uint32_t now, float temp) {
  if (!run.first.at) {
    run.first = {now, temp};
  }
  run.last = {now, temp};
}

float ThermalModel::runOutside(const Run &run) const {
  return run.outsideCount ? static_cast<float>(run.outsideSum / run.outsideCount) : NAN;
}

void ThermalModel::finishHeatRun() {
  heatRun_.active = false;
  const Run &run = heatRun_;
  const uint32_t span = run.last.at - run.first.at;
  const float rise = run.last.temp - run.first.temp;
  if (!run.first.at || span < MinHeatRunSec || !(rise >= MinHeatRiseF)) {
    return;
  }
  const float hours = static_cast<float>(span) / 3600.0f;
  const float outside = runOutside(run);
  const float middle = (run.first.temp + run.last.temp) / 2.0f;
  // What was measured is net of losses; add them back to get the heater's own rate.
  const float heater = rise / hours + coolingRate(middle, outside);
  if (heater < 1.0f || heater > 30.0f) {
    return;  // not a believable heater; probably a sensor glitch
  }
  ThermalObservation obs;
  obs.end = run.last.at;
  obs.value = heater;
  obs.startTemp = run.first.temp;
  obs.endTemp = run.last.temp;
  obs.outside = outside;
  obs.minutes = static_cast<uint16_t>(span / 60);
  addObservation(heat_, obs);
}

void ThermalModel::finishCoolRun(bool restart) {
  const Run run = coolRun_;
  coolRun_.active = false;
  if (restart) {
    // Carry on from the last edge so the next stretch starts on a precise reading.
    startRun(coolRun_, run.last.at);
    coolRun_.first = run.last;
    coolRun_.last = run.last;
  }
  const uint32_t span = run.last.at - run.first.at;
  const float outside = runOutside(run);
  if (!run.first.at || span < MinCoolRunSec || isnan(outside) || !(run.first.temp - run.last.temp >= 1.0f)) {
    return;
  }
  // Too close to the outside temperature and the log below is mostly noise.
  if (run.last.temp - outside < 5.0f) {
    return;
  }
  const float hours = static_cast<float>(span) / 3600.0f;
  const float loss = logf((run.first.temp - outside) / (run.last.temp - outside)) / hours;
  if (loss < 0.001f || loss > 0.1f) {
    return;
  }
  ThermalObservation obs;
  obs.end = run.last.at;
  obs.value = loss;
  obs.startTemp = run.first.temp;
  obs.endTemp = run.last.temp;
  obs.outside = outside;
  obs.minutes = static_cast<uint16_t>(span / 60);
  addObservation(cool_, obs);
}

void ThermalModel::addObservation(std::deque<ThermalObservation> &list, const ThermalObservation &obs) {
  list.push_back(obs);
  while (list.size() > MaxObservations) {
    list.pop_front();
  }
  dirty_ = true;
}
