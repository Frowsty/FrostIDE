// SPDX-License-Identifier: Apache-2.0
//
// The logging backend behind frost::log (frost/core/log.hpp): an spdlog asynchronous logger on
// its own one-thread pool that writes each record straight to a log file in <cache>/logs
// (ADR 0022, D-H).
//
/*
 * Records:   "2026-11-01T18:59:12.123456Z [info] [app/1234] message", UTC, one per line.
 * Queue:     1024 slots on the "frost-log" thread; when full the oldest queued record is
 *            dropped (overrun_oldest), so a logging call never blocks on the disk (Budget: B19
 *            for the slots, B5 for the caller's cost).
 * Files:     <logs>/frost-<yyyymmddThhmmssZ>-<pid>-<n>.log, created exclusively (an existing
 *            name moves on to the next n), opened lazily when the first record is written, and
 *            written without user-space buffering, so a crash loses only the records still
 *            queued. A file is rotated at 8 MiB; this process keeps its last 4 files and deletes
 *            older ones it wrote itself.
 * Sharing:   Windows opens the file with FILE_SHARE_READ only (other processes may read it,
 *            nobody may delete it while frost runs); Linux takes a shared flock on it, which
 *            retention probes before deleting (frost/diag/retention.hpp).
 * Failures:  a file that cannot be created or written disables the file output (records are
 *            then dropped and counted); logging itself never fails or throws.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/core/log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace frost::diag {

// One open log file. write() appends the bytes with one OS call (or a few, for partial writes).
class ILogFile {
public:
    virtual ~ILogFile() = default;
    [[nodiscard]] virtual Result<void> write(std::string_view bytes) noexcept = 0;
};

// Creates log files; the seam the tests replace.
class ILogFileOpener {
public:
    // Creates `path` exclusively. Errors: AlreadyExists (the name is taken), PermissionDenied,
    // NotFound (no directory), IoError.
    [[nodiscard]] virtual Result<std::unique_ptr<ILogFile>> create(const std::string& path) = 0;
    // Deletes a file this backend wrote earlier (rotation). Best effort.
    virtual void remove(const std::string& path) noexcept = 0;

protected:
    ~ILogFileOpener() = default;
};

// The real files (CreateFileW on the \\?\ path / open(2) with flock; see the header comment).
[[nodiscard]] ILogFileOpener& realLogFileOpener() noexcept;

// "frost-<yyyymmddThhmmssZ>-<pid>-<n>.log" for a start time `start`. Pure.
[[nodiscard]] std::string logFileName(IClock::SystemTime start, std::uint32_t pid, unsigned sequence);

struct LogBackendOptions {
    std::string directory;         // <cache>/logs, absolute; must exist.
    std::uint32_t pid = 0;         // For the file names.
    std::size_t queueSlots = 1024; // Budget: B19.
    std::uint64_t rotateBytes = 8u * 1024 * 1024;
    unsigned maxFilesPerProcess = 4;
    log::Level level = log::Level::Info;
};

// Counters for tests and the self-test; every field is monotonic.
struct LogBackendStats {
    std::uint64_t submitted = 0; // Records accepted by write().
    std::uint64_t written = 0;   // Records that reached a file.
    std::uint64_t overrun = 0;   // Records dropped because the queue was full.
    std::uint64_t failed = 0;    // Records dropped because no file could be written.
    unsigned filesCreated = 0;
};

/*
 * The backend: a log::ISink. Thread-safe: write() and flush() may be called from any thread.
 * Owned by its creator; frost's program keeps it for the life of the process ([[clang::no_destroy]],
 * see log::setSink's lifetime rule) and calls flushFor() during shutdown.
 */
class LogBackend final : public log::ISink {
public:
    // Starts the "frost-log" thread. No file exists until the first record arrives.
    // Errors: InvalidArgument (empty directory, zero slots), Internal (spdlog refused).
    [[nodiscard]] static Result<std::unique_ptr<LogBackend>> start(const LogBackendOptions& options,
                                                                   ILogFileOpener& opener, const IClock& clock);

    LogBackend(const LogBackend&) = delete ("owns the logging thread");
    LogBackend& operator=(const LogBackend&) = delete ("owns the logging thread");
    // Drains the queue and joins the thread.
    ~LogBackend();

    void write(log::Level level, std::string_view category, std::string_view message) noexcept override;
    // Queues a flush; returns at once. The file has no user-space buffer, so a flush only waits
    // for the queue, see flushFor().
    void flush() noexcept override;

    /*
     * Waits until every record submitted before the call was written or dropped, or until
     * `deadline` passed (Budget: B20, 50 ms at shutdown). Returns true when the queue drained.
     */
    [[nodiscard]] bool flushFor(std::chrono::milliseconds deadline) noexcept;

    [[nodiscard]] LogBackendStats stats() const noexcept;
    // The current file's path, or "" before the first record.
    [[nodiscard]] std::string currentFile() const;

    struct Impl;

private:
    explicit LogBackend(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

// Parses FROST_LOG_LEVEL ("trace" .. "off", log::parseLevel's names); `fallback` when unset or unknown.
[[nodiscard]] log::Level logLevelFromEnvironment(log::Level fallback) noexcept;

} // namespace frost::diag
