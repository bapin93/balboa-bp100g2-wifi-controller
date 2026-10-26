#include <unity.h>

#include "scheduler/tou.h"

void test_weekdays() {
  TEST_ASSERT_EQUAL_INT(4, weekdayOf({1970, 1, 1}));   // Thursday
  TEST_ASSERT_EQUAL_INT(1, weekdayOf({2026, 10, 26}));  // Monday
  TEST_ASSERT_EQUAL_INT(6, weekdayOf({2026, 7, 4}));    // Saturday
}

void test_floating_holidays() {
  TEST_ASSERT_TRUE(isHoliday(HolidaySet::Federal, {2026, 5, 25}));   // Memorial Day, last Monday
  TEST_ASSERT_FALSE(isHoliday(HolidaySet::Federal, {2026, 5, 18}));
  TEST_ASSERT_TRUE(isHoliday(HolidaySet::Nerc, {2026, 9, 7}));       // Labor Day
  TEST_ASSERT_TRUE(isHoliday(HolidaySet::Nerc, {2026, 11, 26}));     // Thanksgiving
  TEST_ASSERT_TRUE(isHoliday(HolidaySet::Federal, {2026, 1, 19}));   // MLK Day
  TEST_ASSERT_FALSE(isHoliday(HolidaySet::Nerc, {2026, 1, 19}));     // not a NERC holiday
  TEST_ASSERT_TRUE(isHoliday(HolidaySet::Federal, {2026, 10, 12}));  // Columbus Day
  TEST_ASSERT_FALSE(isHoliday(HolidaySet::None, {2026, 12, 25}));
}

void test_weekend_holidays_are_observed() {
  // July 4, 2026 is a Saturday: federal observes Friday the 3rd, NERC doesn't move it.
  TEST_ASSERT_TRUE(isHoliday(HolidaySet::Federal, {2026, 7, 3}));
  TEST_ASSERT_FALSE(isHoliday(HolidaySet::Nerc, {2026, 7, 3}));
  // New Year's 2028 is a Saturday: observed Friday, Dec 31, 2027.
  TEST_ASSERT_TRUE(isHoliday(HolidaySet::Federal, {2027, 12, 31}));
  // Juneteenth 2033 is a Sunday: observed Monday the 20th.
  TEST_ASSERT_TRUE(isHoliday(HolidaySet::Federal, {2033, 6, 20}));
  // Christmas 2033 is a Sunday: both sets observe Monday the 26th.
  TEST_ASSERT_TRUE(isHoliday(HolidaySet::Nerc, {2033, 12, 26}));
}

void test_federal_without_juneteenth() {
  TEST_ASSERT_FALSE(isHoliday(HolidaySet::FederalNoJuneteenth, {2026, 6, 19}));
  TEST_ASSERT_FALSE(isHoliday(HolidaySet::FederalNoJuneteenth, {2033, 6, 20}));  // nor its observed day
  TEST_ASSERT_TRUE(isHoliday(HolidaySet::FederalNoJuneteenth, {2026, 1, 19}));   // MLK Day
  TEST_ASSERT_TRUE(isHoliday(HolidaySet::FederalNoJuneteenth, {2026, 11, 11}));  // Veterans Day
  TEST_ASSERT_TRUE(isHoliday(HolidaySet::FederalNoJuneteenth, {2026, 7, 3}));    // July 4 observed Friday
  HolidaySet set = HolidaySet::None;
  TEST_ASSERT_TRUE(holidaySetFromName("federal10", &set));
  TEST_ASSERT_EQUAL(HolidaySet::FederalNoJuneteenth, set);
}

void test_peak_window() {
  TouSchedule tou;  // 5-9 PM, weekends and federal holidays off-peak
  const CivilDate monday{2026, 10, 26};
  TEST_ASSERT_FALSE(isPeakTime(tou, monday, 16));
  TEST_ASSERT_TRUE(isPeakTime(tou, monday, 17));
  TEST_ASSERT_TRUE(isPeakTime(tou, monday, 20));
  TEST_ASSERT_FALSE(isPeakTime(tou, monday, 21));
  TEST_ASSERT_FALSE(isPeakTime(tou, {2026, 10, 24}, 18));  // Saturday
  TEST_ASSERT_FALSE(isPeakTime(tou, {2026, 11, 26}, 18));  // Thanksgiving
  tou.weekendsOffPeak = false;
  TEST_ASSERT_TRUE(isPeakTime(tou, {2026, 10, 24}, 18));
}

void test_overnight_window_belongs_to_its_start_day() {
  TouSchedule tou;
  tou.peakStart = 22;
  tou.peakEnd = 2;
  TEST_ASSERT_TRUE(isPeakTime(tou, {2026, 10, 23}, 23));  // Friday night
  TEST_ASSERT_TRUE(isPeakTime(tou, {2026, 10, 24}, 1));   // after midnight, still Friday's window
  TEST_ASSERT_FALSE(isPeakTime(tou, {2026, 10, 25}, 1));  // Saturday night's window
  TEST_ASSERT_FALSE(isPeakTime(tou, {2026, 10, 24}, 3));
}

void test_names_round_trip() {
  HolidaySet set = HolidaySet::None;
  TEST_ASSERT_TRUE(holidaySetFromName("nerc", &set));
  TEST_ASSERT_EQUAL(HolidaySet::Nerc, set);
  TEST_ASSERT_FALSE(holidaySetFromName("bank", &set));
  TEST_ASSERT_EQUAL_STRING("federal", holidaySetName(HolidaySet::Federal));
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_weekdays);
  RUN_TEST(test_floating_holidays);
  RUN_TEST(test_weekend_holidays_are_observed);
  RUN_TEST(test_federal_without_juneteenth);
  RUN_TEST(test_peak_window);
  RUN_TEST(test_overnight_window_belongs_to_its_start_day);
  RUN_TEST(test_names_round_trip);
  return UNITY_END();
}
