// SPDX-License-Identifier: Apache-2.0
//
// BuildSession (see frost/build/build_session.hpp): the reader threads, the waiter thread, the
// clean-up of the build's container and the hand-over of the result.

#include "frost/build/build_session.hpp"
#include "frost/build/line_splitter.hpp"
#include "frost/core/mutex.hpp"

#include <chrono>
#include <condition_variable>
#include <format>
#include <thread>
#include <utility>

namespace frost::build {

namespace {

// How long one wait() for the process blocks before the waiter looks at the cancel flag again.
constexpr auto kWaitSlice = std::chrono::milliseconds(100);
// The pause after a wait that returned Timeout at once (the scripted double does), so it does not spin.
constexpr auto kIdlePause = std::chrono::milliseconds(20);
constexpr std::size_t kReadChunk = 64 * 1024;

// ASCII case-insensitive equality, for image file names.
bool sameImageName(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

struct BuildSession::State {
    IClock* clock = nullptr;
    BuildCallbacks callbacks;
    BuildSessionOptions options;
    std::unique_ptr<io::IProcess> process;
    io::ProcessId pid = 0;
    IClock::SteadyTime started{};

    // Written by the starting thread before the waiter starts; joined by the waiter.
    std::thread stdoutReader;
    std::thread stderrReader;
    // Joined by the destructor.
    std::thread waiter;

    // Serializes onLine and onDiagnostic and guards the diagnostics.
    Mutex outputMutex;
    DiagnosticSet diagnostics FROST_GUARDED_BY(outputMutex);

    mutable Mutex stateMutex;
    mutable std::condition_variable stateChanged;
    bool cancelRequested FROST_GUARDED_BY(stateMutex) = false;
    bool exited FROST_GUARDED_BY(stateMutex) = false;
    bool done FROST_GUARDED_BY(stateMutex) = false;
    int readersDone FROST_GUARDED_BY(stateMutex) = 0; // Readers that saw the end of their stream.
    // Written once by the waiter before it sets `done` under stateMutex, and read only after a
    // reader saw `done` under it, so the mutex orders every access; it never changes afterwards.
    BuildResult result;

    // Adds a parsed diagnostic unless it is a duplicate. Called with outputMutex held.
    void addDiagnostic(Diagnostic diagnostic) FROST_REQUIRES(outputMutex) {
        if (diagnostics.add(std::move(diagnostic)) && callbacks.onDiagnostic) {
            callbacks.onDiagnostic(diagnostics.diagnostics().back());
        }
    }

    void handleLine(OutputStream stream, std::string_view line, IOutputParser& parser) {
        LockGuard lock(outputMutex);
        if (callbacks.onLine) {
            callbacks.onLine(stream, line);
        }
        parser.feed(line, [this](Diagnostic diagnostic) FROST_NO_THREAD_SAFETY_ANALYSIS {
            addDiagnostic(std::move(diagnostic)); // feed() runs under outputMutex (above).
        });
    }

    void readStream(OutputStream stream) {
        LineSplitter splitter;
        const std::unique_ptr<IOutputParser> parser = options.parser ? options.parser() : makeMsBuildOutputParser();
        const LineSplitter::Sink sink = [this, stream, &parser](std::string_view line) {
            handleLine(stream, line, *parser);
        };
        std::vector<char> buffer(kReadChunk);
        while (true) {
            auto read = stream == OutputStream::Stdout ? process->readStdout(buffer) : process->readStderr(buffer);
            if (!read || *read == 0) {
                break; // EOF, or the pipe broke because the process tree was ended.
            }
            splitter.feed(std::string_view(buffer.data(), *read), sink);
        }
        splitter.finish(sink);
        {
            LockGuard lock(outputMutex);
            parser->finish([this](Diagnostic diagnostic) FROST_NO_THREAD_SAFETY_ANALYSIS {
                addDiagnostic(std::move(diagnostic)); // Under outputMutex (above).
            });
        }
        {
            LockGuard lock(stateMutex);
            ++readersDone;
        }
        stateChanged.notify_all();
    }

    bool isInfrastructure(std::string_view image) const {
        for (const std::string& name : options.infrastructureImages) {
            if (sameImageName(image, name)) {
                return true;
            }
        }
        return false;
    }

    /*
     * After the tool exited on its own, with infrastructureImages set: ends the listed images,
     * releases the container when anything else survives, and gives the readers outputGrace to
     * reach the end of their streams before their output is abandoned.
     */
    void endInfrastructure() {
        auto contained = process->containedProcesses();
        if (!contained) {
            static_cast<void>(process->terminate()); // Cannot tell them apart: end them all.
        } else {
            bool survivors = false;
            for (const io::ContainedProcess& entry : *contained) {
                if (entry.pid == pid) {
                    continue; // The tool itself, still being torn down.
                }
                if (isInfrastructure(entry.imageName)) {
                    static_cast<void>(process->terminateContained(entry.pid));
                } else {
                    survivors = true;
                }
            }
            if (survivors) {
                // On failure they end with the session, as by default.
                static_cast<void>(process->releaseContainer());
            }
        }
        bool abandon = false;
        {
            const auto deadline = std::chrono::steady_clock::now() + options.outputGrace;
            UniqueLock lock(stateMutex);
            while (readersDone < 2) {
                if (stateChanged.wait_until(lock.native(), deadline) == std::cv_status::timeout) {
                    break;
                }
            }
            abandon = readersDone < 2;
        }
        if (abandon) {
            process->abandonOutput();
        }
    }

    // Sleeps for `pause`, or less if cancel() is called meanwhile.
    void idle(std::chrono::milliseconds pause) {
        UniqueLock lock(stateMutex);
        if (!cancelRequested) {
            stateChanged.wait_for(lock.native(), pause);
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
            // A real wait blocked for its slice (cancel() ends the process, so it returns soon
            // after one); a wait that returned at once gets a short pause instead of a spin.
            if (clock->steadyNow() - before < kWaitSlice / 2) {
                idle(kIdlePause);
            }
        }
        const auto end = clock->steadyNow();
        bool cancelled = false;
        {
            LockGuard lock(stateMutex);
            cancelled = cancelRequested;
            exited = true;
        }
        // A cancel that reached a process which had already finished on its own did not end it
        // (terminate() leaves an exited process's code alone): report the build as it finished.
        if (cancelled && !waitFailed && status.exitCode != io::kTerminatedExitCode) {
            cancelled = false;
        }
        if (cancelled || waitFailed || options.infrastructureImages.empty()) {
            // Stragglers that inherited the pipes (a PDB server, a tool's child) keep them open;
            // end them so the readers see EOF once the buffered output is read.
            static_cast<void>(process->terminate());
        } else {
            endInfrastructure();
        }
        stdoutReader.join();
        stderrReader.join();

        BuildResult outcome;
        {
            LockGuard lock(outputMutex);
            outcome.diagnostics = diagnostics.diagnostics();
            outcome.errorCount = diagnostics.errorCount();
            outcome.warningCount = diagnostics.warningCount();
        }
        outcome.exitCode = status.exitCode;
        outcome.cancelled = cancelled;
        outcome.success = !cancelled && !waitFailed && status.exitCode == 0;
        outcome.duration = end - started;
        if (callbacks.onFinished) {
            callbacks.onFinished(outcome);
        }
        result = std::move(outcome);
        {
            LockGuard lock(stateMutex);
            done = true;
        }
        stateChanged.notify_all();
    }
};

Result<std::unique_ptr<BuildSession>> BuildSession::start(io::IProcessLauncher& launcher, const io::ProcessSpec& spec,
                                                          bool trusted, BuildCallbacks callbacks, IClock& clock,
                                                          BuildSessionOptions options) {
    if (!trusted) {
        return fail(ErrorCode::Untrusted,
                    std::format("'{}' was not started: the folder it builds is not trusted", spec.executable));
    }
    if (spec.stdoutMode != io::Stdio::Pipe || spec.stderrMode != io::Stdio::Pipe) {
        return fail(ErrorCode::InvalidArgument, "a build session reads stdout and stderr through pipes");
    }
    if (!spec.killOnClose) {
        return fail(ErrorCode::InvalidArgument, "a build session needs killOnClose to end the build's process tree");
    }
    auto state = std::make_shared<State>();
    state->clock = &clock;
    state->callbacks = std::move(callbacks);
    state->options = std::move(options);
    state->started = clock.steadyNow();
    // A parallel build saturates every CPU: below normal priority, it and the compilers it starts
    // leave the CPU to FrostIDE's UI thread (j2_run's full FrostEditor build stalled painting for
    // up to 3 s at normal priority, with ninja running 26 cl.exe on 24 hardware threads).
    io::ProcessSpec started = spec;
    started.belowNormalPriority = true;
    auto process = launcher.launch(started);
    if (!process) {
        return std::unexpected(std::move(process.error()));
    }
    state->process = std::move(*process);
    state->pid = state->process->pid();
    State* raw = state.get();
    state->stdoutReader = std::thread([raw] { raw->readStream(OutputStream::Stdout); });
    state->stderrReader = std::thread([raw] { raw->readStream(OutputStream::Stderr); });
    state->waiter = std::thread([raw] { raw->waitForExit(); });
    return std::unique_ptr<BuildSession>(new BuildSession(std::move(state)));
}

BuildSession::BuildSession(std::shared_ptr<State> state) noexcept : m_state(std::move(state)) {}

BuildSession::~BuildSession() {
    cancel();
    if (m_state->waiter.joinable()) {
        m_state->waiter.join();
    }
}

void BuildSession::cancel() noexcept {
    {
        LockGuard lock(m_state->stateMutex);
        if (m_state->cancelRequested || m_state->exited) {
            return;
        }
        m_state->cancelRequested = true;
    }
    m_state->stateChanged.notify_all();
    static_cast<void>(m_state->process->terminate());
}

bool BuildSession::finished() const {
    LockGuard lock(m_state->stateMutex);
    return m_state->done;
}

const BuildResult& BuildSession::wait() const {
    UniqueLock lock(m_state->stateMutex);
    while (!m_state->done) {
        m_state->stateChanged.wait(lock.native());
    }
    // Published results never change again, so the reference stays valid without the lock.
    return m_state->result;
}

std::optional<BuildResult> BuildSession::waitFor(IClock::Duration timeout) const {
    UniqueLock lock(m_state->stateMutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!m_state->done) {
        if (m_state->stateChanged.wait_until(lock.native(), deadline) == std::cv_status::timeout) {
            if (!m_state->done) {
                return std::nullopt;
            }
        }
    }
    return m_state->result;
}

io::ProcessId BuildSession::processId() const noexcept { return m_state->pid; }

} // namespace frost::build
