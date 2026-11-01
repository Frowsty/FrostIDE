// SPDX-License-Identifier: Apache-2.0
//
// Private: UTC timestamps for file names (journal and quarantine names) and the nanosecond
// encoding of times stored in the databases.
#pragma once

#include "frost/core/clock.hpp"

#include <cstdint>
#include <string>

namespace frost::storage::detail {

// "yyyymmddTHHMMSSZ", or "yyyymmddTHHMMSSfffZ" with `milliseconds`. Times before year 0 or after
// 9999 are clamped to those years' bounds (file names keep a fixed width). O(1).
[[nodiscard]] std::string utcStamp(IClock::SystemTime time, bool milliseconds);

// Nanoseconds since the Unix epoch (the stores' timestamp columns) and back.
[[nodiscard]] constexpr std::int64_t toUnixNanos(IClock::SystemTime time) noexcept {
    return static_cast<std::int64_t>(time.time_since_epoch().count());
}
[[nodiscard]] constexpr IClock::SystemTime fromUnixNanos(std::int64_t nanos) noexcept {
    return IClock::SystemTime(IClock::Duration(nanos));
}

} // namespace frost::storage::detail
