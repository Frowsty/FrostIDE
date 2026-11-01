// SPDX-License-Identifier: Apache-2.0
//
/*
 * IAsyncProcess on POSIX (ADR 0020): the parent's non-blocking pipe ends (process_posix.cpp) are
 * registered with the reactor's epoll instance through asio::posix::stream_descriptor; the exit is
 * watched through the child's pidfd (readable once it exits; Linux >= 5.3), or, without one, by polling
 * waitid(WNOHANG | WNOWAIT) on a strand timer backing off from 10 to 50 ms.
 */
/*
 * Reaping (the ChildReaper; child_reaper_posix.cpp). The exit watch never reaps: the zombie keeps the pid
 * and the process-group id reserved while the public object lives, so terminate() can never signal an
 * unrelated process. Release (the object destroyed) closes the pipes and, with killOnClose, kills the
 * process group; the watch then keeps the state alive until the child has exited and reaps it by pid.
 * Without killOnClose the child keeps running and is reaped whenever it exits. If the reactor shuts
 * down first, the watch stops, and a child still running when the last reference drops is left
 * unreaped (killed first with killOnClose); it is reparented when frost exits.
 *
 * SIGPIPE: every write is initiated on the strand, so it runs on a reactor thread, which blocks SIGPIPE
 * for its lifetime (reactor.cpp); EPIPE comes back as an error.
 */

#include "frost/core/assert.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/reactor.hpp"

#include "async_process_impl.hpp" // asio first.
#include "process_internal.hpp"
#include <sys/types.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <format>
#include <memory>
#include <optional>
#include <utility>

namespace frost::io::detail {
namespace {

using namespace std::chrono_literals;

// The exit poll's first and longest period when there is no pidfd.
constexpr auto kFirstPoll = 10ms;
constexpr auto kLongestPoll = 50ms;

class PosixAsyncProcess final : public AsyncProcessState {
public:
    PosixAsyncProcess(Reactor::Impl& reactor, sched::IScheduler& scheduler, SpawnedChild& child)
        : AsyncProcessState(reactor, scheduler, static_cast<ProcessId>(child.pid), child.in.valid(), child.out.valid(),
                            child.err.valid()),
          m_pid(child.pid), m_ownGroup(child.ownGroup) {}

    // The last reference is gone, so no handler runs: reap if possible (see the file comment).
    ~PosixAsyncProcess() override {
        if (!m_reaped) {
            if (m_ownGroup) {
                static_cast<void>(::kill(-m_pid, SIGKILL));
            }
            static_cast<void>(reapIfExited(m_pid));
        }
    }

    Result<void> terminateChild() override {
        // Never reaped while the public object lives, so m_pid cannot have been reused.
        const pid_t target = m_ownGroup ? -m_pid : m_pid;
        if (::kill(target, SIGKILL) != 0 && errno != ESRCH) {
            return std::unexpected(errnoError(errno, "kill"));
        }
        return {};
    }

    // launchAsync() failed after the spawn: the child was never handed out.
    void killAndReapNow() noexcept {
        static_cast<void>(terminateChild());
        reapAfterKill(m_pid);
        m_reaped = true;
    }

protected:
    void startExitWatch() override {
        UniqueFd pidfd = openPidfd(m_pid);
        if (pidfd.valid()) {
            m_pidfd.emplace(reactor().context);
            std::error_code ec;
            static_cast<void>(m_pidfd->assign(pidfd.get(), ec));
            if (!ec) {
                static_cast<void>(pidfd.release());
                armPidfd();
                /*
                 * asio registers descriptors edge-triggered and does not try a wait speculatively, so
                 * readiness that another reactor thread processed between assign() and async_wait() (a
                 * child that exited that early) would be dropped. Once the wait is queued every later
                 * exit wakes it; one probe now covers the earlier ones.
                 */
                if (observeIfExited()) {
                    stopExitWatch();
                }
                return;
            }
            m_pidfd.reset(); // Fall back to polling; `pidfd` closes.
        }
        m_timer.emplace(reactor().context);
        m_pollPeriod = kFirstPoll;
        armTimer();
    }

    void stopExitWatch() noexcept override {
        m_watchStopped = true;
        std::error_code ignored;
        if (m_pidfd && m_pidfd->is_open()) {
            static_cast<void>(m_pidfd->close(ignored));
        }
        if (m_timer) {
            m_timer->cancel();
        }
    }

    void releaseChild() noexcept override {
        m_released = true;
        if (m_ownGroup) {
            // The zombie (or live) leader keeps the group id reserved, so this reaches only our group.
            static_cast<void>(::kill(-m_pid, SIGKILL));
        }
        if (exitKnown()) {
            reapNow();
            stopExitWatch();
        }
        // Otherwise the exit watch keeps running and reaps on exit.
    }

private:
    void armPidfd() {
        m_pidfd->async_wait(asio::posix::stream_descriptor::wait_read, bindHandler([this](const std::error_code& ec) {
                                if (ec == asio::error::operation_aborted || m_watchStopped) {
                                    return;
                                }
                                if (ec) {
                                    exitObserved(std::unexpected(errnoError(ec.value(), "wait for the process")));
                                    return;
                                }
                                check();
                            }));
    }

    void armTimer() {
        m_timer->expires_after(m_pollPeriod);
        m_timer->async_wait(bindHandler([this](const std::error_code& ec) {
            if (ec == asio::error::operation_aborted || m_watchStopped) {
                return;
            }
            m_pollPeriod = std::min<std::chrono::milliseconds>(m_pollPeriod * 2, kLongestPoll);
            check();
        }));
    }

    // The watch fired: report the exit (and reap after release), or wait again.
    void check() {
        if (observeIfExited()) {
            stopExitWatch();
        } else if (m_pidfd) {
            armPidfd(); // Spurious readiness.
        } else {
            armTimer();
        }
    }

    // Reports the exit (or the probe's failure) and reaps after release; false while the child runs.
    [[nodiscard]] bool observeIfExited() {
        if (exitKnown()) {
            return true;
        }
        auto probed = probeExit(m_pid);
        if (!probed) {
            exitObserved(std::unexpected(std::move(probed.error())));
            return true;
        }
        if (!*probed) {
            return false;
        }
        exitObserved(**probed);
        if (m_released) {
            reapNow();
        }
        return true;
    }

    void reapNow() noexcept {
        if (!m_reaped) {
            m_reaped = reapIfExited(m_pid);
        }
    }

    const pid_t m_pid;
    const bool m_ownGroup;
    // Strand only (and the destructor, when no handler can run any more).
    std::optional<asio::posix::stream_descriptor> m_pidfd;
    std::optional<asio::steady_timer> m_timer;
    std::chrono::milliseconds m_pollPeriod = kFirstPoll;
    bool m_watchStopped = false;
    bool m_released = false;
    bool m_reaped = false;
};

// Registers the parent's pipe end with the reactor; an invalid descriptor gives no stream.
Result<std::optional<PipeStream>> associate(Reactor::Impl& reactor, UniqueFd& fd, std::string_view what) {
    if (!fd.valid()) {
        return std::optional<PipeStream>{};
    }
    PipeStream stream(reactor.context);
    std::error_code ec;
    static_cast<void>(stream.assign(fd.get(), ec));
    if (ec) {
        return std::unexpected(errnoError(ec.value(), std::format("register {} with the reactor", what)));
    }
    static_cast<void>(fd.release()); // The stream owns it now.
    return std::optional<PipeStream>(std::move(stream));
}

} // namespace

Result<std::unique_ptr<IAsyncProcess>> makeAsyncProcess(SpawnedChild child, const AsyncContext& context) {
    Reactor::Impl& reactor = context.reactor.impl();
    auto state = std::make_shared<PosixAsyncProcess>(reactor, context.scheduler, child);
    auto in = associate(reactor, child.in, "stdin");
    auto out =
        in ? associate(reactor, child.out, "stdout") : Result<std::optional<PipeStream>>(std::unexpected(in.error()));
    auto err =
        out ? associate(reactor, child.err, "stderr") : Result<std::optional<PipeStream>>(std::unexpected(out.error()));
    if (!err) {
        state->killAndReapNow(); // Never handed out: end it.
        return std::unexpected(std::move(err.error()));
    }
    // Adopted before the state is bound: until then no other thread (shutdown included) can see it.
    state->adoptPipes(std::move(*in), std::move(*out), std::move(*err));
    if (auto bound = reactor.bind(state); !bound) {
        state->killAndReapNow();
        return std::unexpected(std::move(bound.error()));
    }
    if (!state->post([state] { state->startOnStrand(); })) {
        state->killAndReapNow();
        return fail(ErrorCode::Cancelled, "launchAsync: the reactor is shut down");
    }
    return std::unique_ptr<IAsyncProcess>(std::make_unique<AsyncProcess>(std::move(state)));
}

} // namespace frost::io::detail
