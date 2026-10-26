#include "storage/runtime_log.h"

namespace {

// Folds elapsedSec at `temp` into a time-weighted running average.
void addToAverage(float &average, uint32_t &seconds, float temp, uint32_t elapsedSec) {
  if (isnan(temp) || elapsedSec == 0) {
    return;
  }
  if (seconds == 0 || isnan(average)) {
    average = temp;
  } else {
    average += (temp - average) * static_cast<float>(elapsedSec) / static_cast<float>(seconds + elapsedSec);
  }
  seconds += elapsedSec;
}

}  // namespace

bool RuntimeLog::add(uint32_t date, uint32_t hourStart, uint32_t elapsedSec, const RuntimeSample &sample) {
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
  addToAverage(today.waterTempAvg, today.waterTempSec, sample.waterTemp, elapsedSec);
  addToAverage(today.setTempAvg, today.setTempSec, sample.setTemp, elapsedSec);

  if (hours_.empty() || hours_.back().start != hourStart) {
    RuntimeHour hour;
    hour.start = hourStart;
    hours_.push_back(hour);
    while (hours_.size() > MaxHours) {
      hours_.pop_front();
    }
  }
  RuntimeHour &hour = hours_.back();
  if (sample.heating) {
    hour.heatingSec += elapsedSec;
  }
  addToAverage(hour.waterTempAvg, hour.waterTempSec, sample.waterTemp, elapsedSec);
  addToAverage(hour.setTempAvg, hour.setTempSec, sample.setTemp, elapsedSec);
  return newDay;
}

void RuntimeLog::load(const std::vector<RuntimeDay> &days, const std::vector<RuntimeHour> &hours) {
  days_.assign(days.begin(), days.end());
  while (days_.size() > MaxDays) {
    days_.pop_front();
  }
  hours_.assign(hours.begin(), hours.end());
  while (hours_.size() > MaxHours) {
    hours_.pop_front();
  }
}
