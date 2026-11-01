// SPDX-License-Identifier: Apache-2.0
//
// Private: UTC file-name timestamps (see time_names.hpp).

#include "time_names.hpp"

#include <chrono>
#include <format>

namespace frost::storage::detail {

std::string utcStamp(IClock::SystemTime time, bool milliseconds) {
    using namespace std::chrono;
    // The proleptic Gregorian range std::chrono::year_month_day handles with 4-digit years.
    constexpr sys_days kFirst = sys_days(year(0) / January / 1);
    constexpr sys_days kLast = sys_days(year(9999) / December / 31);
    const auto ms = floor<std::chrono::milliseconds>(time);
    auto day = floor<days>(ms);
    auto sinceMidnight = ms - day;
    if (day < kFirst) {
        day = kFirst;
        sinceMidnight = std::chrono::milliseconds(0);
    } else if (day > kLast) {
        day = kLast;
        sinceMidnight = hours(24) - std::chrono::milliseconds(1);
    }
    const year_month_day date(day);
    const hh_mm_ss<std::chrono::milliseconds> clock(sinceMidnight);
    const std::string stamp = std::format("{:04}{:02}{:02}T{:02}{:02}{:02}", static_cast<int>(date.year()),
                                          static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                                          clock.hours().count(), clock.minutes().count(), clock.seconds().count());
    if (milliseconds) {
        return std::format("{}{:03}Z", stamp, clock.subseconds().count());
    }
    return stamp + "Z";
}

} // namespace frost::storage::detail
