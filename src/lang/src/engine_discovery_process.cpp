// SPDX-License-Identifier: Apache-2.0
// Run and supervise the FrostEngine automation discovery helper.
#include "frost/lang/engine_discovery_process.hpp"

#include "frost/io/async_process.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include <algorithm>

namespace frost::lang {
sched::Task<Result<Json>> requestEngineInHelper(io::IProcessLauncher& launcher, io::AsyncContext context,
                                                std::string executable, Json input, std::chrono::milliseconds timeout,
                                                std::stop_token stop) {
    if (!io::isAbsolutePath(executable) || !input.isObject() || timeout.count() <= 0)
        co_return fail(ErrorCode::InvalidArgument, "invalid engine helper request");
    if (stop.stop_requested())
        co_return fail(ErrorCode::Cancelled, "engine request cancelled");
    std::string request = input.dump();
    if (request.size() > 16 * 1024 * 1024)
        co_return fail(ErrorCode::OutOfRange, "engine request is too large");
    io::ProcessSpec spec;
    spec.executable = std::move(executable);
    spec.arguments = {"--internal-engine-request"};
    spec.stdinMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Null;
    auto process = launcher.launchAsync(spec, context);
    if (!process)
        co_return fail(process.error().code(), "cannot start engine helper");
    auto response = co_await (*process)->communicate(std::move(request), timeout, stop);
    if (!response)
        co_return fail(response.error().code(), "engine helper did not finish");
    if (response->exitCode || response->signal || response->stdoutBytes.size() > 16 * 1024 * 1024)
        co_return fail(ErrorCode::IoError, "engine helper failed");
    auto result = parseJson(response->stdoutBytes);
    if (!result || !result->isObject())
        co_return fail(ErrorCode::Corrupt, "invalid engine helper response");
    if (result->boolAt("ok") == false) {
        const auto code = result->integerAt("code").value_or(0);
        if (code < 0 || code > static_cast<int>(ErrorCode::Untrusted))
            co_return fail(ErrorCode::Corrupt, "invalid engine helper error");
        co_return fail(static_cast<ErrorCode>(code), std::string(result->stringAt("message")));
    }
    auto* value = result->find("value");
    if (result->boolAt("ok") != true || !value || !value->isObject())
        co_return fail(ErrorCode::Corrupt, "invalid engine helper response");
    co_return std::move(*value);
}
} // namespace frost::lang
