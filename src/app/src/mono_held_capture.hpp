// SPDX-License-Identifier: Apache-2.0
// Worker-only invocations of owned input and source-verification helpers.
#pragma once
#include "frost/core/error.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
namespace frost::io {
class IProcessLauncher;
}
namespace frost::app {
// The owner requests stop immediately on workspace/trust/session change. `current` is thread-safe
// and must bind the selected trusted root and generation. Helper integrity is the owner's responsibility.
[[nodiscard]] Result<std::string> captureMonoHeldInput(io::IProcessLauncher& launcher, std::string_view helper,
                                                       std::string_view root, std::string_view path, std::size_t limit,
                                                       std::chrono::milliseconds timeout, std::stop_token stop,
                                                       const std::function<bool()>& current);
// Sends framed snapshots to the selected owned helper; successful output is bounded and current.
[[nodiscard]] Result<std::string> runMonoGuardInput(io::IProcessLauncher& launcher, std::string_view dotnet,
                                                    std::string_view guard, std::string_view root, bool mapPhase,
                                                    std::string input, std::chrono::milliseconds timeout,
                                                    std::stop_token stop, const std::function<bool()>& current);
} // namespace frost::app
