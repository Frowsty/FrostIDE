// SPDX-License-Identifier: Apache-2.0
//
// ScriptedProcessLauncher, the scripted IProcessLauncher double (see
// frost/io/scripted_process_launcher.hpp).

#include "frost/io/scripted_process_launcher.hpp"

#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"

#include <algorithm>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <format>
#include <utility>

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
          m_neverExits(script.neverExits), m_racesTerminate(script.racesTerminate), m_outputHeld(script.outputHeld),
          m_contained(spec.killOnClose), m_image(fileName(spec.executable)), m_stdinPipe(spec.stdinMode == Stdio::Pipe),
          m_stdoutPipe(spec.stdoutMode == Stdio::Pipe), m_stderrPipe(spec.stderrMode == Stdio::Pipe),
          m_leftovers(script.leftovers) {
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
        UniqueLock lock(m_mutex);
        return read(lock, m_stdoutPipe, m_stdout, m_stdoutPos, buffer, "readStdout");
    }

    Result<std::size_t> readStderr(std::span<char> buffer) override {
        UniqueLock lock(m_mutex);
        return read(lock, m_stderrPipe, m_stderr, m_stderrPos, buffer, "readStderr");
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
        return status();
    }

    Result<void> terminate() override {
        {
            LockGuard lock(m_mutex);
            m_terminated = true;
            m_leftovers.clear();
        }
        m_changed.notify_all();
        LockGuard lock(m_state->mutex);
        m_state->records[m_index].terminated = true;
        return {};
    }

    Result<std::vector<ContainedProcess>> containedProcesses() override {
        if (!m_contained) {
            return fail(ErrorCode::Unsupported, std::format("scripted process {} is not contained", m_pid));
        }
        LockGuard lock(m_mutex);
        std::vector<ContainedProcess> processes;
        if (m_neverExits && !m_terminated && !m_childTerminated) {
            processes.push_back({m_pid, m_image});
        }
        processes.insert(processes.end(), m_leftovers.begin(), m_leftovers.end());
        return processes;
    }

    Result<void> terminateContained(ProcessId pid) override {
        if (!m_contained) {
            return fail(ErrorCode::Unsupported, std::format("scripted process {} is not contained", m_pid));
        }
        {
            LockGuard lock(m_mutex);
            if (pid == m_pid) {
                m_childTerminated = true;
            } else {
                const auto it =
                    std::ranges::find_if(m_leftovers, [pid](const ContainedProcess& p) { return p.pid == pid; });
                if (it == m_leftovers.end()) {
                    return fail(ErrorCode::InvalidArgument,
                                std::format("process {} is not in the container of scripted process {}", pid, m_pid));
                }
                m_leftovers.erase(it);
            }
        }
        LockGuard lock(m_state->mutex);
        m_state->records[m_index].terminatedContained.push_back(pid);
        return {};
    }

    Result<void> releaseContainer() override {
        if (!m_contained) {
            return fail(ErrorCode::Unsupported, std::format("scripted process {} is not contained", m_pid));
        }
        LockGuard lock(m_state->mutex);
        m_state->records[m_index].containerReleased = true;
        return {};
    }

    void abandonOutput() noexcept override {
        {
            LockGuard lock(m_mutex);
            m_abandoned = true;
        }
        m_changed.notify_all();
        LockGuard lock(m_state->mutex);
        m_state->records[m_index].outputAbandoned = true;
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
        const ExitStatus exit = status();
        ProcessResult result{exit.exitCode, exit.signal, m_stdout.substr(m_stdoutPos), m_stderr.substr(m_stderrPos)};
        m_stdoutPos = m_stdout.size();
        m_stderrPos = m_stderr.size();
        return result;
    }

private:
    // The status wait() reports once the child has exited (see the header comment).
    ExitStatus status() const FROST_REQUIRES(m_mutex) {
        const bool ended = m_terminated || m_childTerminated;
        return ended && m_neverExits && !m_racesTerminate ? kTerminatedStatus : m_status;
    }

    Result<std::size_t> read(UniqueLock& lock, bool isPipe, const std::string& data, std::size_t& position,
                             std::span<char> buffer, std::string_view operation) FROST_REQUIRES(m_mutex) {
        if (!isPipe) {
            return fail(ErrorCode::InvalidArgument, std::format("{}: the stream is not a pipe", operation));
        }
        if (m_abandoned) {
            return std::size_t{0};
        }
        if (m_outputHeld && position == data.size()) {
            // A descendant holds the pipe open: no EOF until it is ended or the output abandoned.
            while (!m_abandoned && !m_terminated) {
                m_changed.wait(lock.native());
            }
            return std::size_t{0};
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
    const bool m_racesTerminate;
    const bool m_outputHeld;
    const bool m_contained;
    const std::string m_image; // The executable's file name, for containedProcesses().
    const bool m_stdinPipe;
    const bool m_stdoutPipe;
    const bool m_stderrPipe;
    std::string m_stdout; // Immutable after construction.
    std::string m_stderr; // Immutable after construction.

    Mutex m_mutex;
    std::condition_variable m_changed; // terminate() or abandonOutput(), for held reads.
    std::size_t m_stdoutPos FROST_GUARDED_BY(m_mutex) = 0;
    std::size_t m_stderrPos FROST_GUARDED_BY(m_mutex) = 0;
    bool m_stdinOpen FROST_GUARDED_BY(m_mutex) = false;
    bool m_terminated FROST_GUARDED_BY(m_mutex) = false;
    bool m_childTerminated FROST_GUARDED_BY(m_mutex) = false; // terminateContained(pid()).
    bool m_abandoned FROST_GUARDED_BY(m_mutex) = false;
    std::vector<ContainedProcess> m_leftovers FROST_GUARDED_BY(m_mutex);
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

} // namespace frost::io
