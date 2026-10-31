// SPDX-License-Identifier: Apache-2.0
//
// The process-wide SystemClock (see frost/core/clock.hpp).

#include "frost/core/clock.hpp"

namespace frost {
namespace {

// constinit and trivially destructible: no code runs before main or after exit.
constinit SystemClock g_systemClock;

} // namespace

IClock& systemClock() noexcept { return g_systemClock; }

} // namespace frost
