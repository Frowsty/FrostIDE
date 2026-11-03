// SPDX-License-Identifier: Apache-2.0
//
// RunSession (see frost/build/run_session.hpp): the reader, writer and waiter threads, what happens
// to processes the program left behind, and the UTF-8 chunk decoder.

#include "frost/build/run_session.hpp"

#include "frost/build/line_splitter.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/utf.hpp"

#include <condition_variable>
#include <deque>
#include <thread>
#include <utility>
#include <vector>

namespace frost::build {

namespace {

// How long one wait() for the process blocks before the waiter looks at the stop flag again.
constexpr auto kWaitSlice = std::chrono::milliseconds(100);
// The pause after a wait that returned Timeout at once (the scripted double does), so it does not spin.
constexpr auto kIdlePause = std::chrono::milliseconds(20);
constexpr std::size_t kReadChunk = 16 * 1024;

} // namespace

std::string Utf8ChunkDecoder::feed(std::string_view bytes) {
    m_pending.append(bytes);
    std::size_t cut = m_pending.size();
    // Look back over at most three bytes for the lead byte of an unfinished sequence.
    for (std::size_t back = 1; back <= 3 && back <= m_pending.size(); ++back) {
        const auto byte = static_cast<unsigned char>(m_pending[m_pending.size() - back]);
        if ((byte & 0xC0U) == 0x80U) {
            continue; // A continuation byte: keep looking for its lead.
        }
        if (byte >= 0xC0U && byte < 0xF8U) {
            const std::size_t length = byte >= 0xF0U ? 4 : byte >= 0xE0U ? 3 : 2;
            if (length > back) {
                cut = m_pending.size() - back;
            }
        }
        break;
    }
    const std::string_view part = std::string_view(m_pending).substr(0, cut);
    m_invalid = m_invalid || !io::isValidUtf8(part);
    std::string complete = sanitizeUtf8(part);
    m_pending.erase(0, cut);
    return complete;
}

std::string Utf8ChunkDecoder::finish() {
    m_invalid = m_invalid || !io::isValidUtf8(m_pending);
    std::string rest = sanitizeUtf8(m_pending);
    m_pending.clear();
    return rest;
}

struct RunSession::State {
    IClock* clock = nullptr;
    RunSessionCallbacks callbacks;
    RunSessionOptions options;
    std::unique_ptr<io::IProcess> process;
    io::ProcessId pid = 0;
    IClock::SteadyTime started{};
    bool stdinPipe = false;
    int readersExpected = 0;

    // Written by the starting thread before the waiter starts; joined by the waiter.
    std::thread stdoutReader;
    std::thread stderrReader;
    std::thread writer;
    // Joined by the destructor.
    std::thread waiter;

    // Serializes the output callbacks.
    Mutex outputMutex;

    mutable Mutex stateMutex;
    mutable std::condition_variable stateChanged;
    bool stopRequested FROST_GUARDED_BY(stateMutex) = false;
    bool exited FROST_GUARDED_BY(stateMutex) = false;
    bool done FROST_GUARDED_BY(stateMutex) = false;
    int readersDone FROST_GUARDED_BY(stateMutex) = 0;
    std::deque<std::string> input FROST_GUARDED_BY(stateMutex);
    bool inputClosed FROST_GUARDED_BY(stateMutex) = false; // closeInput() was called.
    bool stdinBroken FROST_GUARDED_BY(stateMutex) = false; // A write failed (the program closed stdin).
    bool writerQuit FROST_GUARDED_BY(stateMutex) = false;
    bool decodingFailed FROST_GUARDED_BY(stateMutex) = false;
    // Written once by the waiter before it sets `done` under stateMutex; never changes afterwards.
    RunExit result;

    void emit(RunStream stream, const std::string& text) {
        if (text.empty()) {
            return;
        }
        LockGuard lock(outputMutex);
        if (callbacks.onOutput) {
            callbacks.onOutput(stream, text);
        }
    }

    void readStream(RunStream stream) {
        Utf8ChunkDecoder decoder;
        std::vector<char> buffer(kReadChunk);
        while (true) {
            auto read = stream == RunStream::Stdout ? process->readStdout(buffer) : process->readStderr(buffer);
            if (!read || *read == 0) {
                break; // EOF, or the pipe broke because the tree was ended.
            }
            auto decoded = decoder.feed(std::string_view(buffer.data(), *read));
            if (options.rejectInvalidUtf8 && decoder.invalid()) {
                {
                    LockGuard lock(stateMutex);
                    decodingFailed = true;
                }
                static_cast<void>(process->terminate());
                break;
            }
            emit(stream, decoded);
        }
        auto tail = decoder.finish();
        if (options.rejectInvalidUtf8 && decoder.invalid()) {
            {
                LockGuard lock(stateMutex);
                decodingFailed = true;
            }
            static_cast<void>(process->terminate());
        } else {
            emit(stream, tail);
        }
        {
            LockGuard lock(stateMutex);
            ++readersDone;
        }
        stateChanged.notify_all();
    }

    void writeInput() {
        while (true) {
            std::string bytes;
            bool closeNow = false;
            {
                UniqueLock lock(stateMutex);
                while (input.empty() && !inputClosed && !writerQuit) {
                    stateChanged.wait(lock.native());
                }
                if (writerQuit) {
                    return; // The program is gone; queued input is dropped.
                }
                if (input.empty()) {
                    closeNow = true; // inputClosed and everything queued is written.
                } else {
                    bytes = std::move(input.front());
                    input.pop_front();
                }
            }
            if (closeNow) {
                process->closeStdin();
                return;
            }
            if (auto written = process->writeStdin(bytes); !written) {
                {
                    LockGuard lock(stateMutex);
                    stdinBroken = true;
                    input.clear();
                }
                process->closeStdin();
                return;
            }
        }
    }

    // Sleeps for `pause`, or less if stop() is called meanwhile.
    void idle(std::chrono::milliseconds pause) {
        UniqueLock lock(stateMutex);
        if (!stopRequested) {
            stateChanged.wait_for(lock.native(), pause);
        }
    }

    // After a natural exit: lets what the program started live on, then bounds the wait for output.
    void releaseLeftovers() {
        if (auto contained = process->containedProcesses()) {
            for (const io::ContainedProcess& entry : *contained) {
                if (entry.pid != pid) {
                    // On failure they end with the session instead.
                    static_cast<void>(process->releaseContainer());
                    break;
                }
            }
        }
        bool abandon = false;
        {
            const auto deadline = std::chrono::steady_clock::now() + options.outputGrace;
            UniqueLock lock(stateMutex);
            while (readersDone < readersExpected) {
                if (stateChanged.wait_until(lock.native(), deadline) == std::cv_status::timeout) {
                    break;
                }
            }
            abandon = readersDone < readersExpected;
        }
        if (abandon) {
            process->abandonOutput();
        }
    }

    void waitForExit() {
        io::ExitStatus status;
        bool waitFailed = false;
        while (true) {
            const auto before = clock->steadyNow();
            auto waited = process->wait(kWaitSlice);
            if (waited) {
                status = *waited;
                break;
            }
            if (waited.error().code() != ErrorCode::Timeout) {
                waitFailed = true;
                status.exitCode = -1;
                break;
            }
            if (clock->steadyNow() - before < kWaitSlice / 2) {
                idle(kIdlePause);
            }
        }
        const auto end = clock->steadyNow();
        bool stopped = false;
        {
            LockGuard lock(stateMutex);
            stopped = stopRequested;
            exited = true;
            writerQuit = true;
        }
        stateChanged.notify_all();
        // A stop that reached a program which had already exited did not end it.
        if (stopped && !waitFailed && status.exitCode != io::kTerminatedExitCode) {
            stopped = false;
        }
        bool cleanupFailed = false;
        if (stopped || waitFailed || options.terminateDescendantsOnExit) {
            if (auto ended = process->terminate(); !ended) {
                cleanupFailed = true;
                process->abandonOutput();
            }
        } else {
            releaseLeftovers();
        }
        if (stdoutReader.joinable()) {
            stdoutReader.join();
        }
        if (stderrReader.joinable()) {
            stderrReader.join();
        }
        if (writer.joinable()) {
            writer.join();
        }
        bool badEncoding = false;
        {
            LockGuard lock(stateMutex);
            badEncoding = decodingFailed;
        }
        RunExit outcome{.exitCode = status.exitCode,
                        .stopped = stopped,
                        .waitFailed = waitFailed,
                        .cleanupFailed = cleanupFailed,
                        .decodingFailed = badEncoding,
                        .duration = end - started};
        if (callbacks.onExited) {
            callbacks.onExited(outcome);
        }
        result = outcome;
        {
            LockGuard lock(stateMutex);
            done = true;
        }
        stateChanged.notify_all();
    }
};

Result<std::unique_ptr<RunSession>> RunSession::start(io::IProcessLauncher& launcher, const io::ProcessSpec& spec,
                                                      RunSessionCallbacks callbacks, IClock& clock,
                                                      RunSessionOptions options) {
    if (!spec.killOnClose) {
        return fail(ErrorCode::InvalidArgument, "a run session needs killOnClose so Stop can end the process tree");
    }
    auto state = std::make_shared<State>();
    state->clock = &clock;
    state->callbacks = std::move(callbacks);
    state->options = options;
    state->started = clock.steadyNow();
    auto process = launcher.launch(spec);
    if (!process) {
        return std::unexpected(std::move(process.error()));
    }
    state->process = std::move(*process);
    state->pid = state->process->pid();
    state->stdinPipe = spec.stdinMode == io::Stdio::Pipe;
    State* raw = state.get();
    if (spec.stdoutMode == io::Stdio::Pipe) {
        ++state->readersExpected;
        state->stdoutReader = std::thread([raw] { raw->readStream(RunStream::Stdout); });
    }
    if (spec.stderrMode == io::Stdio::Pipe) {
        ++state->readersExpected;
        state->stderrReader = std::thread([raw] { raw->readStream(RunStream::Stderr); });
    }
    if (state->stdinPipe) {
        state->writer = std::thread([raw] { raw->writeInput(); });
    }
    state->waiter = std::thread([raw] { raw->waitForExit(); });
    return std::unique_ptr<RunSession>(new RunSession(std::move(state)));
}

RunSession::RunSession(std::shared_ptr<State> state) noexcept : m_state(std::move(state)) {}

RunSession::~RunSession() {
    stop();
    if (m_state->waiter.joinable()) {
        m_state->waiter.join();
    }
}

void RunSession::stop() noexcept {
    {
        LockGuard lock(m_state->stateMutex);
        if (m_state->stopRequested || m_state->exited) {
            return;
        }
        m_state->stopRequested = true;
    }
    m_state->stateChanged.notify_all();
    static_cast<void>(m_state->process->terminate());
}

Result<void> RunSession::writeInput(std::string bytes) {
    {
        LockGuard lock(m_state->stateMutex);
        if (!m_state->stdinPipe) {
            return fail(ErrorCode::InvalidArgument, "the program's input is not connected to the Run console");
        }
        if (m_state->inputClosed || m_state->stdinBroken) {
            return fail(ErrorCode::InvalidArgument, "the program's input is closed");
        }
        if (m_state->exited) {
            return fail(ErrorCode::NotFound, "the program has exited");
        }
        m_state->input.push_back(std::move(bytes));
    }
    m_state->stateChanged.notify_all();
    return {};
}

void RunSession::closeInput() noexcept {
    {
        LockGuard lock(m_state->stateMutex);
        if (!m_state->stdinPipe || m_state->inputClosed) {
            return;
        }
        m_state->inputClosed = true;
    }
    m_state->stateChanged.notify_all();
}

bool RunSession::acceptsInput() const {
    LockGuard lock(m_state->stateMutex);
    return m_state->stdinPipe && !m_state->inputClosed && !m_state->stdinBroken && !m_state->exited;
}

bool RunSession::finished() const {
    LockGuard lock(m_state->stateMutex);
    return m_state->done;
}

std::optional<RunExit> RunSession::waitFor(IClock::Duration timeout) const {
    UniqueLock lock(m_state->stateMutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!m_state->done) {
        if (m_state->stateChanged.wait_until(lock.native(), deadline) == std::cv_status::timeout && !m_state->done) {
            return std::nullopt;
        }
    }
    return m_state->result;
}

io::ProcessId RunSession::processId() const noexcept { return m_state->pid; }

} // namespace frost::build
