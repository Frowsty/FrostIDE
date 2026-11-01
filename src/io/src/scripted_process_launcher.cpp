// SPDX-License-Identifier: Apache-2.0
//
// ScriptedProcessLauncher, the scripted IProcessLauncher double (see
// frost/io/scripted_process_launcher.hpp).

#include "frost/io/scripted_process_launcher.hpp"

#include "frost/core/mutex.hpp"

#include <algorithm>
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
