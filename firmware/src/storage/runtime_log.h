#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <vector>

// Daily totals of how long the heater, pump 1 and filter cycles ran.
struct RuntimeDay {
  uint32_t date = 0;  // local date as YYYYMMDD
  uint32_t heatingSec = 0;
  uint32_t pumpLowSec = 0;
  uint32_t pumpHighSec = 0;
  uint32_t filterSec = 0;
  float waterTempAvg = NAN;  // time-weighted over waterTempSec
  uint32_t waterTempSec = 0;
  float setTempAvg = NAN;    // time-weighted over setTempSec
  uint32_t setTempSec = 0;
};

// One hour of history for the temperature chart.
struct RuntimeHour {
  uint32_t start = 0;  // Unix time at the top of the hour
  float waterTempAvg = NAN;
  uint32_t waterTempSec = 0;
  float setTempAvg = NAN;
  uint32_t setTempSec = 0;
  uint32_t heatingSec = 0;
};

struct RuntimeSample {
  bool heating = false;
  uint8_t pump1Speed = 0;  // 0 off, 1 low, 2 high
  bool filterRunning = false;
  float waterTemp = NAN;   // NAN while the spa hasn't reported one
  float setTemp = NAN;
};

class RuntimeLog {
 public:
  static constexpr size_t MaxDays = 30;
  static constexpr size_t MaxHours = 48;

  // Credits elapsedSec of `sample` to `date` and to the hour starting at hourStart.
  // Returns true when this starts a new day.
  bool add(uint32_t date, uint32_t hourStart, uint32_t elapsedSec, const RuntimeSample &sample);
  void load(const std::vector<RuntimeDay> &days, const std::vector<RuntimeHour> &hours = {});
  const std::deque<RuntimeDay> &days() const { return days_; }    // oldest first, today last
  const std::deque<RuntimeHour> &hours() const { return hours_; }  // oldest first, this hour last

 private:
  std::deque<RuntimeDay> days_;
  std::deque<RuntimeHour> hours_;
};
