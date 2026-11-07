// SPDX-License-Identifier: Apache-2.0
//
// SpawnLogLauncher: an IProcessLauncher decorator that records every child process it starts
// (FND-8, ADR 0078 decision 1), so a test can prove that a result came from a real tool and which
// one: the absolute executable path with its SHA-256, argv, the working directory, the pid, and the
// start and exit times, one JSON object per line in a file per session. Test builds wire it in
// through FROST_SPAWN_LOG_DIR (src/app, built with FROST_ENABLE_SPAWN_LOG); the product UI never
// shows it.
/*
 * The file. <directory>/spawn-<UTC yyyymmddThhmmss.mmm>-<pid>.jsonl, created (with the directory)
 * by open(). Each record is one line of UTF-8 JSON with an "event":
 *   {"event":"start","id":1,"time":"2026-11-07T20:01:02.345Z","executable":"C:\\...\\clangd.exe",
 *    "sha256":"<64 hex digits>","argv":["<executable>","--log=error"],"cwd":"","pid":4242,
 *    "async":true}
 *     A launch that succeeded. argv[0] is the executable (ProcessSpec's convention); a Windows
 *     `rawArguments` line is recorded as "rawArguments" instead of argv[1..]. "cwd" is
 *     ProcessSpec::workingDirectory ("" for the parent's). When the executable cannot be read the
 *     record has "sha256":null and a "sha256Error" with the reason; the launch is not affected.
 *   {"event":"launch-failed","id":2,"time":...,"executable":...,"argv":[...],"cwd":...,
 *    "error":"<frost::Error::describe()>"}
 *     A launch the inner launcher refused; the error is returned to the caller unchanged.
 *   {"event":"exit","id":1,"time":...,"exitCode":0,"signal":0}
 *     The first time the process's exit status is observed through it (wait, communicate, exited).
 *     A process that is never waited for has no exit record.
 * Ids count launches from 1 in this session. Times come from the given IClock's systemNow().
 *
 * Hashing. The SHA-256 (FIPS 180-4) of the executable's bytes, read through the IFileSystem
 * before the launch, is cached by path, size and modification time, so each image is read once
 * while it stays unchanged. O(file size) for the first launch of an image, O(1) afterwards. The
 * hashing runs on the launching thread (a large image such as clangd costs seconds in a Debug
 * build), which is acceptable only because the log is a test instrument.
 *
 * Writing. Every record rewrites the session file atomically (IFileSystem::writeFileAtomic) with
 * all records so far: a crash leaves a complete file, and the log works on a MemFs in unit tests.
 * O(total log size) per record, fine for the few hundred launches of a test session. A write that
 * fails does not fail the launch; lastWriteError() reports it.
 *
 * Ownership and threads. The decorator borrows the inner launcher, the file system and the clock,
 * which must outlive it and every process it returned. Processes it returned share the log with
 * it, so they may outlive the decorator itself. All members are thread-safe.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/async_process.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace frost::io {

// The SHA-256 of `bytes` (FIPS 180-4) as 64 lowercase hex digits. O(bytes); thread-safe.
[[nodiscard]] std::string sha256Hex(std::string_view bytes);

class SpawnLogLauncher final : public IProcessLauncher {
public:
    /*
     * A decorator over `inner` that logs to a new session file in `directory` (absolute; created
     * when missing). Errors: InvalidArgument (a relative directory), those of createDirectories and
     * of writing the empty session file.
     */
    [[nodiscard]] static Result<std::unique_ptr<SpawnLogLauncher>>
    open(IProcessLauncher& inner, IFileSystem& fs, const IClock& clock, std::string_view directory);

    ~SpawnLogLauncher();
    SpawnLogLauncher(const SpawnLogLauncher&) = delete ("the session file has one writer");
    SpawnLogLauncher& operator=(const SpawnLogLauncher&) = delete ("the session file has one writer");

    // As IProcessLauncher; each launch adds a start or launch-failed record first (see above).
    [[nodiscard]] Result<std::unique_ptr<IProcess>> launch(const ProcessSpec& spec) override;
    [[nodiscard]] Result<std::unique_ptr<IAsyncProcess>> launchAsync(const ProcessSpec& spec,
                                                                     const AsyncContext& context) override;

    // The session file's absolute path.
    [[nodiscard]] const std::string& path() const noexcept;
    // The last failed write of the session file, or nullopt when every write succeeded.
    [[nodiscard]] std::optional<Error> lastWriteError() const;

    struct State; // Shared with the processes it returned; defined in the implementation.

private:
    SpawnLogLauncher(IProcessLauncher& inner, std::shared_ptr<State> state);

    IProcessLauncher& m_inner;
    std::shared_ptr<State> m_state;
};

} // namespace frost::io
