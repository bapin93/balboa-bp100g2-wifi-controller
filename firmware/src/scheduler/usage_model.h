#pragma once

#include <stdint.h>

#include <ArduinoJson.h>

// How likely a soak is to start in each half hour, separately for workdays and for weekends and holidays.
// Starts from what the owner said (rarely after midnight, in the morning or on weekday daytimes; evenings
// likely; weekend daytimes sometimes) and moves toward what actually happens: each finished day nudges every
// slot toward 1 if a soak started there (½ for the neighbouring slots) or toward 0 if not, so a slot's value
// tracks how often that half hour gets used. Weekends adapt faster since there are fewer of them.
class UsageModel {
 public:
  static constexpr int Slots = 48;  // half hours
  static constexpr uint32_t QuietGapSec = 2 * 3600;  // use after this long without any counts as a new soak
  static constexpr float WorkdayRate = 1.0f / 20;
  static constexpr float WeekendRate = 1.0f / 8;
  static constexpr float LikelyAbove = 0.25f;
  static constexpr float PossibleAbove = 0.04f;

  UsageModel();

  // Call with each status tick. `date` is the local YYYYMMDD and `slot` the local half hour (0-47).
  // inUse turning on after a quiet spell, or a manual raise of the set temp, marks a soak starting.
  void observe(uint32_t now, uint32_t date, bool weekend, int slot, bool inUse, bool raised);

  float likelihood(bool weekend, int slot) const;
  // Hours of heat-up the owner is willing to wait at this slot: 0.5 likely, 2 possible, 4 unlikely.
  float readyWithinHours(bool weekend, int slot) const;

  void toJson(JsonObject out) const;
  void fromJson(JsonVariantConst in);
  bool dirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }

 private:
  void fold();

  float grid_[2][Slots];  // [weekend][slot]
  uint32_t day_ = 0;  // the day being recorded
  bool dayWeekend_ = false;
  uint64_t started_ = 0;  // slots of that day where a soak started
  uint32_t lastUse_ = 0;
  bool wasInUse_ = false;
  bool dirty_ = false;
};
