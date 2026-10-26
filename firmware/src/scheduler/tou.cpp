#include "scheduler/tou.h"

#include <string.h>

#include <initializer_list>

namespace {

// Days since 1970-01-01 (Howard Hinnant's days_from_civil).
long daysFromCivil(int year, int month, int day) {
  year -= month <= 2;
  const long era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long>(doe) - 719468;
}

CivilDate civilFromDays(long days) {
  days += 719468;
  const long era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  CivilDate date;
  date.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  date.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  date.year = static_cast<int>(yoe) + static_cast<int>(era) * 400 + (date.month <= 2);
  return date;
}

CivilDate shifted(const CivilDate &date, int days) {
  return civilFromDays(daysFromCivil(date.year, date.month, date.day) + days);
}

// Day of the month of the nth `weekday` (n = 1..4), or the last one when n = 0.
int nthWeekday(int year, int month, int weekday, int n) {
  if (n == 0) {
    const CivilDate last = shifted(month == 12 ? CivilDate{year + 1, 1, 1} : CivilDate{year, month + 1, 1}, -1);
    return last.day - (weekdayOf(last) - weekday + 7) % 7;
  }
  const int first = weekdayOf({year, month, 1});
  return 1 + (weekday - first + 7) % 7 + 7 * (n - 1);
}

bool sameDay(const CivilDate &a, int month, int day) {
  return a.month == month && a.day == day;
}

// A fixed-date holiday, or the weekday it is observed on when it falls on a weekend.
bool isFixedHoliday(HolidaySet set, const CivilDate &date, int weekday, int month, int day) {
  if (sameDay(date, month, day)) {
    return true;
  }
  if (weekday == 1 && sameDay(shifted(date, -1), month, day)) {
    return true;  // Sunday holiday, observed Monday
  }
  return set != HolidaySet::Nerc && weekday == 5 && sameDay(shifted(date, 1), month, day);
}

}  // namespace

int weekdayOf(const CivilDate &date) {
  const long days = daysFromCivil(date.year, date.month, date.day);
  return static_cast<int>(((days % 7) + 7 + 4) % 7);  // 1970-01-01 was a Thursday
}

bool isHoliday(HolidaySet set, const CivilDate &date) {
  if (set == HolidaySet::None) {
    return false;
  }
  const int weekday = weekdayOf(date);
  const int y = date.year;
  const int m = date.month;
  const int d = date.day;
  constexpr int Monday = 1;
  constexpr int Thursday = 4;
  if (isFixedHoliday(set, date, weekday, 1, 1) || isFixedHoliday(set, date, weekday, 7, 4) ||
      isFixedHoliday(set, date, weekday, 12, 25)) {
    return true;
  }
  if ((m == 5 && d == nthWeekday(y, 5, Monday, 0)) || (m == 9 && d == nthWeekday(y, 9, Monday, 1)) ||
      (m == 11 && d == nthWeekday(y, 11, Thursday, 4))) {
    return true;  // Memorial Day, Labor Day, Thanksgiving
  }
  if (set == HolidaySet::Nerc) {
    return false;
  }
  if (set == HolidaySet::Federal && isFixedHoliday(set, date, weekday, 6, 19)) {
    return true;  // Juneteenth
  }
  return (m == 1 && d == nthWeekday(y, 1, Monday, 3)) ||   // Martin Luther King Jr. Day
         (m == 2 && d == nthWeekday(y, 2, Monday, 3)) ||   // Washington's Birthday
         (m == 10 && d == nthWeekday(y, 10, Monday, 2)) ||  // Columbus Day
         isFixedHoliday(set, date, weekday, 11, 11);        // Veterans Day
}

bool isPeakTime(const TouSchedule &schedule, const CivilDate &date, int hour) {
  if (schedule.peakStart == schedule.peakEnd) {
    return false;
  }
  const bool inWindow = schedule.peakStart < schedule.peakEnd
                            ? hour >= schedule.peakStart && hour < schedule.peakEnd
                            : hour >= schedule.peakStart || hour < schedule.peakEnd;
  if (!inWindow) {
    return false;
  }
  // An overnight window belongs to the day it started on.
  const CivilDate billedDay = schedule.peakStart > schedule.peakEnd && hour < schedule.peakEnd ? shifted(date, -1) : date;
  const int weekday = weekdayOf(billedDay);
  if (schedule.weekendsOffPeak && (weekday == 0 || weekday == 6)) {
    return false;
  }
  return !isHoliday(schedule.holidays, billedDay);
}

const char *holidaySetName(HolidaySet set) {
  switch (set) {
    case HolidaySet::None:
      return "none";
    case HolidaySet::Federal:
      return "federal";
    case HolidaySet::Nerc:
      return "nerc";
    case HolidaySet::FederalNoJuneteenth:
      return "federal10";
  }
  return "none";
}

bool holidaySetFromName(const char *name, HolidaySet *set) {
  for (HolidaySet candidate :
       {HolidaySet::None, HolidaySet::Federal, HolidaySet::Nerc, HolidaySet::FederalNoJuneteenth}) {
    if (strcmp(name, holidaySetName(candidate)) == 0) {
      *set = candidate;
      return true;
    }
  }
  return false;
}
