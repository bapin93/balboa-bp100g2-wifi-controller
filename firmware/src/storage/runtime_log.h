#pragma once

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
};

struct RuntimeSample {
  bool heating = false;
  uint8_t pump1Speed = 0;  // 0 off, 1 low, 2 high
  bool filterRunning = false;
};

class RuntimeLog {
 public:
  static constexpr size_t MaxDays = 30;

  // Credits elapsedSec of `sample` to `date`. Returns true when this starts a new day.
  bool add(uint32_t date, uint32_t elapsedSec, const RuntimeSample &sample);
  void load(const std::vector<RuntimeDay> &days);
  const std::deque<RuntimeDay> &days() const { return days_; }  // oldest first, today last

 private:
  std::deque<RuntimeDay> days_;
};
