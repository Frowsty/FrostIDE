// SPDX-License-Identifier: Apache-2.0
//
// LldbDapE2E: the pinned lldb-dap (built by tools/build-lldb.ps1 and tools/build-lldb.sh, pinned in
// sdk/sdk.lock.json; ADR 0049) driven over stdio through frost::io's process launcher, as U1's DAP
// client will drive it. One test stops at a line breakpoint in main and reads the top frame; the
// other stops in the second source file, checks the threads, continues to the exit and reads the
// exit code. Both end with disconnect. Label "integration" with the tag "lldb"; skipped with a
// message when the pinned lldb-dap is not installed. Each test takes a second or two.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/jsonrpc.hpp"

#include "support/crash_dialogs.hpp"
#include "support/private_desktop.hpp"
#include "support/scratch_dir.hpp"
#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::lang::Json;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;
// lldb-dap answers in milliseconds; the first launch loads the debuggee's symbols in well under a
// second. This is slack for a loaded machine.
constexpr auto kPatience = 60s;

// <sdk>/lldb-<version>/bin/lldb-dap[.exe], where <sdk> is $FROST_SDK (tools/env.ps1, ~/sdk/env.sh)
// or the default SDK root, and <version> is the lock file's pin.
std::string pinnedLldbDap() {
    std::string sdk = frost::test::environment("FROST_SDK");
    if (sdk.empty()) {
        sdk = FROST_SDK_DEFAULT_ROOT;
    }
    const std::string relative =
        std::string("lldb-") + FROST_LLDB_VERSION + (kWindows ? "\\bin\\lldb-dap.exe" : "/bin/lldb-dap");
    auto joined = frost::io::joinPath(sdk, relative);
    auto normalized = joined ? frost::io::normalizePath(*joined) : joined;
    return normalized.value_or(std::string());
}

std::string native(std::string_view path) { return frost::io::normalizePath(path).value_or(std::string(path)); }

// The 1-based line of `file` that contains `marker` ("BP:main"), or 0.
int lineOf(const std::string& file, std::string_view marker) {
    std::ifstream in(file);
    std::string line;
    for (int number = 1; std::getline(in, line); ++number) {
        if (line.find(marker) != std::string::npos) {
            return number;
        }
    }
    return 0;
}

std::string fixtureSource(std::string_view name) {
    return native(std::string(FROST_LLDB_DEBUGGEE_SOURCE_DIR) + "/" + std::string(name));
}

/*
 * One lldb-dap process and its DAP conversation. A reader thread splits stdout into messages
 * (Content-Length framing, frost::lang's FrameDecoder) and queues them; another drains stderr.
 * The destructor ends the whole process tree (lldb-dap, lldb-server on Linux and the debuggee)
 * if it is still running and joins both threads.
 */
class DapSession {
public:
    explicit DapSession(std::unique_ptr<frost::io::IProcess> process) : m_process(std::move(process)) {
        m_stdout = std::thread([this] { readMessages(); });
        m_stderr = std::thread([this] {
            std::array<char, 4096> buffer{};
            while (true) {
                auto read = m_process->readStderr(buffer);
                if (!read || *read == 0) {
                    return;
                }
                const std::scoped_lock lock(m_mutex);
                m_stderrText.append(buffer.data(), *read);
            }
        });
    }

    ~DapSession() {
        if (!m_process->wait(0ms)) {
            (void)m_process->terminate();
            (void)m_process->wait(10s);
        }
        m_process->abandonOutput(); // A surviving descendant may still hold the pipes.
        m_stdout.join();
        m_stderr.join();
    }

    DapSession(const DapSession&) = delete ("owns the process and its reader threads");
    DapSession& operator=(const DapSession&) = delete ("owns the process and its reader threads");

    // Sends a request and returns its sequence number (0 if the write failed).
    std::int64_t send(std::string_view command, Json arguments = Json::object()) {
        const std::int64_t seq = ++m_seq;
        Json request = Json::object();
        request.set("seq", seq);
        request.set("type", "request");
        request.set("command", std::string(command));
        request.set("arguments", std::move(arguments));
        auto written = m_process->writeStdin(frost::lang::encodeFrame(request.dump()));
        EXPECT_TRUE(written.has_value()) << written.error().describe();
        return written ? seq : 0;
    }

    // The response to request `seq`, or nullopt after kPatience.
    std::optional<Json> response(std::int64_t seq) {
        return take([seq](const Json& message) {
            return message.stringAt("type") == "response" && message.integerAt("request_seq") == seq;
        });
    }

    // Sends a request and returns its response's body if it succeeded (a test failure otherwise).
    std::optional<Json> request(std::string_view command, Json arguments = Json::object()) {
        auto answer = response(send(command, std::move(arguments)));
        if (!answer) {
            ADD_FAILURE() << "no response to " << command << "\nlldb-dap stderr:\n" << stderrText();
            return std::nullopt;
        }
        if (answer->boolAt("success") != true) {
            ADD_FAILURE() << command << " failed: " << answer->dump();
            return std::nullopt;
        }
        const Json* body = answer->find("body");
        return body != nullptr ? *body : Json::object();
    }

    // The next event named `name` that no earlier call returned, or nullopt after kPatience.
    std::optional<Json> event(std::string_view name) {
        auto found = take([name](const Json& message) {
            return message.stringAt("type") == "event" && message.stringAt("event") == name;
        });
        if (!found) {
            ADD_FAILURE() << "no '" << name << "' event\nlldb-dap stderr:\n" << stderrText();
        }
        return found;
    }

    frost::io::IProcess& process() { return *m_process; }

    std::string stderrText() {
        const std::scoped_lock lock(m_mutex);
        return m_stderrText;
    }

private:
    void readMessages() {
        frost::lang::FrameDecoder decoder;
        std::array<char, 8192> buffer{};
        while (true) {
            auto read = m_process->readStdout(buffer);
            if (!read || *read == 0) {
                break;
            }
            decoder.feed(std::string_view(buffer.data(), *read));
            while (auto frame = decoder.next()) {
                const std::scoped_lock lock(m_mutex);
                if (!*frame) {
                    m_stderrText += "[test] bad DAP frame: " + frame->error().describe() + "\n";
                    continue;
                }
                auto parsed = frost::lang::parseJson(**frame);
                if (parsed) {
                    m_messages.push_back(std::move(*parsed));
                    m_taken.push_back(false);
                } else {
                    m_stderrText += "[test] unreadable DAP message: " + parsed.error().describe() + "\n";
                }
            }
            m_changed.notify_all();
        }
        const std::scoped_lock lock(m_mutex);
        m_eof = true;
        m_changed.notify_all();
    }

    // The first message not yet taken that `match` accepts, waiting up to kPatience.
    std::optional<Json> take(const std::function<bool(const Json&)>& match) {
        std::unique_lock lock(m_mutex);
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while (true) {
            for (std::size_t i = 0; i < m_messages.size(); ++i) {
                if (!m_taken[i] && match(m_messages[i])) {
                    m_taken[i] = true;
                    return m_messages[i];
                }
            }
            if (m_eof || m_changed.wait_until(lock, deadline) == std::cv_status::timeout) {
                return std::nullopt;
            }
        }
    }

    std::unique_ptr<frost::io::IProcess> m_process;
    std::int64_t m_seq = 0;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<Json> m_messages;
    std::vector<bool> m_taken;
    std::string m_stderrText;
    bool m_eof = false;
    std::thread m_stdout;
    std::thread m_stderr;
};

class LldbDapE2E : public ::testing::Test {
protected:
    void SetUp() override {
        m_lldbDap = pinnedLldbDap();
        if (m_lldbDap.empty() || !frost::io::realFileSystem().exists(m_lldbDap).value_or(false)) {
            GTEST_SKIP() << "the pinned lldb-dap is not installed at '" << m_lldbDap
                         << "'; build it with tools/build-lldb.ps1 (Windows) or tools/build-lldb.sh (WSL), or "
                            "run the bootstrap (ADR 0049)";
        }
        frost::test::disableCrashDialogs(); // lldb-dap inherits the error mode: no WER box if it dies.
    }

    // Starts lldb-dap at its absolute path with stdio pipes, without init files, symbol servers or
    // debuginfod (PLAN.md M4), on a private desktop on Windows.
    std::unique_ptr<DapSession> start() {
        frost::io::ProcessSpec spec;
        spec.executable = m_lldbDap;
        spec.arguments = {"--no-lldbinit"};
        spec.stdinMode = frost::io::Stdio::Pipe;
        spec.environment = {{"_NT_SYMBOL_PATH", std::nullopt},
                            {"_NT_ALT_SYMBOL_PATH", std::nullopt},
                            {"DEBUGINFOD_URLS", std::nullopt},
                            {"LLDB_LAUNCH_INFERIORS_WITHOUT_CONSOLE", "true"}};
#ifdef _WIN32
        if (!m_desktop.valid()) {
            return nullptr;
        }
        spec.desktop = m_desktop.name();
        const frost::test::NoHardErrorBoxes noBoxes;
#endif
        auto process = frost::io::realProcessLauncher().launch(spec);
        if (!process) {
            ADD_FAILURE() << process.error().describe();
            return nullptr;
        }
        return std::make_unique<DapSession>(std::move(*process));
    }

    // initialize, then launch the debuggee with `args` in a scratch working directory; returns
    // the launch request's sequence number (its response follows configurationDone).
    std::int64_t initializeAndLaunch(DapSession& dap, const std::vector<std::string>& args) {
        Json init = Json::object();
        init.set("clientID", "frost-test");
        init.set("adapterID", "lldb-dap");
        init.set("linesStartAt1", true);
        init.set("columnsStartAt1", true);
        init.set("pathFormat", "path");
        auto capabilities = dap.request("initialize", std::move(init));
        if (!capabilities) {
            return 0;
        }
        EXPECT_EQ(capabilities->boolAt("supportsConfigurationDoneRequest"), true) << capabilities->dump();

        Json launch = Json::object();
        launch.set("program", native(FROST_LLDB_DEBUGGEE));
        Json argv = Json::array();
        for (const std::string& arg : args) {
            argv.push(arg);
        }
        launch.set("args", std::move(argv));
        launch.set("cwd", m_scratch.path());
        launch.set("stopOnEntry", false);
        return dap.send("launch", std::move(launch));
    }

    // setBreakpoints with one line in `source`; true when lldb-dap accepted it.
    static bool setBreakpoint(DapSession& dap, const std::string& source, int line) {
        Json sourceArg = Json::object();
        sourceArg.set("path", source);
        Json breakpoint = Json::object();
        breakpoint.set("line", line);
        Json arguments = Json::object();
        arguments.set("source", std::move(sourceArg));
        arguments.set("breakpoints", Json::array({std::move(breakpoint)}));
        auto body = dap.request("setBreakpoints", std::move(arguments));
        if (!body) {
            return false;
        }
        const Json* breakpoints = body->find("breakpoints");
        EXPECT_TRUE(breakpoints != nullptr && breakpoints->size() == 1) << body->dump();
        return breakpoints != nullptr && breakpoints->size() == 1;
    }

    // The top frame of `threadId`'s stack, or nullopt.
    static std::optional<Json> topFrame(DapSession& dap, std::int64_t threadId) {
        Json arguments = Json::object();
        arguments.set("threadId", threadId);
        arguments.set("startFrame", 0);
        arguments.set("levels", 1);
        auto body = dap.request("stackTrace", std::move(arguments));
        const Json* frames = body ? body->find("stackFrames") : nullptr;
        if (frames == nullptr || frames->size() == 0) {
            ADD_FAILURE() << "no stack frames: " << (body ? body->dump() : std::string("(no body)"));
            return std::nullopt;
        }
        return frames->items().front();
    }

    // disconnect with terminateDebuggee, then lldb-dap exits once stdin closes.
    static void disconnect(DapSession& dap) {
        Json arguments = Json::object();
        arguments.set("terminateDebuggee", true);
        EXPECT_TRUE(dap.request("disconnect", std::move(arguments)).has_value());
        dap.process().closeStdin();
        auto exited = dap.process().wait(15s);
        EXPECT_TRUE(exited.has_value()) << "lldb-dap did not exit after disconnect";
    }

    std::string m_lldbDap;
    frost::test::ScratchDir m_scratch{"lldb-dap"};
#ifdef _WIN32
    frost::test::PrivateDesktop m_desktop;
#endif
};

std::string fileNameOf(const Json& frame) {
    const Json* source = frame.find("source");
    const std::string path(source != nullptr ? source->stringAt("path") : std::string_view());
    const auto slash = path.find_last_of(kWindows ? "\\/" : "/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

TEST_F(LldbDapE2E, StopsAtALineBreakpointInMainAndReportsTheTopFrame) {
    const std::string mainSource = fixtureSource("main.cpp");
    const int line = lineOf(mainSource, "// BP:main");
    ASSERT_GT(line, 0) << mainSource;
    auto dap = start();
    ASSERT_NE(dap, nullptr);

    const std::int64_t launchSeq = initializeAndLaunch(*dap, {"run", "7"});
    ASSERT_NE(launchSeq, 0);
    ASSERT_TRUE(dap->event("initialized"));
    ASSERT_TRUE(setBreakpoint(*dap, mainSource, line));
    ASSERT_TRUE(dap->request("configurationDone"));
    auto launched = dap->response(launchSeq);
    ASSERT_TRUE(launched.has_value()) << dap->stderrText();
    ASSERT_EQ(launched->boolAt("success"), true) << launched->dump();

    auto stopped = dap->event("stopped");
    ASSERT_TRUE(stopped);
    const Json* body = stopped->find("body");
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->stringAt("reason"), "breakpoint") << stopped->dump();
    const auto threadId = body->integerAt("threadId");
    ASSERT_TRUE(threadId.has_value()) << stopped->dump();

    auto frame = topFrame(*dap, *threadId);
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->integerAt("line"), line) << frame->dump();
    EXPECT_EQ(fileNameOf(*frame), "main.cpp") << frame->dump();
    EXPECT_NE(frame->stringAt("name").find("main"), std::string_view::npos) << frame->dump();

    disconnect(*dap);
}

TEST_F(LldbDapE2E, StopsInAnotherFileThenRunsToTheExitCode) {
    const std::string workerSource = fixtureSource("worker.cpp");
    const int line = lineOf(workerSource, "// BP:worker");
    ASSERT_GT(line, 0) << workerSource;
    auto dap = start();
    ASSERT_NE(dap, nullptr);

    const std::int64_t launchSeq = initializeAndLaunch(*dap, {"run", "7"});
    ASSERT_NE(launchSeq, 0);
    ASSERT_TRUE(dap->event("initialized"));
    ASSERT_TRUE(setBreakpoint(*dap, workerSource, line));
    ASSERT_TRUE(dap->request("configurationDone"));
    auto launched = dap->response(launchSeq);
    ASSERT_TRUE(launched.has_value()) << dap->stderrText();
    ASSERT_EQ(launched->boolAt("success"), true) << launched->dump();
    /* The baseline threads request a client sends after configurationDone (VS Code does). lldb-dap
       answers the first threads request after a launch from the thread list it cached at launch
       time, whenever it arrives, so without this one the request at the stop below would list only
       the main thread (S10, ADR 0049). */
    ASSERT_TRUE(dap->request("threads"));

    auto stopped = dap->event("stopped");
    ASSERT_TRUE(stopped);
    const auto threadId = stopped->find("body") ? stopped->find("body")->integerAt("threadId") : std::nullopt;
    ASSERT_TRUE(threadId.has_value()) << stopped->dump();
    auto frame = topFrame(*dap, *threadId);
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->integerAt("line"), line) << frame->dump();
    EXPECT_EQ(fileNameOf(*frame), "worker.cpp") << frame->dump();

    // main waits in std::thread::join while the worker is stopped: at least two threads.
    auto threads = dap->request("threads");
    ASSERT_TRUE(threads);
    const Json* list = threads->find("threads");
    ASSERT_NE(list, nullptr);
    EXPECT_GE(list->size(), 2U) << threads->dump();

    Json continueArgs = Json::object();
    continueArgs.set("threadId", *threadId);
    ASSERT_TRUE(dap->request("continue", std::move(continueArgs)));
    auto exited = dap->event("exited");
    ASSERT_TRUE(exited);
    const Json* exitBody = exited->find("body");
    ASSERT_NE(exitBody, nullptr);
    EXPECT_EQ(exitBody->integerAt("exitCode"), 7) << exited->dump();
    EXPECT_TRUE(dap->event("terminated"));

    disconnect(*dap);
}

} // namespace
