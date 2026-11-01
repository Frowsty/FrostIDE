// SPDX-License-Identifier: Apache-2.0
//
// The platform half of the crash reporter and the fatal paths (crash_reporter_win.cpp,
// crash_reporter_posix.cpp), shared with crash_reporter.cpp and fatal_handlers.cpp. Not part
// of frost::diag's interface.
#pragma once

#include "frost/diag/crash_reporter.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace frost::diag::detail {

// The handler a successful launch left running.
struct HandlerConnection {
    std::unique_ptr<io::IProcess> process;
    std::string pipeName; // Windows only.
};

// Launches the handler for `config` and registers this process with it (crash_reporter.hpp).
[[nodiscard]] Result<HandlerConnection> launchAndRegister(io::IProcessLauncher& launcher, const IClock& clock,
                                                          ICrashpadConnector& connector,
                                                          const CrashReporterConfig& config);

// Whether this process runs with a full administrator token (Windows); false on Linux.
[[nodiscard]] bool processIsElevated() noexcept;

// Writes `text` to the standard error handle with one OS call (WriteFile / write(2)).
void writeToStderr(std::string_view text) noexcept;

// Captures up to `capacity` return addresses of the calling thread, skipping `skip` frames
// (Windows: RtlCaptureStackBackTrace; Linux: _Unwind_Backtrace, see crash_reporter_posix.cpp).
// Allocation-free and usable in a signal handler. Returns the number written.
[[nodiscard]] std::size_t captureFrames(std::uintptr_t* frames, std::size_t capacity, std::size_t skip) noexcept;

// The calling thread's stack, skipping `skip` frames, as lines "\n    <module>+0x<offset>"
// (cpptrace::generate_object_trace, at most 32 frames). Allocates; never throws. For logs.
[[nodiscard]] std::string describeStack(std::size_t skip);

// Ends the process through the registered handler with a report for the calling thread
// (Windows: DumpAndCrash with kFatalExceptionCode; Linux: raise(SIGABRT)). Falls back to
// endWithoutReport() if the handler path returns.
[[noreturn]] void crashThroughHandler() noexcept;

// Writes a report of the calling thread and returns (crashpad's DumpWithoutCrash).
void dumpThroughHandler() noexcept;

// Ends the process without a report (see ICrashReporter::crashNow).
[[noreturn]] void endWithoutReport() noexcept;

/*
 * The fatal section: the first thread to call it proceeds; the same thread calling again (a
 * fault while reporting) ends the process at once without a report; any other thread waits
 * forever (the first one ends the process). Allocation-free.
 */
void enterFatalSection() noexcept;

// Sets a crashpad annotation (no-op until registerAnnotations() ran). Allocation-free once
// registered; values are truncated to the annotation's size.
void setAnnotation(AnnotationKey key, std::string_view value) noexcept;
// Registers crashpad's annotation list for this process (allocates once). Idempotent.
void registerAnnotations() noexcept;
// Whether frost.frames was set since start (the violation handler sets it before crashNow).
[[nodiscard]] bool framesAnnotated() noexcept;

// A fixed-capacity text buffer for fatal paths: append() truncates, never allocates.
template<std::size_t Capacity>
struct FixedText {
    char data[Capacity];
    std::size_t size = 0;

    FixedText& append(std::string_view text) noexcept {
        for (const char c : text) {
            if (size == Capacity) {
                break;
            }
            data[size++] = c;
        }
        return *this;
    }
    [[nodiscard]] std::string_view view() const noexcept { return {data, size}; }
};

// Formats frames as "0x... 0x..." into `buffer`; returns the length. Allocation-free.
[[nodiscard]] std::size_t formatFrames(const std::uintptr_t* frames, std::size_t count, char* buffer,
                                       std::size_t capacity) noexcept;

} // namespace frost::diag::detail
