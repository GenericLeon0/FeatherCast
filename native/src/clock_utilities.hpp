#pragma once

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace feathercast::clock_utilities {

struct ClockSnapshot {
  std::chrono::year_month_day localDate{};
  int localHour = 0;
  int localMinute = 0;
  int localSecond = 0;
  long long epochSeconds = 0;
};

struct Result {
  std::wstring stableId;
  std::wstring title;
  std::wstring value;
  std::vector<std::wstring> keywords;
};

inline std::wstring Normalize(std::wstring value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) {
    return std::iswspace(ch) != 0;
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) {
    return std::iswspace(ch) != 0;
  }).base();
  if (first >= last) return L"";
  value = std::wstring(first, last);
  for (auto& ch : value) ch = static_cast<wchar_t>(std::towlower(ch));
  return value;
}

inline std::wstring TwoDigits(int value) {
  std::wostringstream out;
  out << std::setfill(L'0') << std::setw(2) << value;
  return out.str();
}

inline std::wstring FourDigits(int value) {
  std::wostringstream out;
  out << std::setfill(L'0') << std::setw(4) << value;
  return out.str();
}

inline std::pair<int, unsigned> IsoWeek(std::chrono::year_month_day date) {
  using namespace std::chrono;
  const sys_days day{date};
  const unsigned isoWeekday = weekday{day}.iso_encoding();
  const sys_days thursday = day + days{4 - static_cast<int>(isoWeekday)};
  const year isoYear = year_month_day{thursday}.year();
  const sys_days januaryFourth = sys_days{isoYear / January / 4};
  const unsigned januaryFourthWeekday = weekday{januaryFourth}.iso_encoding();
  const sys_days weekOneMonday =
      januaryFourth - days{static_cast<int>(januaryFourthWeekday) - 1};
  const auto week = static_cast<unsigned>(
      (day - weekOneMonday).count() / 7 + 1);
  return {static_cast<int>(isoYear), week};
}

inline std::optional<Result> Evaluate(std::wstring query,
                                      const ClockSnapshot& clock) {
  query = Normalize(std::move(query));
  const int year = static_cast<int>(clock.localDate.year());
  const unsigned month = static_cast<unsigned>(clock.localDate.month());
  const unsigned day = static_cast<unsigned>(clock.localDate.day());

  if (query == L"time" || query == L"local time") {
    return Result{L"local-time", L"Local Time",
                  TwoDigits(clock.localHour) + L":" +
                      TwoDigits(clock.localMinute) + L":" +
                      TwoDigits(clock.localSecond),
                  {L"time", L"local time", L"clock"}};
  }
  if (query == L"date" || query == L"today") {
    return Result{L"local-date", L"Local Date",
                  FourDigits(year) + L"-" +
                      TwoDigits(static_cast<int>(month)) + L"-" +
                      TwoDigits(static_cast<int>(day)),
                  {L"date", L"today", L"calendar"}};
  }
  if (query == L"week number" || query == L"iso week") {
    const auto [isoYear, week] = IsoWeek(clock.localDate);
    return Result{L"iso-week", L"ISO Week",
                  FourDigits(isoYear) + L"-W" +
                      TwoDigits(static_cast<int>(week)),
                  {L"week", L"week number", L"iso week"}};
  }
  if (query == L"unix time" || query == L"unix timestamp") {
    return Result{L"unix-time", L"Unix Time",
                  std::to_wstring(clock.epochSeconds),
                  {L"unix", L"timestamp", L"epoch"}};
  }
  return std::nullopt;
}

}  // namespace feathercast::clock_utilities
