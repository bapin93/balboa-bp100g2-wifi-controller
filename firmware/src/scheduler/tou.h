#pragma once

#include <stdint.h>

// Which holidays the utility bills at off-peak rates all day.
enum class HolidaySet : uint8_t {
  None,
  Federal,  // the 11 US federal holidays; Saturday ones observed Friday, Sunday ones Monday
  Nerc,     // the 6 NERC holidays many utilities use; only Sunday ones move (to Monday)
  FederalNoJuneteenth,  // the federal list before Juneteenth was added; some utilities still use it
};

// Time-of-use peak window. Hours are local, 0-24; the peak runs from peakStart up to peakEnd
// and may wrap past midnight.
struct TouSchedule {
  uint8_t peakStart = 17;
  uint8_t peakEnd = 21;
  bool weekendsOffPeak = true;
  HolidaySet holidays = HolidaySet::Federal;
};

struct CivilDate {
  int year = 0;
  int month = 0;  // 1-12
  int day = 0;    // 1-31
};

// 0 = Sunday ... 6 = Saturday.
int weekdayOf(const CivilDate &date);
bool isHoliday(HolidaySet set, const CivilDate &date);
bool isPeakTime(const TouSchedule &schedule, const CivilDate &date, int hour);

const char *holidaySetName(HolidaySet set);
bool holidaySetFromName(const char *name, HolidaySet *set);
