// SPDX-License-Identifier: Apache-2.0
// Isolated helper process protocol for FrostEngine discovery and requests.
#pragma once
#include "frost/core/error.hpp"
#include "frost/lang/json.hpp"
#include "frost/sched/task.hpp"

#include <chrono>
#include <stop_token>
#include <string_view>
namespace frost::io {
class IProcessLauncher;
struct AsyncContext;
} // namespace frost::io
namespace frost::lang {
// Executes one complete engine request in the contained helper, including all path resolution.
// Call only after project trust is checked. Cancellation destroys the async process without waiting
// for a stalled child; the reactor retains responsibility for reaping it. Credentials stay there.
[[nodiscard]] sched::Task<Result<Json>> requestEngineInHelper(io::IProcessLauncher& launcher, io::AsyncContext context,
                                                              std::string executable, Json input,
                                                              std::chrono::milliseconds timeout,
                                                              std::stop_token stop = {});
} // namespace frost::lang
