// SPDX-License-Identifier: Apache-2.0
//
// GitRunner (frost/vcs/git_runner.hpp): the command line, the environment, the pipe threads, the
// time limit and cancellation, and the failure messages.

#include "frost/vcs/git_runner.hpp"

#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"

#include "environment.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <thread>

namespace frost::vcs {
namespace {

// How long one wait() blocks before the stop token and the deadline are looked at again.
constexpr auto kWaitSlice = std::chrono::milliseconds(50);
constexpr std::size_t kReadChunk = 64 * 1024;

// Splits a stream into lines for a GitLineSink; '\n' and '\r' both end a line.
class LineFeeder {
public:
    LineFeeder(GitStream stream, const GitLineSink& sink, Mutex& sinkMutex)
        : m_stream(stream), m_sink(sink), m_mutex(sinkMutex) {}

    void feed(std::string_view bytes) {
        if (!m_sink) {
            return;
        }
        for (const char c : bytes) {
            if (c == '\n') {
                // The LF of a CRLF: the CR already ended the line.
                if (!m_afterCr) {
                    emit();
                }
                m_afterCr = false;
            } else if (c == '\r') {
                // A progress update rewrites its line with CR; empty rewrites are not lines.
                if (!m_pending.empty()) {
                    emit();
                }
                m_afterCr = true;
            } else {
                m_pending.push_back(c);
                m_afterCr = false;
            }
        }
    }

    void finish() {
        if (m_sink && !m_pending.empty()) {
            emit();
        }
    }

private:
    void emit() {
        {
            LockGuard lock(m_mutex);
            m_sink(m_stream, m_pending);
        }
        m_pending.clear();
    }

    GitStream m_stream;
    const GitLineSink& m_sink;
    Mutex& m_mutex;
    std::string m_pending;
    bool m_afterCr = false;
};

// The default core.hooksPath of restricted mode: <temp>/frost-git-no-hooks (never created).
std::string defaultNoHooksDirectory() {
#ifdef _WIN32
    std::string temp = detail::environmentVariable("TEMP");
    if (temp.empty()) {
        temp = "C:\\Windows\\Temp";
    }
#else
    std::string temp = detail::environmentVariable("TMPDIR");
    if (temp.empty()) {
        temp = "/tmp";
    }
#endif
    auto joined = io::joinPath(temp, "frost-git-no-hooks");
    return joined ? std::move(*joined) : temp;
}

bool contains(std::string_view text, std::string_view needle) { return text.find(needle) != std::string_view::npos; }

// The last `count` non-empty lines of `text`, joined with '\n'.
std::string lastLines(std::string_view text, std::size_t count) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = trimGitText(text.substr(start, end - start));
        if (!line.empty()) {
            lines.push_back(line);
        }
        start = end + 1;
    }
    const std::size_t first = lines.size() > count ? lines.size() - count : 0;
    std::string out;
    for (std::size_t i = first; i < lines.size(); ++i) {
        if (!out.empty()) {
            out += '\n';
        }
        out += lines[i];
    }
    return out;
}

} // namespace

std::string_view trimGitText(std::string_view text) noexcept {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

GitRunner::GitRunner(io::IProcessLauncher& launcher, std::string gitPath, GitRunnerOptions options)
    : m_launcher(&launcher), m_gitPath(std::move(gitPath)), m_options(std::move(options)) {
    if (!m_options.trusted && m_options.noHooksDirectory.empty()) {
        m_options.noHooksDirectory = defaultNoHooksDirectory();
    }
}

io::ProcessSpec GitRunner::processSpec(const GitInvocation& invocation) const {
    io::ProcessSpec spec;
    spec.executable = m_gitPath;
    spec.arguments = {"--no-pager", "-c", "core.quotepath=false", "-c", "color.ui=false"};
    if (!m_options.trusted) {
        spec.arguments.insert(spec.arguments.end(),
                              {"-c", "core.fsmonitor=false", "-c", "core.hooksPath=" + m_options.noHooksDirectory});
    }
    spec.arguments.insert(spec.arguments.end(), invocation.arguments.begin(), invocation.arguments.end());
    spec.workingDirectory = invocation.workingDirectory;
    spec.environment = {
        {"LC_ALL", "C"},
        {"LANG", "C"},
        {"LANGUAGE", std::nullopt},
        {"GIT_TERMINAL_PROMPT", "0"},
        {"GIT_EDITOR", ":"},
        {"GIT_SEQUENCE_EDITOR", ":"},
        {"GIT_MERGE_AUTOEDIT", "no"},
    };
    if (invocation.background) {
        spec.environment.emplace_back("GIT_OPTIONAL_LOCKS", "0");
    }
    spec.environment.insert(spec.environment.end(), m_options.environment.begin(), m_options.environment.end());
    spec.environment.insert(spec.environment.end(), invocation.environment.begin(), invocation.environment.end());
    spec.stdinMode = invocation.stdinData.empty() ? io::Stdio::Null : io::Stdio::Pipe;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    return spec;
}

Result<GitOutput> GitRunner::run(const GitInvocation& invocation) const {
    auto launched = m_launcher->launch(processSpec(invocation));
    if (!launched) {
        return std::unexpected(std::move(launched.error()).withContext(std::format("starting {}", m_gitPath)));
    }
    std::unique_ptr<io::IProcess> process = std::move(*launched);
    GitOutput output;
    Mutex sinkMutex;

    const auto reader = [&](GitStream stream, std::string& into) {
        LineFeeder lines(stream, invocation.onLine, sinkMutex);
        std::array<char, kReadChunk> buffer{};
        while (true) {
            auto read = stream == GitStream::Stdout ? process->readStdout(buffer) : process->readStderr(buffer);
            if (!read || *read == 0) {
                break;
            }
            const std::string_view chunk(buffer.data(), *read);
            into.append(chunk);
            lines.feed(chunk);
        }
        lines.finish();
    };
    std::jthread stdoutReader([&] { reader(GitStream::Stdout, output.stdoutBytes); });
    std::jthread stderrReader([&] { reader(GitStream::Stderr, output.stderrBytes); });
    std::jthread stdinWriter;
    if (!invocation.stdinData.empty()) {
        stdinWriter = std::jthread([&] {
            static_cast<void>(process->writeStdin(invocation.stdinData)); // A child that exits early drops the rest.
            process->closeStdin();
        });
    }

    const auto deadline = std::chrono::steady_clock::now() + invocation.timeout;
    std::optional<ErrorCode> ended;
    io::ExitStatus status;
    while (true) {
        auto waited = process->wait(kWaitSlice);
        if (waited) {
            status = *waited;
            break;
        }
        if (waited.error().code() != ErrorCode::Timeout) {
            static_cast<void>(process->terminate());
            ended = ErrorCode::IoError;
            break;
        }
        if (invocation.stop.stop_requested()) {
            ended = ErrorCode::Cancelled;
        } else if (std::chrono::steady_clock::now() >= deadline) {
            ended = ErrorCode::Timeout;
        }
        if (ended) {
            static_cast<void>(process->terminate());
            static_cast<void>(process->wait(std::chrono::seconds(5)));
            break;
        }
    }
    // Whatever git left in its container (a helper that inherited the pipes) would hold the pipes
    // open; end it so the readers reach EOF.
    static_cast<void>(process->terminate());
    if (stdinWriter.joinable()) {
        stdinWriter.join();
    }
    stdoutReader.join();
    stderrReader.join();
    if (ended == ErrorCode::Cancelled) {
        return fail(ErrorCode::Cancelled, "git was cancelled");
    }
    if (ended == ErrorCode::Timeout) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(invocation.timeout).count();
        std::string command = invocation.arguments.empty() ? std::string() : invocation.arguments.front();
        return fail(ErrorCode::Timeout,
                    std::format("git {} did not finish within {} s and was stopped", command, seconds));
    }
    if (ended == ErrorCode::IoError) {
        return fail(ErrorCode::IoError, "waiting for git failed");
    }
    output.exitCode = status.exitCode;
    return output;
}

Result<std::string> GitRunner::version() const {
    GitInvocation invocation;
    invocation.arguments = {"--version"};
    auto output = run(invocation);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    if (!output->succeeded()) {
        return std::unexpected(gitFailure("git --version", *output));
    }
    return std::string(trimGitText(output->stdoutBytes));
}

Error gitFailure(std::string_view what, const GitOutput& output) {
    const std::string_view err = output.stderrBytes;
    std::string hint;
    ErrorCode code = ErrorCode::IoError;
    if (contains(err, "terminal prompts disabled") || contains(err, "could not read Username") ||
        contains(err, "could not read Password")) {
        hint = "git needs credentials and cannot prompt for them here: set up Git Credential Manager or another "
               "credential helper (git config credential.helper), or sign in once in a terminal.";
    } else if (contains(err, "Host key verification failed") || contains(err, "Permission denied (publickey")) {
        hint = "SSH could not authenticate without a prompt: load the key into ssh-agent (ssh-add) or accept the "
               "host key once in a terminal.";
    } else if (contains(err, "[rejected]") && (contains(err, "fetch first") || contains(err, "non-fast-forward"))) {
        hint = "Push rejected: the remote has commits this branch does not have. Update the project (pull) first, "
               "or force-push with lease.";
    } else if (contains(err, "stale info")) {
        hint = "Force-push refused: the remote branch moved since the last fetch (force-with-lease). Fetch and look "
               "at the new commits first.";
    } else if (contains(err, "There is no tracking information") || contains(err, "has no upstream branch")) {
        hint = "The current branch has no upstream branch: push it once to set one.";
    } else if (contains(err, "Could not resolve host") || contains(err, "unable to access") ||
               contains(err, "Could not read from remote repository")) {
        hint = "The remote could not be reached: check the network and the remote's URL.";
    } else if (contains(err, "index.lock") || contains(err, ".lock': File exists")) {
        hint = "Another git process holds the repository's lock; wait for it to finish (or remove a stale "
               "index.lock).";
        code = ErrorCode::Busy;
    } else if (contains(err, "unmerged files") || contains(err, "you need to resolve your current index first")) {
        hint = "The repository has unresolved merge conflicts: resolve them first.";
    }
    std::string details = lastLines(err.empty() ? std::string_view(output.stdoutBytes) : err, 6);
    std::string message = std::format("{} failed (exit code {})", what, output.exitCode);
    if (!hint.empty()) {
        message = std::format("{}: {}", message, hint);
    }
    if (!details.empty()) {
        message += "\n";
        message += details;
    }
    return Error(code, std::move(message));
}

} // namespace frost::vcs
