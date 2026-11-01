// SPDX-License-Identifier: Apache-2.0
//
/*
 * The seam between the real launchers' process creation and the objects that own the child (frost::io
 * internals; ADR 0003, ADR 0020): process_win.cpp / process_posix.cpp start a child (their spawn
 * functions return a SpawnedChild), then wrap it either in the blocking IProcess or, through
 * makeAsyncProcess (async_process_win.cpp / async_process_posix.cpp), in an IAsyncProcess bound to the
 * reactor. Asio-free.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/async_process.hpp"
#include "frost/io/process.hpp"

#include "internal.hpp"

#include <memory>
#include <optional>

#ifndef _WIN32
#include <sys/types.h>
#endif

namespace frost::io::detail {

/*
 * A started child before an IProcess or IAsyncProcess owns it. Windows: the process handle, the job
 * (valid with killOnClose; closing it ends the tree) and the parent's overlapped pipe ends. POSIX: the pid
 * (never reaped yet), whether it leads its own process group (killOnClose), and the parent's non-blocking,
 * close-on-exec pipe ends. Pipe ends are invalid for streams that are not Stdio::Pipe.
 */
struct SpawnedChild {
#ifdef _WIN32
    UniqueHandle process;
    UniqueHandle job;
    ProcessId pid = 0;
    UniqueHandle in;
    UniqueHandle out;
    UniqueHandle err;
#else
    pid_t pid = 0;
    bool ownGroup = false;
    UniqueFd in;
    UniqueFd out;
    UniqueFd err;
#endif
};

/*
 * Binds a started child to context.reactor (async_process_win.cpp / async_process_posix.cpp). Errors:
 * Cancelled (the reactor is shutting down; the child is then killed, and reaped on POSIX, before
 * returning), IoError (the pipes cannot be registered with the reactor).
 */
[[nodiscard]] Result<std::unique_ptr<IAsyncProcess>> makeAsyncProcess(SpawnedChild child, const AsyncContext& context);

// The checks launchAsync() makes before starting anything. Errors: InvalidArgument (a caller-driven
// scheduler), Cancelled (the reactor is shut down).
[[nodiscard]] Result<void> checkAsyncContext(const AsyncContext& context);

#ifndef _WIN32
/*
 * The POSIX child reaper's primitives (child_reaper_posix.cpp; ADR 0003's rule: a child is reaped only by
 * its owner, by pid, never with waitpid(-1)). All thread-safe.
 */
// A pidfd for `pid` (Linux >= 5.3, readable once the process exits), or an invalid descriptor.
[[nodiscard]] UniqueFd openPidfd(pid_t pid) noexcept;
// The exit status if `pid` has exited, leaving it unreaped (waitid WNOWAIT | WNOHANG); nullopt while it
// runs. Errors: IoError (not our child, or already reaped).
[[nodiscard]] Result<std::optional<ExitStatus>> probeExit(pid_t pid);
// Reaps `pid` if it has exited (waitpid WNOHANG); true once it is reaped.
[[nodiscard]] bool reapIfExited(pid_t pid) noexcept;
// Reaps `pid`, waiting for it to exit (waitpid without WNOHANG). Only for a child just sent SIGKILL.
void reapAfterKill(pid_t pid) noexcept;
#endif

} // namespace frost::io::detail
