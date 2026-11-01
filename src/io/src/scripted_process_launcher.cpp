// SPDX-License-Identifier: Apache-2.0
//
// ScriptedProcessLauncher, the scripted IProcessLauncher double (see
// frost/io/scripted_process_launcher.hpp).

#include "frost/io/scripted_process_launcher.hpp"

#include "frost/core/mutex.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include <algorithm>
#include <coroutine>
#include <csignal>
#include <cstdint>
#include <deque>
#include <format>
#include <stop_token>
#include <utility>
#include <vector>

namespace frost::io {

struct ScriptedProcessLauncher::State {
    Mutex mutex;
    std::deque<ScriptedProcess> scripts FROST_GUARDED_BY(mutex);
    std::vector<LaunchRecord> records FROST_GUARDED_BY(mutex);
    // Arbitrary but recognisable fake pids.
    ProcessId nextPid FROST_GUARDED_BY(mutex) = 4000;
};

namespace {

// The status a terminated process reports on this platform (see process.hpp).
constexpr ExitStatus kTerminatedStatus{kTerminatedExitCode,
#ifdef _WIN32
                                       0
#else
                                       SIGKILL
#endif
};

class ScriptedChild final : public IProcess {
public:
    ScriptedChild(std::shared_ptr<ScriptedProcessLauncher::State> state, std::size_t index, ProcessId pid,
                  const ScriptedProcess& script, const ProcessSpec& spec)
        : m_state(std::move(state)), m_index(index), m_pid(pid), m_status{script.exitCode, script.signal},
          m_neverExits(script.neverExits), m_stdinPipe(spec.stdinMode == Stdio::Pipe),
          m_stdoutPipe(spec.stdoutMode == Stdio::Pipe), m_stderrPipe(spec.stderrMode == Stdio::Pipe) {
        if (m_stdoutPipe) {
            m_stdout = script.stdoutBytes;
            if (spec.stderrMode == Stdio::MergeIntoStdout) {
                m_stdout += script.stderrBytes;
            }
        }
        if (m_stderrPipe) {
            m_stderr = script.stderrBytes;
        }
        m_stdinOpen = m_stdinPipe;
    }

    ProcessId pid() const noexcept override { return m_pid; }

    Result<void> writeStdin(std::string_view bytes) override {
        {
            LockGuard lock(m_mutex);
            if (!m_stdinOpen) {
                return fail(ErrorCode::InvalidArgument, "stdin is not an open pipe");
            }
        }
        LockGuard lock(m_state->mutex);
        m_state->records[m_index].stdinBytes += bytes;
        return {};
    }

    void closeStdin() noexcept override {
        {
            LockGuard lock(m_mutex);
            if (!m_stdinOpen) {
                return;
            }
            m_stdinOpen = false;
        }
        LockGuard lock(m_state->mutex);
        m_state->records[m_index].stdinClosed = true;
    }

    Result<std::size_t> readStdout(std::span<char> buffer) override {
        LockGuard lock(m_mutex);
        return read(m_stdoutPipe, m_stdout, m_stdoutPos, buffer, "readStdout");
    }

    Result<std::size_t> readStderr(std::span<char> buffer) override {
        LockGuard lock(m_mutex);
        return read(m_stderrPipe, m_stderr, m_stderrPos, buffer, "readStderr");
    }

    Result<ExitStatus> wait(std::optional<IClock::Duration> timeout) override {
        LockGuard lock(m_mutex);
        if (m_neverExits && !m_terminated) {
            if (!timeout) {
                return fail(ErrorCode::Internal,
                            std::format("scripted process {} never exits; wait() without a timeout would hang", m_pid));
            }
            return fail(ErrorCode::Timeout, std::format("process {} is still running", m_pid));
        }
        return m_terminated && m_neverExits ? kTerminatedStatus : m_status;
    }

    Result<void> terminate() override {
        {
            LockGuard lock(m_mutex);
            m_terminated = true;
        }
        LockGuard lock(m_state->mutex);
        m_state->records[m_index].terminated = true;
        return {};
    }

    Result<ProcessResult> communicate(std::string_view stdinData, IClock::Duration timeout) override {
        bool stdinOpen = false;
        {
            LockGuard lock(m_mutex);
            stdinOpen = m_stdinOpen;
        }
        if (!stdinData.empty() && !stdinOpen) {
            return fail(ErrorCode::InvalidArgument, "communicate: stdin data given but stdin is not an open pipe");
        }
        if (stdinOpen) {
            if (auto written = writeStdin(stdinData); !written) {
                return std::unexpected(std::move(written.error()));
            }
            closeStdin();
        }
        {
            LockGuard lock(m_mutex);
            if (m_neverExits && !m_terminated) {
                m_terminated = true;
                LockGuard records(m_state->mutex);
                m_state->records[m_index].terminated = true;
                return fail(ErrorCode::Timeout,
                            std::format("process {} did not finish within {} ms and was terminated", m_pid,
                                        std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()));
            }
        }
        LockGuard lock(m_mutex);
        const ExitStatus status = m_terminated && m_neverExits ? kTerminatedStatus : m_status;
        ProcessResult result{status.exitCode, status.signal, m_stdout.substr(m_stdoutPos),
                             m_stderr.substr(m_stderrPos)};
        m_stdoutPos = m_stdout.size();
        m_stderrPos = m_stderr.size();
        return result;
    }

private:
    static Result<std::size_t> read(bool isPipe, const std::string& data, std::size_t& position, std::span<char> buffer,
                                    std::string_view operation) {
        if (!isPipe) {
            return fail(ErrorCode::InvalidArgument, std::format("{}: the stream is not a pipe", operation));
        }
        const std::size_t count = std::min(buffer.size(), data.size() - position);
        std::copy_n(data.data() + position, count, buffer.data());
        position += count;
        return count;
    }

    const std::shared_ptr<ScriptedProcessLauncher::State> m_state;
    const std::size_t m_index; // Into m_state->records.
    const ProcessId m_pid;
    const ExitStatus m_status;
    const bool m_neverExits;
    const bool m_stdinPipe;
    const bool m_stdoutPipe;
    const bool m_stderrPipe;
    std::string m_stdout; // Immutable after construction.
    std::string m_stderr; // Immutable after construction.

    Mutex m_mutex;
    std::size_t m_stdoutPos FROST_GUARDED_BY(m_mutex) = 0;
    std::size_t m_stderrPos FROST_GUARDED_BY(m_mutex) = 0;
    bool m_stdinOpen FROST_GUARDED_BY(m_mutex) = false;
    bool m_terminated FROST_GUARDED_BY(m_mutex) = false;
};

/*
 * The async double's shared state (launchAsync). Coroutines own a reference, so they keep working when
 * the process object is destroyed. Waiting for terminate() (neverExits) goes through `m_waiters`: the
 * waker takes the entries under the mutex and enqueues them after unlocking.
 */
class ScriptedAsyncState {
public:
    ScriptedAsyncState(std::shared_ptr<ScriptedProcessLauncher::State> state, std::size_t index, ProcessId pid,
                       const ScriptedProcess& script, const ProcessSpec& spec, sched::IScheduler& scheduler)
        : m_state(std::move(state)), m_index(index), m_pid(pid), m_scheduler(scheduler),
          m_status{script.exitCode, script.signal}, m_neverExits(script.neverExits), m_chunkBytes(script.chunkBytes),
          m_chunkInterval(script.chunkInterval), m_stdinPipe(spec.stdinMode == Stdio::Pipe),
          m_stdoutPipe(spec.stdoutMode == Stdio::Pipe), m_stderrPipe(spec.stderrMode == Stdio::Pipe) {
        if (m_stdoutPipe) {
            m_stdout = script.stdoutBytes;
            if (spec.stderrMode == Stdio::MergeIntoStdout) {
                m_stdout += script.stderrBytes;
            }
        }
        if (m_stderrPipe) {
            m_stderr = script.stderrBytes;
        }
        m_stdinOpen = m_stdinPipe;
    }

    [[nodiscard]] ProcessId pid() const noexcept { return m_pid; }

    // `co_await waitForTerminate(token)`: true once terminated, false if the token stopped first.
    class TerminateAwaiter;
    [[nodiscard]] TerminateAwaiter waitForTerminate(std::stop_token token);

    static sched::Task<Result<std::size_t>> read(std::shared_ptr<ScriptedAsyncState> self, bool errStream,
                                                 std::span<char> buffer, std::stop_token token);
    static sched::Task<Result<void>> write(std::shared_ptr<ScriptedAsyncState> self, std::string bytes,
                                           std::stop_token token);
    static sched::Task<Result<ExitStatus>> exited(std::shared_ptr<ScriptedAsyncState> self, std::stop_token token);
    static sched::Task<Result<ProcessResult>> communicate(std::shared_ptr<ScriptedAsyncState> self,
                                                          std::string stdinData, IClock::Duration timeout,
                                                          std::stop_token token);

    void closeStdin() noexcept {
        {
            LockGuard lock(m_mutex);
            if (!m_stdinOpen) {
                return;
            }
            m_stdinOpen = false;
        }
        LockGuard lock(m_state->mutex);
        m_state->records[m_index].stdinClosed = true;
    }

    void terminate() {
        std::vector<Waiter> woken;
        {
            LockGuard lock(m_mutex);
            m_terminated = true;
            woken = std::exchange(m_waiters, {});
        }
        {
            LockGuard lock(m_state->mutex);
            m_state->records[m_index].terminated = true;
        }
        for (const Waiter& waiter : woken) {
            m_scheduler.enqueue(waiter.handle, waiter.lane);
        }
    }

private:
    struct Waiter {
        std::coroutine_handle<> handle;
        sched::Lane lane;
        std::uint64_t id;
    };

    [[nodiscard]] std::size_t position(bool errStream) {
        LockGuard lock(m_mutex);
        return errStream ? m_stderrPos : m_stdoutPos;
    }

    const std::shared_ptr<ScriptedProcessLauncher::State> m_state;
    const std::size_t m_index; // Into m_state->records.
    const ProcessId m_pid;
    sched::IScheduler& m_scheduler;
    const ExitStatus m_status;
    const bool m_neverExits;
    const std::size_t m_chunkBytes;
    const IClock::Duration m_chunkInterval;
    const bool m_stdinPipe;
    const bool m_stdoutPipe;
    const bool m_stderrPipe;
    std::string m_stdout; // Immutable after construction.
    std::string m_stderr; // Immutable after construction.

    Mutex m_mutex;
    std::size_t m_stdoutPos FROST_GUARDED_BY(m_mutex) = 0;
    std::size_t m_stderrPos FROST_GUARDED_BY(m_mutex) = 0;
    bool m_stdinOpen FROST_GUARDED_BY(m_mutex) = false;
    bool m_terminated FROST_GUARDED_BY(m_mutex) = false;
    std::vector<Waiter> m_waiters FROST_GUARDED_BY(m_mutex);
    std::uint64_t m_nextWaiter FROST_GUARDED_BY(m_mutex) = 1;
};

class ScriptedAsyncState::TerminateAwaiter {
public:
    TerminateAwaiter(ScriptedAsyncState& state, std::stop_token token) : m_state(state), m_token(std::move(token)) {}
    TerminateAwaiter(const TerminateAwaiter&) = delete ("the stop callback refers to the awaiter");
    TerminateAwaiter& operator=(const TerminateAwaiter&) = delete ("the stop callback refers to the awaiter");

    [[nodiscard]] bool await_ready() {
        LockGuard lock(m_state.m_mutex);
        return m_state.m_terminated || m_token.stop_requested();
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        const sched::Lane lane = m_state.m_scheduler.currentLane().value_or(sched::Lane::UserVisible);
        {
            LockGuard lock(m_state.m_mutex);
            m_id = m_state.m_nextWaiter++;
        }
        // Registered before the entry: a stop that comes first finds no entry, and the check below sees it.
        if (m_token.stop_possible()) {
            m_callback.emplace(m_token, StopWaiting{&m_state, m_id});
        }
        LockGuard lock(m_state.m_mutex);
        if (m_state.m_terminated || m_token.stop_requested()) {
            return false;
        }
        m_state.m_waiters.push_back(Waiter{handle, lane, m_id});
        return true;
    }

    // True once terminated (re-read: a stop and terminate() may both have happened).
    [[nodiscard]] bool await_resume() {
        LockGuard lock(m_state.m_mutex);
        return m_state.m_terminated;
    }

private:
    struct StopWaiting {
        ScriptedAsyncState* state;
        std::uint64_t id;
        void operator()() const noexcept {
            std::optional<Waiter> found;
            {
                LockGuard lock(state->m_mutex);
                const auto it = std::ranges::find(state->m_waiters, id, &Waiter::id);
                if (it != state->m_waiters.end()) {
                    found = *it;
                    state->m_waiters.erase(it);
                }
            }
            if (found) {
                state->m_scheduler.enqueue(found->handle, found->lane);
            }
        }
    };

    ScriptedAsyncState& m_state;
    std::stop_token m_token;
    std::uint64_t m_id = 0;
    std::optional<std::stop_callback<StopWaiting>> m_callback;
};

ScriptedAsyncState::TerminateAwaiter ScriptedAsyncState::waitForTerminate(std::stop_token token) {
    return TerminateAwaiter(*this, std::move(token));
}

sched::Task<Result<std::size_t>> ScriptedAsyncState::read(std::shared_ptr<ScriptedAsyncState> self, bool errStream,
                                                          std::span<char> buffer, std::stop_token token) {
    const std::string_view operation = errStream ? "readStderr" : "readStdout";
    if (!(errStream ? self->m_stderrPipe : self->m_stdoutPipe)) {
        co_return fail(ErrorCode::InvalidArgument, std::format("{}: the stream is not a pipe", operation));
    }
    if (buffer.empty()) {
        co_return fail(ErrorCode::InvalidArgument, std::format("{}: the buffer is empty", operation));
    }
    const std::string& data = errStream ? self->m_stderr : self->m_stdout;
    if (self->position(errStream) == data.size()) {
        // A hung child keeps its pipes open until it is terminated.
        if (self->m_neverExits && !co_await self->waitForTerminate(token)) {
            co_return fail(ErrorCode::Cancelled, std::format("{} was cancelled", operation));
        }
        co_return std::size_t{0};
    }
    if (self->m_chunkInterval > IClock::Duration::zero() &&
        !co_await self->m_scheduler.sleepFor(self->m_chunkInterval, token)) {
        co_return fail(ErrorCode::Cancelled, std::format("{} was cancelled", operation));
    }
    LockGuard lock(self->m_mutex);
    std::size_t& position = errStream ? self->m_stderrPos : self->m_stdoutPos;
    std::size_t count = std::min(buffer.size(), data.size() - position);
    if (self->m_chunkBytes > 0) {
        count = std::min(count, self->m_chunkBytes);
    }
    std::copy_n(data.data() + position, count, buffer.data());
    position += count;
    co_return count;
}

sched::Task<Result<void>> ScriptedAsyncState::write(std::shared_ptr<ScriptedAsyncState> self, std::string bytes,
                                                    std::stop_token token) {
    if (token.stop_requested()) {
        co_return fail(ErrorCode::Cancelled, "writeStdin was cancelled");
    }
    {
        LockGuard lock(self->m_mutex);
        if (!self->m_stdinOpen) {
            co_return fail(ErrorCode::InvalidArgument, "writeStdin: stdin is not an open pipe");
        }
    }
    LockGuard lock(self->m_state->mutex);
    self->m_state->records[self->m_index].stdinBytes += bytes;
    co_return Result<void>{};
}

sched::Task<Result<ExitStatus>> ScriptedAsyncState::exited(std::shared_ptr<ScriptedAsyncState> self,
                                                           std::stop_token token) {
    if (token.stop_requested()) {
        co_return fail(ErrorCode::Cancelled, "exited() was cancelled");
    }
    if (self->m_neverExits) {
        if (!co_await self->waitForTerminate(token)) {
            co_return fail(ErrorCode::Cancelled, "exited() was cancelled");
        }
        co_return kTerminatedStatus;
    }
    co_return self->m_status;
}

sched::Task<Result<ProcessResult>> ScriptedAsyncState::communicate(std::shared_ptr<ScriptedAsyncState> self,
                                                                   std::string stdinData, IClock::Duration timeout,
                                                                   std::stop_token token) {
    bool stdinOpen = false;
    {
        LockGuard lock(self->m_mutex);
        stdinOpen = self->m_stdinOpen;
    }
    if (!stdinData.empty() && !stdinOpen) {
        co_return fail(ErrorCode::InvalidArgument, "communicate: stdin data given but stdin is not an open pipe");
    }
    if (stdinOpen) {
        if (auto written = co_await write(self, std::move(stdinData), token); !written) {
            co_return std::unexpected(std::move(written.error()));
        }
        self->closeStdin();
    }
    if (self->m_neverExits) {
        // A hung child: the deadline passes on the scheduler's clock, then it is terminated.
        if (!co_await self->m_scheduler.sleepFor(timeout, token)) {
            co_return fail(ErrorCode::Cancelled, "communicate was cancelled");
        }
        self->terminate();
        co_return fail(ErrorCode::Timeout,
                       std::format("process {} did not finish within {} ms and was terminated", self->m_pid,
                                   std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()));
    }
    ProcessResult result{self->m_status.exitCode, self->m_status.signal, {}, {}};
    std::vector<char> chunk(64 * 1024);
    for (const bool errStream : {false, true}) {
        if (!(errStream ? self->m_stderrPipe : self->m_stdoutPipe)) {
            continue;
        }
        for (;;) {
            auto got = co_await read(self, errStream, chunk, token);
            if (!got) {
                co_return std::unexpected(std::move(got.error()));
            }
            if (*got == 0) {
                break;
            }
            (errStream ? result.stderrBytes : result.stdoutBytes).append(chunk.data(), *got);
        }
    }
    co_return result;
}

class ScriptedAsyncChild final : public IAsyncProcess {
public:
    explicit ScriptedAsyncChild(std::shared_ptr<ScriptedAsyncState> state) noexcept : m_state(std::move(state)) {}
    ProcessId pid() const noexcept override { return m_state->pid(); }
    sched::Task<Result<std::size_t>> readStdout(std::span<char> buffer, std::stop_token token) override {
        return ScriptedAsyncState::read(m_state, false, buffer, std::move(token));
    }
    sched::Task<Result<std::size_t>> readStderr(std::span<char> buffer, std::stop_token token) override {
        return ScriptedAsyncState::read(m_state, true, buffer, std::move(token));
    }
    sched::Task<Result<void>> writeStdin(std::string bytes, std::stop_token token) override {
        return ScriptedAsyncState::write(m_state, std::move(bytes), std::move(token));
    }
    void closeStdin() noexcept override { m_state->closeStdin(); }
    sched::Task<Result<ExitStatus>> exited(std::stop_token token) override {
        return ScriptedAsyncState::exited(m_state, std::move(token));
    }
    Result<void> terminate() override {
        m_state->terminate();
        return {};
    }
    sched::Task<Result<ProcessResult>> communicate(std::string stdinData, IClock::Duration timeout,
                                                   std::stop_token token) override {
        return ScriptedAsyncState::communicate(m_state, std::move(stdinData), timeout, std::move(token));
    }

private:
    std::shared_ptr<ScriptedAsyncState> m_state;
};

} // namespace

ScriptedProcessLauncher::ScriptedProcessLauncher() : m_state(std::make_shared<State>()) {}

ScriptedProcessLauncher::~ScriptedProcessLauncher() = default;

void ScriptedProcessLauncher::expect(ScriptedProcess script) {
    LockGuard lock(m_state->mutex);
    m_state->scripts.push_back(std::move(script));
}

std::size_t ScriptedProcessLauncher::pendingScripts() const {
    LockGuard lock(m_state->mutex);
    return m_state->scripts.size();
}

std::vector<LaunchRecord> ScriptedProcessLauncher::launches() const {
    LockGuard lock(m_state->mutex);
    return m_state->records;
}

Result<std::unique_ptr<IProcess>> ScriptedProcessLauncher::launch(const ProcessSpec& spec) {
    auto valid = validateProcessSpec(spec);
    LockGuard lock(m_state->mutex);
    const std::size_t index = m_state->records.size();
    LaunchRecord record;
    record.spec = spec;
    m_state->records.push_back(std::move(record));
    if (!valid) {
        return std::unexpected(std::move(valid.error()));
    }
    if (m_state->scripts.empty()) {
        return fail(ErrorCode::NotFound, std::format("no scripted process left for '{}'", spec.executable));
    }
    const ScriptedProcess& script = m_state->scripts.front();
    if (script.expectExecutable && *script.expectExecutable != spec.executable) {
        return fail(ErrorCode::InvalidArgument, std::format("unexpected launch of '{}'; the next script expects '{}'",
                                                            spec.executable, *script.expectExecutable));
    }
    if (script.launchError) {
        Error error = *script.launchError;
        m_state->scripts.pop_front();
        return std::unexpected(std::move(error));
    }
    const ProcessId pid = m_state->nextPid++;
    auto child = std::make_unique<ScriptedChild>(m_state, index, pid, script, spec);
    m_state->scripts.pop_front();
    m_state->records[index].launched = true;
    return child;
}

Result<std::unique_ptr<IAsyncProcess>> ScriptedProcessLauncher::launchAsync(const ProcessSpec& spec,
                                                                            const AsyncContext& context) {
    auto valid = validateProcessSpec(spec);
    LockGuard lock(m_state->mutex);
    const std::size_t index = m_state->records.size();
    LaunchRecord record;
    record.spec = spec;
    m_state->records.push_back(std::move(record));
    if (!valid) {
        return std::unexpected(std::move(valid.error()));
    }
    if (m_state->scripts.empty()) {
        return fail(ErrorCode::NotFound, std::format("no scripted process left for '{}'", spec.executable));
    }
    const ScriptedProcess& script = m_state->scripts.front();
    if (script.expectExecutable && *script.expectExecutable != spec.executable) {
        return fail(ErrorCode::InvalidArgument, std::format("unexpected launch of '{}'; the next script expects '{}'",
                                                            spec.executable, *script.expectExecutable));
    }
    if (script.launchError) {
        Error error = *script.launchError;
        m_state->scripts.pop_front();
        return std::unexpected(std::move(error));
    }
    const ProcessId pid = m_state->nextPid++;
    auto state = std::make_shared<ScriptedAsyncState>(m_state, index, pid, script, spec, context.scheduler);
    m_state->scripts.pop_front();
    m_state->records[index].launched = true;
    return std::unique_ptr<IAsyncProcess>(std::make_unique<ScriptedAsyncChild>(std::move(state)));
}

} // namespace frost::io
