#include "storage/runtime_log.h"

bool RuntimeLog::add(uint32_t date, uint32_t elapsedSec, const RuntimeSample &sample) {
  bool newDay = false;
  if (days_.empty() || days_.back().date != date) {
    RuntimeDay day;
    day.date = date;
    days_.push_back(day);
    while (days_.size() > MaxDays) {
      days_.pop_front();
    }
    newDay = true;
  }

  RuntimeDay &today = days_.back();
  if (sample.heating) {
    today.heatingSec += elapsedSec;
  }
  if (sample.pump1Speed == 1) {
    today.pumpLowSec += elapsedSec;
  } else if (sample.pump1Speed == 2) {
    today.pumpHighSec += elapsedSec;
  }
  if (sample.filterRunning) {
    today.filterSec += elapsedSec;
  }
  return newDay;
}

void RuntimeLog::load(const std::vector<RuntimeDay> &days) {
  days_.assign(days.begin(), days.end());
  while (days_.size() > MaxDays) {
    days_.pop_front();
  }
}
