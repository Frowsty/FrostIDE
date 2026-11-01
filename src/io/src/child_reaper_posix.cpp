// SPDX-License-Identifier: Apache-2.0
//
/*
 * The POSIX child reaper's primitives (process_internal.hpp; ADR 0003, ADR 0020): pidfds, the
 * non-reaping exit probe and the reaps. A child is reaped only by its owner, by pid, never with
 * waitpid(-1) or wait(): until the owner reaps it, the zombie keeps the pid (and with killOnClose the
 * process-group id) reserved, so kill() can never reach an unrelated process. The async process watches
 * the pidfd (or polls probeExit on a timer) and reaps once its public object is gone and the child has
 * exited (async_process_posix.cpp). Asio-free.
 */

#include "frost/io/os_error.hpp"

#include "process_internal.hpp"
#include <sys/syscall.h>
#include <sys/wait.h>

#include <cerrno>
#include <csignal>

#include <unistd.h>

namespace frost::io::detail {

UniqueFd openPidfd(pid_t pid) noexcept {
#ifdef SYS_pidfd_open
    return UniqueFd(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
#else
    static_cast<void>(pid);
    return UniqueFd();
#endif
}

Result<std::optional<ExitStatus>> probeExit(pid_t pid) {
    for (;;) {
        siginfo_t info{};
        if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT | WNOHANG) != 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(errnoError(errno, "waitid"));
        }
        if (info.si_pid == 0) {
            return std::optional<ExitStatus>{}; // Still running.
        }
        if (info.si_code == CLD_EXITED) {
            return std::optional<ExitStatus>(ExitStatus{info.si_status, 0});
        }
        return std::optional<ExitStatus>(ExitStatus{128 + info.si_status, info.si_status}); // Killed or dumped.
    }
}

bool reapIfExited(pid_t pid) noexcept {
    int status = 0;
    for (;;) {
        const pid_t got = ::waitpid(pid, &status, WNOHANG);
        if (got == pid) {
            return true;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        return false; // Still running (0), or not ours any more (ECHILD).
    }
}

void reapAfterKill(pid_t pid) noexcept {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

} // namespace frost::io::detail
