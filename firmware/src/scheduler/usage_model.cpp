#include "scheduler/usage_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

// Starting likelihood for an hour of a workday or a weekend day.
float prior(bool weekend, int hour) {
  if (hour < 7) {
    return 0.02f;  // rarely after midnight
  }
  if (hour < 10) {
    return 0.03f;  // rarely in the morning
  }
  if (hour < 17) {
    return weekend ? 0.15f : 0.05f;  // weekend daytimes sometimes, workdays rarely
  }
  if (hour < 18) {
    return 0.15f;
  }
  if (hour < 23) {
    return 0.4f;  // evenings
  }
  return 0.1f;
}

}  // namespace

UsageModel::UsageModel() {
  for (int weekend = 0; weekend < 2; ++weekend) {
    for (int slot = 0; slot < Slots; ++slot) {
      grid_[weekend][slot] = prior(weekend, slot / 2);
    }
  }
}

void UsageModel::observe(uint32_t now, uint32_t date, bool weekend, int slot, bool inUse, bool raised) {
  if (date != day_) {
    if (day_ != 0) {
      fold();
    }
    day_ = date;
    dayWeekend_ = weekend;
    started_ = 0;
    dirty_ = true;
  }
  const bool startedUse = inUse && !wasInUse_;
  wasInUse_ = inUse;
  if ((startedUse || raised) && slot >= 0 && slot < Slots) {
    if (lastUse_ == 0 || now - lastUse_ >= QuietGapSec) {
      started_ |= 1ULL << slot;
      dirty_ = true;
    }
  }
  if (inUse || raised) {
    lastUse_ = now;
  }
}

float UsageModel::likelihood(bool weekend, int slot) const {
  return grid_[weekend ? 1 : 0][((slot % Slots) + Slots) % Slots];
}

float UsageModel::readyWithinHours(bool weekend, int slot) const {
  const float p = likelihood(weekend, slot);
  return p >= LikelyAbove ? 0.5f : p >= PossibleAbove ? 2.0f : 4.0f;
}

void UsageModel::fold() {
  const int kind = dayWeekend_ ? 1 : 0;
  const float rate = dayWeekend_ ? WeekendRate : WorkdayRate;
  for (int slot = 0; slot < Slots; ++slot) {
    const bool here = started_ & (1ULL << slot);
    const bool before = slot > 0 && (started_ & (1ULL << (slot - 1)));
    const bool after = slot + 1 < Slots && (started_ & (1ULL << (slot + 1)));
    const float seen = here ? 1.0f : (before || after) ? 0.5f : 0.0f;
    grid_[kind][slot] += rate * (seen - grid_[kind][slot]);
  }
}

void UsageModel::toJson(JsonObject out) const {
  const char *names[2] = {"workday", "weekend"};
  for (int kind = 0; kind < 2; ++kind) {
    JsonArray row = out[names[kind]].to<JsonArray>();
    for (int slot = 0; slot < Slots; ++slot) {
      row.add(roundf(grid_[kind][slot] * 1000) / 1000);
    }
  }
  out["day"] = day_;
  out["dayWeekend"] = dayWeekend_;
  // As a string: JSON numbers lose precision past 2^53.
  char started[24];
  snprintf(started, sizeof(started), "%llx", static_cast<unsigned long long>(started_));
  out["started"] = started;
  out["lastUse"] = lastUse_;
}

void UsageModel::fromJson(JsonVariantConst in) {
  const char *names[2] = {"workday", "weekend"};
  for (int kind = 0; kind < 2; ++kind) {
    JsonArrayConst row = in[names[kind]];
    if (row.size() != Slots) {
      continue;  // keep the priors
    }
    for (int slot = 0; slot < Slots; ++slot) {
      const float value = row[slot] | grid_[kind][slot];
      grid_[kind][slot] = value < 0 ? 0 : value > 1 ? 1 : value;
    }
  }
  day_ = in["day"] | 0;
  dayWeekend_ = in["dayWeekend"] | false;
  started_ = strtoull(in["started"] | "0", nullptr, 16);
  lastUse_ = in["lastUse"] | 0;
}
