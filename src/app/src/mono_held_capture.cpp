// SPDX-License-Identifier: Apache-2.0
// Capture trusted Mono module and source bytes through held filesystem handles.
#include "mono_held_capture.hpp"

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

namespace frost::app {
namespace {
Result<std::string> runOwned(io::IProcessLauncher& launcher, io::ProcessSpec spec, std::optional<std::string> input,
                             std::size_t limit, std::chrono::milliseconds timeout, std::stop_token stop,
                             const std::function<bool()>& current) {
    if (stop.stop_requested() || !current())
        return fail(ErrorCode::Cancelled, "Mono helper admission was cancelled");
    auto launched = launcher.launch(spec);
    if (!launched)
        return std::unexpected(std::move(launched.error()));
    struct State {
        std::unique_ptr<io::IProcess> process;
        std::atomic<bool> cancelled{false}, overflow{false}, readFailure{false}, stdoutDone{false}, stderrDone{false};
        std::atomic<bool> writerDone{false}, writeFailure{false};
        std::string output;
    };
    auto state = std::make_shared<State>();
    state->process = std::move(*launched);
    auto stopChild = [state] {
        state->cancelled.store(true);
        (void)state->process->terminate();
        state->process->abandonOutput();
    };
    std::stop_callback onStop(stop, stopChild);
    std::thread stdoutReader;
    std::thread stderrReader;
    std::thread writer;
    if (!input)
        state->writerDone.store(true);
    try {
        if (input) {
            writer = std::thread([state, bytes = std::move(*input)] {
                if (!state->process->writeStdin(bytes))
                    state->writeFailure.store(true);
                state->process->closeStdin();
                state->writerDone.store(true);
            });
        }
        stdoutReader = std::thread([state, limit] {
            std::array<char, 8192> buffer{};
            for (;;) {
                auto count = state->process->readStdout(buffer);
                if (!count) {
                    state->readFailure.store(true);
                    break;
                }
                if (*count == 0)
                    break;
                if (*count > limit - state->output.size()) {
                    state->overflow.store(true);
                    (void)state->process->terminate();
                    break;
                }
                state->output.append(buffer.data(), *count);
            }
            state->stdoutDone.store(true);
        });
        stderrReader = std::thread([state] {
            std::array<char, 4096> buffer{};
            std::size_t total = 0;
            for (;;) {
                auto count = state->process->readStderr(buffer);
                if (!count) {
                    state->readFailure.store(true);
                    break;
                }
                if (*count == 0)
                    break;
                if (*count > 16384 - total) {
                    state->overflow.store(true);
                    (void)state->process->terminate();
                    break;
                }
                total += *count;
            }
            state->stderrDone.store(true);
        });
    } catch (...) {
        stopChild();
        if (stdoutReader.joinable())
            stdoutReader.detach();
        if (stderrReader.joinable())
            stderrReader.detach();
        if (writer.joinable())
            writer.detach();
        return fail(ErrorCode::IoError, "could not start held input readers");
    }
    const auto end = std::chrono::steady_clock::now() + timeout;
    bool exited = false;
    bool timedOut = false;
    int exitCode = -1;
    while (!state->cancelled.load() && !state->overflow.load()) {
        if (std::chrono::steady_clock::now() >= end) {
            timedOut = true;
            stopChild();
            break;
        }
        if (!current()) {
            stopChild();
            break;
        }
        auto status = state->process->wait(std::chrono::milliseconds(50));
        if (status) {
            exited = true;
            exitCode = status->exitCode;
            break;
        }
        if (status.error().code() != ErrorCode::Timeout) {
            state->readFailure.store(true);
            stopChild();
            break;
        }
    }
    if (!exited) {
        (void)state->process->terminate();
        state->process->abandonOutput();
    }
    const auto drainEnd = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!state->stdoutDone.load() || !state->stderrDone.load() || !state->writerDone.load()) &&
           std::chrono::steady_clock::now() < drainEnd)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!state->stdoutDone.load() || !state->stderrDone.load() || !state->writerDone.load()) {
        state->readFailure.store(true);
        (void)state->process->terminate();
        state->process->abandonOutput();
    }
    // An IProcess double may not implement abandonOutput. An unconfirmed read retains its own
    // process/state instead of blocking cancellation or accessing the caller after return.
    if (state->stdoutDone.load())
        stdoutReader.join();
    else
        stdoutReader.detach();
    if (state->stderrDone.load())
        stderrReader.join();
    else
        stderrReader.detach();
    if (writer.joinable()) {
        if (state->writerDone.load())
            writer.join();
        else
            writer.detach();
    }
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "held input admission was cancelled");
    if (timedOut)
        return fail(ErrorCode::Timeout, "held input admission timed out");
    if (state->cancelled.load() || !current())
        return fail(ErrorCode::Cancelled, "held input admission is no longer current");
    if (!exited || exitCode != 0 || state->overflow.load() || state->readFailure.load() || state->writeFailure.load())
        return fail(ErrorCode::IoError, "held input helper failed or exceeded its output limit");
    return std::move(state->output);
}
} // namespace
Result<std::string> captureMonoHeldInput(io::IProcessLauncher& launcher, std::string_view helper, std::string_view root,
                                         std::string_view path, std::size_t limit, std::chrono::milliseconds timeout,
                                         std::stop_token stop, const std::function<bool()>& current) {
    if (!io::isAbsolutePath(helper) || !io::isAbsolutePath(root) || !io::isAbsolutePath(path) ||
        limit > 4 * 1024 * 1024 || timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(60) ||
        !current)
        return fail(ErrorCode::InvalidArgument, "invalid held input request");
    io::ProcessSpec spec;
    spec.executable = helper;
    spec.arguments = {std::string(root), std::string(path), std::to_string(limit)};
    spec.workingDirectory = std::string(root);
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    return runOwned(launcher, std::move(spec), std::nullopt, limit, timeout, stop, current);
}
Result<std::string> runMonoGuardInput(io::IProcessLauncher& launcher, std::string_view dotnet, std::string_view guard,
                                      std::string_view root, bool mapPhase, std::string input,
                                      std::chrono::milliseconds timeout, std::stop_token stop,
                                      const std::function<bool()>& current) {
    constexpr std::size_t maxInput = 18 * 1024 * 1024 + 65536;
    constexpr std::size_t maxOutput = 4 * 1024 * 1024;
    if (!io::isAbsolutePath(dotnet) || !io::isAbsolutePath(guard) || !io::isAbsolutePath(root) || input.empty() ||
        input.size() > maxInput || timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(60) ||
        !current)
        return fail(ErrorCode::InvalidArgument, "invalid Mono guard request");
    io::ProcessSpec spec;
    spec.executable = dotnet;
    spec.arguments = {std::string(guard), mapPhase ? "--mono-map-pipe" : "--mono-documents-pipe"};
    if (mapPhase)
        spec.arguments.push_back(std::to_string(maxOutput));
    spec.workingDirectory = std::string(root);
    spec.stdinMode = io::Stdio::Pipe;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    return runOwned(launcher, std::move(spec), std::move(input), maxOutput, timeout, stop, current);
}
} // namespace frost::app
