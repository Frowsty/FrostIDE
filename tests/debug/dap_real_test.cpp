// SPDX-License-Identifier: Apache-2.0
//
// The dap-real scenario suite (DBG-1, DBG-2a; label "integration", tag "lldb"): DebugSession drives
// the pinned lldb-dap through the real process launcher against tests/debug/fixtures/dap_scenarios.cpp
// (clang-cl and a PDB on Windows, clang and DWARF on Linux), one scenario per test.
/*
 * Oracles. Every expected value comes from outside the debugger: the fixture run without a debugger
 * through the same launcher (its output and exit code), the fixture's source (the "// MARK: <name>"
 * lines a scenario breaks on), the operating system (whether a process still runs) or the adapter's
 * own answers (the capabilities it lists). Nothing is a number written into the test.
 *
 * Load. ctest runs the scenarios one at a time (integration tests share the frost_desktop resource
 * lock) but beside other suites; each waits for events with a deadline (kWait) and never sleeps to
 * synchronise. A missing lldb-dap fails every scenario (FND-6), never skips it.
 */

#include "frost/debug/debug_session.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>

#include <cerrno>
#include <csignal>
#endif

#ifndef FROST_SCENARIOS_PATH
#error "FROST_SCENARIOS_PATH comes from tests/debug/CMakeLists.txt"
#endif

namespace frost::debug {
namespace {

using namespace std::chrono_literals;

// How long a scenario waits for one thing lldb-dap does (generous: other suites may load the machine).
constexpr auto kWait = 90s;

// Whether process `pid` still runs (the oracle for terminate and disconnect).
bool processAlive(std::int64_t pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return false;
    }
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

// Sets a variable of this test process (lldb-dap and so the debuggee inherit it).
void setProcessVariable(const char* name, const char* value) {
#ifdef _WIN32
    ASSERT_EQ(_putenv_s(name, value), 0);
#else
    ASSERT_EQ(::setenv(name, value, 1), 0);
#endif
}

// The value of "key=<value>" in program output, up to the line's end; "" when missing.
std::string field(std::string_view text, std::string_view key) {
    const std::string prefix = std::string(key) + "=";
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.starts_with(prefix)) {
            return std::string(line.substr(prefix.size()));
        }
        start = end + 1;
    }
    return {};
}

// Parse complete decimal oracle values, rejecting missing, malformed and out-of-range output.
std::optional<int> oracleInteger(std::string_view value) {
    int number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size())
        return std::nullopt;
    return number;
}

// Every line of `text` that starts with `prefix`, without the prefix.
std::vector<std::string> linesStartingWith(std::string_view text, std::string_view prefix) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.starts_with(prefix)) {
            out.emplace_back(line.substr(prefix.size()));
        }
        start = end + 1;
    }
    return out;
}

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// Two absolute paths name one file (case-insensitively and either separator on Windows).
bool samePath(std::string_view a, std::string_view b) {
    const auto normal = [](std::string_view path) {
        std::string out = io::normalizePath(path).value_or(std::string(path));
        if constexpr (test::kWindows) {
            std::ranges::transform(out, out.begin(), [](char c) {
                return c == '/' ? '\\' : (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
            });
        }
        return out;
    };
    return normal(a) == normal(b);
}

// A session's callbacks plus what the base recorder does not keep.
struct Recorder : test::SessionRecorder {
    std::optional<dap::Capabilities> caps FROST_GUARDED_BY(mutex);
    std::vector<std::pair<std::string, std::string>> outputs FROST_GUARDED_BY(mutex); // Category, text.

    DebugSessionCallbacks recording() {
        DebugSessionCallbacks out = callbacks();
        auto base = out.onOutput;
        out.onOutput = [this, base](std::string_view category, std::string_view text) {
            base(category, text);
            LockGuard lock(mutex);
            outputs.emplace_back(category, text);
        };
        out.onCapabilities = [this](const dap::Capabilities& found) {
            LockGuard lock(mutex);
            caps = found;
        };
        return out;
    }
};

// What a launch adds to the program and its arguments.
struct Extras {
    bool stopOnEntry = false;
    std::vector<FunctionBreakpointLine> functions{};
    std::vector<std::string> exceptionFilters{};
    std::vector<std::pair<std::string, std::string>> environment{};
    bool inheritEnvironment = true;
    std::string workingDirectory{};
};

class DapReal : public testing::Test {
protected:
    void SetUp() override {
        auto adapter = findLldbDap(io::realFileSystem(), defaultLldbDapSearch());
        ASSERT_TRUE(adapter.has_value()) << "the pinned lldb-dap is required: " << adapter.error().message();
        m_adapter = *adapter;
        m_program = io::normalizePath(FROST_SCENARIOS_PATH).value();
        m_sourcePath = io::normalizePath(FROST_SCENARIOS_SOURCE).value();
        auto source = io::realFileSystem().readFile(m_sourcePath);
        ASSERT_TRUE(source.has_value()) << source.error().describe();
        m_source = std::move(*source);
    }

    void TearDown() override {
        if (m_session) {
            m_session->kill();
            m_session.reset();
        }
    }

    // The 1-based line holding "// MARK: <name>" (the whole marker); a failure when it is missing.
    std::int64_t line(std::string_view name) const {
        const std::string marker = std::format("// MARK: {}", name);
        std::int64_t number = 1;
        std::size_t start = 0;
        while (start < m_source.size()) {
            const std::size_t end = std::min(m_source.find('\n', start), m_source.size());
            std::string_view text = std::string_view(m_source).substr(start, end - start);
            while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) {
                text.remove_suffix(1);
            }
            if (text.ends_with(marker)) {
                return number;
            }
            start = end + 1;
            ++number;
        }
        ADD_FAILURE() << "no marker " << marker << " in " << m_sourcePath;
        return 0;
    }

    BreakpointLine at(std::string_view name, std::uint64_t id = 1) const {
        return BreakpointLine{.id = id, .line = line(name)};
    }

    DebugTarget target(std::vector<std::string> arguments, const Extras& extras) const {
        DebugTarget out{.name = "dap_scenarios", .program = m_program};
        out.arguments = std::move(arguments);
        out.workingDirectory = extras.workingDirectory;
        out.environment = extras.environment;
        out.inheritEnvironment = extras.inheritEnvironment;
        out.stopOnEntry = extras.stopOnEntry;
        return out;
    }

    // Starts a session as the product does (lldbDapLaunchArguments, lldbDapSessionOptions).
    void launch(std::vector<std::string> arguments, std::vector<BreakpointLine> lines = {}, const Extras& extras = {}) {
        DebugSessionOptions options = lldbDapSessionOptions(DebugTargetMode::Launch);
        options.functionBreakpoints = extras.functions;
        options.exceptionFilters = extras.exceptionFilters;
        std::map<std::string, std::vector<BreakpointLine>> breakpoints;
        if (!lines.empty()) {
            breakpoints.emplace(m_sourcePath, std::move(lines));
        }
        m_session = DebugSession::start(lldbDapAdapterFactory(io::realProcessLauncher(), m_adapter),
                                        lldbDapLaunchArguments(target(std::move(arguments), extras)),
                                        std::move(breakpoints), m_recorder.recording(), std::move(options));
    }

    // Launches "wait" and returns once it prints its pid, so the program is in its spin loop.
    void launchSpinning() {
        launch({"wait"});
        ASSERT_TRUE(waitOutput("pid="));
        ASSERT_EQ(m_session->state(), DebugState::Running);
    }

    // Attaches to `pid` as the workbench does (workbench_run.cpp, Attach to Process).
    void attach(std::int64_t pid, std::vector<BreakpointLine> lines = {}) {
        DebugSessionOptions options = lldbDapSessionOptions(DebugTargetMode::Attach);
        std::map<std::string, std::vector<BreakpointLine>> breakpoints;
        if (!lines.empty()) {
            breakpoints.emplace(m_sourcePath, std::move(lines));
        }
        rpc::Json arguments = rpc::Json::object()
                                  .set("name", "dap_scenarios")
                                  .set("type", "lldb-dap")
                                  .set("request", "attach")
                                  .set("pid", pid)
                                  .set("program", m_program);
        m_session =
            DebugSession::start(lldbDapAdapterFactory(io::realProcessLauncher(), m_adapter), std::move(arguments),
                                std::move(breakpoints), m_recorder.recording(), std::move(options));
    }

    // What the session saw, for failure messages.
    std::string report() const {
        LockGuard lock(m_recorder.mutex);
        std::string text = "events: " + testing::PrintToString(m_recorder.events) + "\nbreakpoints:";
        for (const BreakpointResult& result : m_recorder.breakpoints) {
            text += std::format(" [{} verified={} line={} '{}']", result.id, result.verified, result.line.value_or(0),
                                result.message);
        }
        for (const StopInfo& stop : m_recorder.stops) {
            text += std::format("\nstop: {} '{}' '{}' filter={}", stop.reason, stop.description, stop.text,
                                stop.exceptionFilter);
        }
        text += "\noutput: " + m_recorder.output.substr(0, 4000);
        if (m_recorder.ended && m_recorder.ended->error) {
            text += "\nerror: " + m_recorder.ended->error->describe();
        }
        return text;
    }

    // Waits for stop `count` (1-based) with its frame selected; nullopt (and a failure) otherwise.
    std::optional<StopInfo> waitStop(std::size_t count = 1) {
        const bool ok = test::waitUntil(
            [&] {
                return m_recorder.hasEnded() ||
                       (m_recorder.stopCount() >= count && m_session->state() == DebugState::Stopped &&
                        m_session->selectedFrame().has_value());
            },
            kWait);
        if (!ok || m_recorder.stopCount() < count || !m_session->selectedFrame()) {
            ADD_FAILURE() << "no stop " << count << (ok ? " (the session ended)" : " (timed out)") << "\n" << report();
            return std::nullopt;
        }
        LockGuard lock(m_recorder.mutex);
        return m_recorder.stops[count - 1];
    }

    // Waits for the session to end; its outcome (a failure when it did not end).
    std::optional<DebugOutcome> waitEnd() {
        if (!test::waitUntil([&] { return m_recorder.hasEnded(); }, kWait)) {
            ADD_FAILURE() << "the session did not end\n" << report();
            return std::nullopt;
        }
        return m_session->outcome();
    }

    // Continues and waits for the end; the exit code (nullopt and a failure without one).
    std::optional<int> continueToExit() {
        if (auto resumed = m_session->continueRun(); !resumed) {
            ADD_FAILURE() << resumed.error().describe();
            return std::nullopt;
        }
        const auto outcome = waitEnd();
        if (!outcome || !outcome->exitCode) {
            ADD_FAILURE() << "no exit code\n" << report();
            return std::nullopt;
        }
        return outcome->exitCode;
    }

    DebugFrame frame() const { return m_session->selectedFrame().value_or(DebugFrame{}); }

    std::string output() const {
        LockGuard lock(m_recorder.mutex);
        return m_recorder.output;
    }

    bool waitOutput(std::string_view text) {
        const bool ok = test::waitUntil([&] { return contains(output(), text); }, kWait);
        if (!ok) {
            ADD_FAILURE() << "no output '" << text << "'\n" << report();
        }
        return ok;
    }

    template<typename T, typename Send>
    static Result<T> await(Send send) {
        auto promise = std::make_shared<std::promise<Result<T>>>();
        auto future = promise->get_future();
        send([promise](Result<T> result) { promise->set_value(std::move(result)); });
        if (future.wait_for(kWait) != std::future_status::ready) {
            return fail(ErrorCode::Timeout, "lldb-dap did not answer in time");
        }
        return future.get();
    }

    // The Locals scope of `frameId` (the selected frame by default).
    std::optional<dap::Scope> localsScope(std::optional<std::int64_t> frameId = std::nullopt) {
        auto scopes =
            await<ScopeList>([&](const auto& done) { m_session->scopes(frameId.value_or(frame().id), done); });
        if (!scopes) {
            ADD_FAILURE() << scopes.error().describe();
            return std::nullopt;
        }
        const auto locals = std::ranges::find_if(*scopes, [](const dap::Scope& scope) {
            return scope.presentationHint == "locals" || scope.name == "Locals";
        });
        if (locals == scopes->end()) {
            ADD_FAILURE() << "no Locals scope";
            return std::nullopt;
        }
        return *locals;
    }

    VariableList variablesOf(std::int64_t reference) {
        auto list = await<VariableList>([&](const auto& done) { m_session->variables(reference, 0, 0, done); });
        if (!list) {
            ADD_FAILURE() << list.error().describe();
            return {};
        }
        return std::move(*list);
    }

    VariableList locals(std::optional<std::int64_t> frameId = std::nullopt) {
        const auto scope = localsScope(frameId);
        return scope ? variablesOf(scope->variablesReference) : VariableList{};
    }

    static std::optional<dap::Variable> named(const VariableList& list, std::string_view name) {
        const auto it = std::ranges::find(list, name, &dap::Variable::name);
        return it == list.end() ? std::nullopt : std::optional<dap::Variable>(*it);
    }

    // A local of the selected frame; a failure when it is missing.
    dap::Variable local(std::string_view name) {
        const VariableList list = locals();
        if (auto found = named(list, name)) {
            return *found;
        }
        ADD_FAILURE() << "no local " << name;
        return {};
    }

    // "watch": lldb-dap's "repl" context answers in lldb's command style ("(int) $0 = 4").
    Result<dap::Variable> eval(std::string expression, std::string context = "watch") {
        return await<dap::Variable>([&](const auto& done) {
            m_session->evaluate(std::move(expression), frame().id, std::move(context), done);
        });
    }

    std::string evalValue(std::string expression) {
        auto value = eval(std::move(expression));
        if (!value) {
            ADD_FAILURE() << value.error().describe();
            return {};
        }
        return value->value;
    }

    // The fixture run without a debugger (the oracle).
    io::ProcessResult oracle(std::vector<std::string> arguments,
                             std::vector<std::pair<std::string, std::string>> env = {},
                             std::string workingDirectory = {}, bool cleanEnvironment = false) {
        io::ProcessSpec spec;
        spec.executable = m_program;
        spec.arguments = std::move(arguments);
        spec.workingDirectory = std::move(workingDirectory);
        spec.cleanEnvironment = cleanEnvironment;
        for (auto& [name, value] : env) {
            spec.environment.emplace_back(name, value);
        }
        auto result = io::runToCompletion(io::realProcessLauncher(), spec, {}, std::chrono::seconds(60));
        if (!result) {
            ADD_FAILURE() << "the oracle run failed: " << result.error().describe();
            return {};
        }
        return std::move(*result);
    }

    std::optional<BreakpointResult> resultFor(std::uint64_t id) const {
        LockGuard lock(m_recorder.mutex);
        std::optional<BreakpointResult> last;
        for (const BreakpointResult& result : m_recorder.breakpoints) {
            if (result.id == id) {
                last = result;
            }
        }
        return last;
    }

    bool waitVerified(std::uint64_t id) {
        const bool ok = test::waitUntil(
            [&] {
                const auto result = resultFor(id);
                return (result && result->verified) || m_recorder.hasEnded();
            },
            kWait);
        const auto result = resultFor(id);
        if (!ok || !result || !result->verified) {
            ADD_FAILURE() << "breakpoint " << id << " was not verified\n" << report();
            return false;
        }
        return true;
    }

    dap::Capabilities waitCapabilities(const std::function<bool(const dap::Capabilities&)>& done) {
        test::waitUntil(
            [&] {
                LockGuard lock(m_recorder.mutex);
                return m_recorder.caps && done(*m_recorder.caps);
            },
            kWait);
        LockGuard lock(m_recorder.mutex);
        return m_recorder.caps.value_or(dap::Capabilities{});
    }

    std::size_t stops() const { return m_recorder.stopCount(); }

    // The frames of the selected thread.
    std::vector<DebugFrame> stack() const {
        const auto thread = m_session->selectedThread();
        return thread ? m_session->frames(*thread).frames : std::vector<DebugFrame>{};
    }

    std::string m_adapter;
    std::string m_program;
    std::string m_sourcePath;
    std::string m_source;
    Recorder m_recorder;
    std::unique_ptr<DebugSession> m_session;
};

// --- Launch, exit codes, arguments, environment ---

TEST_F(DapReal, LaunchRunsToTheExitCodeOfAnUndebuggedRun) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"});
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome && outcome->exitCode) << report();
    EXPECT_EQ(*outcome->exitCode, expected.exitCode);
    EXPECT_FALSE(outcome->error.has_value());
}

TEST_F(DapReal, ExitCodeSevenMatchesTheUndebuggedRun) {
    const io::ProcessResult expected = oracle({"exit", "7"});
    launch({"exit", "7"});
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome && outcome->exitCode) << report();
    EXPECT_EQ(*outcome->exitCode, expected.exitCode);
}

TEST_F(DapReal, ExitCodeZeroMatchesTheUndebuggedRun) {
    const io::ProcessResult expected = oracle({"exit", "0"});
    launch({"exit", "0"});
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome && outcome->exitCode) << report();
    EXPECT_EQ(*outcome->exitCode, expected.exitCode);
}

TEST_F(DapReal, ExitCodeTwoHundredMatchesTheUndebuggedRun) {
    const io::ProcessResult expected = oracle({"exit", "200"});
    launch({"exit", "200"});
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome && outcome->exitCode) << report();
    EXPECT_EQ(*outcome->exitCode, expected.exitCode);
}

TEST_F(DapReal, ExitedEventPrecedesTheEnd) {
    launch({"exit", "5"});
    ASSERT_TRUE(waitEnd());
    const auto events = m_recorder.eventList();
    const auto exited = std::ranges::find_if(events, [](const std::string& e) { return e.starts_with("exited "); });
    const auto ended = std::ranges::find(events, std::string("ended"));
    ASSERT_NE(exited, events.end()) << report();
    EXPECT_LT(exited - events.begin(), ended - events.begin());
}

TEST_F(DapReal, ArgumentsReachTheProgram) {
    const std::vector<std::string> args{"args", "alpha", "two words", "third"};
    const io::ProcessResult expected = oracle(args);
    launch(args);
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome && outcome->exitCode) << report();
    EXPECT_EQ(*outcome->exitCode, expected.exitCode);
    const std::string seen = output();
    for (const std::string& key : {"arg0", "arg1", "arg2"}) {
        EXPECT_EQ(field(seen, key), field(expected.stdoutBytes, key)) << key << "\n" << report();
    }
}

TEST_F(DapReal, EnvironmentReachesTheProgram) {
    const std::vector<std::pair<std::string, std::string>> env{{"FROST_SCENARIO_ENV", "dap-env-value"}};
    const io::ProcessResult expected = oracle({"env"}, env);
    launch({"env"}, {}, Extras{.environment = env});
    ASSERT_TRUE(waitEnd());
    EXPECT_EQ(field(output(), "env"), field(expected.stdoutBytes, "env")) << report();
}

TEST_F(DapReal, InheritedEnvironmentReachesTheProgram) {
    setProcessVariable("FROST_SCENARIO_ENV", "inherited-by-the-debuggee");
    const io::ProcessResult expected = oracle({"env"});
    launch({"env"});
    ASSERT_TRUE(waitEnd());
    EXPECT_EQ(field(output(), "env"), field(expected.stdoutBytes, "env")) << report();
}

TEST_F(DapReal, UninheritedEnvironmentHidesTheParentsVariables) {
    setProcessVariable("FROST_SCENARIO_ENV", "must-not-be-inherited");
    const io::ProcessResult expected = oracle({"env"}, {}, {}, true);
    launch({"env"}, {}, Extras{.inheritEnvironment = false});
    ASSERT_TRUE(waitEnd());
    EXPECT_EQ(field(output(), "env"), field(expected.stdoutBytes, "env")) << report();
}

TEST_F(DapReal, WorkingDirectoryIsTheConfiguredOne) {
    const std::string directory(io::parentPath(m_sourcePath));
    ASSERT_FALSE(directory.empty());
    const io::ProcessResult expected = oracle({"cwd"}, {}, directory);
    launch({"cwd"}, {}, Extras{.workingDirectory = directory});
    ASSERT_TRUE(waitEnd());
    const std::string seen = field(output(), "cwd");
    EXPECT_TRUE(samePath(seen, field(expected.stdoutBytes, "cwd"))) << seen << "\n" << report();
}

TEST_F(DapReal, ProcessEventGivesTheDebuggeesProcessId) {
    launch({"wait"});
    ASSERT_TRUE(waitOutput("pid="));
    const std::string printed = field(output(), "pid");
    ASSERT_TRUE(test::waitUntil([&] { return m_session->processId().has_value(); }, kWait)) << report();
    EXPECT_EQ(std::to_string(*m_session->processId()), printed);
}

// --- Stop on entry ---

TEST_F(DapReal, StopOnEntryStopsWithReasonEntry) {
    launch({"values"}, {}, Extras{.stopOnEntry = true});
    const auto stop = waitStop();
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->reason, "entry");
    EXPECT_TRUE(output().find("outer.inner.count=") == std::string::npos) << "the program ran before the stop";
}

TEST_F(DapReal, StopOnEntryThenContinueRunsToTheExit) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {}, Extras{.stopOnEntry = true});
    ASSERT_TRUE(waitStop());
    EXPECT_EQ(continueToExit(), expected.exitCode);
}

TEST_F(DapReal, StopOnEntryThenABreakpointAddedThereIsHit) {
    launch({"values"}, {}, Extras{.stopOnEntry = true});
    ASSERT_TRUE(waitStop());
    m_session->setBreakpoints(m_sourcePath, {at("values")});
    ASSERT_TRUE(m_session->continueRun());
    const auto stop = waitStop(2);
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->reason, "breakpoint");
    EXPECT_EQ(frame().line, line("values"));
}

// --- Output ---

TEST_F(DapReal, StdoutArrivesAsOutputEvents) {
    const io::ProcessResult expected = oracle({"output"});
    launch({"output"});
    ASSERT_TRUE(waitEnd());
    const std::string line = linesStartingWith(expected.stdoutBytes, "frost-stdout").at(0);
    EXPECT_TRUE(contains(output(), "frost-stdout" + line)) << report();
}

TEST_F(DapReal, StderrArrivesAsOutputEvents) {
    const io::ProcessResult expected = oracle({"output"});
    launch({"output"});
    ASSERT_TRUE(waitEnd());
    const std::string line = linesStartingWith(expected.stderrBytes, "frost-stderr").at(0);
    EXPECT_TRUE(contains(output(), "frost-stderr" + line)) << report();
}

TEST_F(DapReal, ProgramOutputHasTheStdoutCategory) {
    launch({"output"});
    ASSERT_TRUE(waitEnd());
    LockGuard lock(m_recorder.mutex);
    const bool found = std::ranges::any_of(m_recorder.outputs, [](const auto& entry) {
        return entry.first == "stdout" && contains(entry.second, "frost-stdout-line");
    });
    EXPECT_TRUE(found);
}

TEST_F(DapReal, AllValuesOutputArrivesBeforeTheEnd) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"});
    ASSERT_TRUE(waitEnd());
    const std::string seen = output();
    for (const std::string key : {"outer.inner.count", "outer.values[2]", "text", "numbers.size", "total"}) {
        EXPECT_EQ(field(seen, key), field(expected.stdoutBytes, key)) << key;
    }
}

// --- Line breakpoints ---

TEST_F(DapReal, LineBreakpointIsVerified) {
    launch({"values"}, {at("values")});
    EXPECT_TRUE(waitVerified(1));
}

TEST_F(DapReal, LineBreakpointStopsAtItsLine) {
    launch({"values"}, {at("values")});
    const auto stop = waitStop();
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->reason, "breakpoint");
    EXPECT_EQ(frame().line, line("values"));
    EXPECT_TRUE(samePath(frame().path, m_sourcePath)) << frame().path;
    EXPECT_FALSE(stop->hitBreakpointIds.empty());
}

TEST_F(DapReal, LineBreakpointStopsAfterTheLinesBeforeItRan) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("values-return")});
    ASSERT_TRUE(waitStop());
    // total is complete on the return line; the program printed it on the line before.
    EXPECT_EQ(evalValue("total"), field(expected.stdoutBytes, "total"));
}

TEST_F(DapReal, LoopBreakpointStopsOnEveryIterationInOrder) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("loop")});
    int iterations = 0;
    while (true) {
        const auto stop = waitStop(static_cast<std::size_t>(iterations) + 1);
        ASSERT_TRUE(stop);
        EXPECT_EQ(evalValue("i"), std::to_string(iterations));
        ++iterations;
        ASSERT_TRUE(m_session->continueRun());
        if (test::waitUntil([&] { return m_recorder.hasEnded() || stops() > static_cast<std::size_t>(iterations); },
                            kWait) &&
            m_recorder.hasEnded()) {
            break;
        }
        ASSERT_LT(iterations, 1000) << "the loop never ended";
    }
    EXPECT_GT(iterations, 1);
    EXPECT_EQ(stops(), static_cast<std::size_t>(iterations));
    EXPECT_EQ(m_session->outcome()->exitCode, expected.exitCode);
}

TEST_F(DapReal, TwoBreakpointsStopInProgramOrder) {
    launch({"values"}, {at("values", 1), at("values-return", 2)});
    ASSERT_TRUE(waitStop(1));
    EXPECT_EQ(frame().line, line("values"));
    ASSERT_TRUE(m_session->continueRun());
    ASSERT_TRUE(waitStop(2));
    EXPECT_EQ(frame().line, line("values-return"));
}

TEST_F(DapReal, BreakpointAddedWhileStoppedIsHit) {
    launch({"values"}, {at("values", 1)});
    ASSERT_TRUE(waitStop(1));
    m_session->setBreakpoints(m_sourcePath, {at("values", 1), at("values-return", 2)});
    ASSERT_TRUE(waitVerified(2));
    ASSERT_TRUE(m_session->continueRun());
    ASSERT_TRUE(waitStop(2));
    EXPECT_EQ(frame().line, line("values-return"));
}

TEST_F(DapReal, BreakpointRemovedWhileStoppedIsNotHitAgain) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("loop")});
    ASSERT_TRUE(waitStop(1));
    m_session->setBreakpoints(m_sourcePath, {});
    EXPECT_EQ(continueToExit(), expected.exitCode);
    EXPECT_EQ(stops(), 1U);
}

TEST_F(DapReal, BreakpointOnALineWithoutCodeBindsWhereItStops) {
    // The line after accumulate's closing brace is blank.
    const std::int64_t blank = line("accumulate") + 3;
    launch({"values"}, {BreakpointLine{.id = 1, .line = blank}});
    ASSERT_TRUE(test::waitUntil([&] { return resultFor(1).has_value() || m_recorder.hasEnded(); }, kWait));
    const auto first = resultFor(1);
    ASSERT_TRUE(first);
    test::waitUntil([&] { return stops() >= 1 || m_recorder.hasEnded(); }, kWait);
    const auto result = resultFor(1);
    if (stops() >= 1) {
        ASSERT_TRUE(waitStop(1));
        ASSERT_TRUE(result->verified) << report();
        ASSERT_TRUE(result->line.has_value());
        EXPECT_NE(*result->line, blank);
        EXPECT_EQ(frame().line, *result->line);
    } else {
        EXPECT_FALSE(result->verified) << report();
    }
}

TEST_F(DapReal, BreakpointInAFileNotInTheProgramIsUnverified) {
    const io::ProcessResult expected = oracle({"values"});
    const std::string other = io::joinPath(io::parentPath(m_sourcePath), "not_in_the_program.cpp").value();
    DebugSessionOptions options = lldbDapSessionOptions(DebugTargetMode::Launch);
    m_session = DebugSession::start(lldbDapAdapterFactory(io::realProcessLauncher(), m_adapter),
                                    lldbDapLaunchArguments(target({"values"}, {})),
                                    {{other, {BreakpointLine{.id = 1, .line = 3}}}}, m_recorder.recording(), options);
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome && outcome->exitCode);
    EXPECT_EQ(*outcome->exitCode, expected.exitCode);
    const auto result = resultFor(1);
    ASSERT_TRUE(result) << report();
    EXPECT_FALSE(result->verified);
    EXPECT_EQ(stops(), 0U);
}

TEST_F(DapReal, RunToLineStopsThere) {
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    ASSERT_TRUE(m_session->runToLine(m_sourcePath, line("values-return")));
    ASSERT_TRUE(waitStop(2));
    EXPECT_EQ(frame().line, line("values-return"));
}

TEST_F(DapReal, RunToLineLeavesNoBreakpointBehind) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    ASSERT_TRUE(m_session->runToLine(m_sourcePath, line("loop")));
    ASSERT_TRUE(waitStop(2));
    EXPECT_EQ(frame().line, line("loop"));
    // The loop line runs again, but the temporary breakpoint is gone.
    EXPECT_EQ(continueToExit(), expected.exitCode);
    EXPECT_EQ(stops(), 2U);
}

// --- Conditions, hit counts, logpoints ---

TEST_F(DapReal, ConditionalBreakpointStopsOnlyWhenTrue) {
    const io::ProcessResult expected = oracle({"values"});
    BreakpointLine conditional = at("loop");
    conditional.condition = "i == 4";
    launch({"values"}, {conditional});
    ASSERT_TRUE(waitStop(1));
    EXPECT_EQ(evalValue("i"), "4");
    EXPECT_EQ(continueToExit(), expected.exitCode);
    EXPECT_EQ(stops(), 1U);
}

TEST_F(DapReal, ConditionalBreakpointThatIsNeverTrueNeverStops) {
    const io::ProcessResult expected = oracle({"values"});
    BreakpointLine conditional = at("loop");
    conditional.condition = "i < 0";
    launch({"values"}, {conditional});
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome && outcome->exitCode) << report();
    EXPECT_EQ(*outcome->exitCode, expected.exitCode);
    EXPECT_EQ(stops(), 0U);
}

TEST_F(DapReal, ConditionOnAStructMemberIsEvaluated) {
    BreakpointLine conditional = at("values");
    const io::ProcessResult expected = oracle({"values"});
    conditional.condition = "outer.inner.count == " + field(expected.stdoutBytes, "outer.inner.count");
    launch({"values"}, {conditional});
    ASSERT_TRUE(waitStop(1));
    EXPECT_EQ(frame().line, line("values"));
}

TEST_F(DapReal, ConditionAddedWhileStoppedApplies) {
    launch({"values"}, {at("loop")});
    ASSERT_TRUE(waitStop(1));
    BreakpointLine conditional = at("loop");
    conditional.condition = "i == 7";
    m_session->setBreakpoints(m_sourcePath, {conditional});
    ASSERT_TRUE(m_session->continueRun());
    ASSERT_TRUE(waitStop(2));
    EXPECT_EQ(evalValue("i"), "7");
}

TEST_F(DapReal, HitCountFirstStopsOnTheNthHit) {
    BreakpointLine counted = at("loop");
    counted.hitCondition = "3";
    launch({"values"}, {counted});
    ASSERT_TRUE(waitStop(1));
    // lldb's ignore count: hits one and two pass, the third (i == 2) stops.
    EXPECT_EQ(evalValue("i"), "2");
}

TEST_F(DapReal, HitCountKeepsStoppingAfterTheNthHit) {
    BreakpointLine counted = at("loop");
    counted.hitCondition = "3";
    launch({"values"}, {counted});
    ASSERT_TRUE(waitStop(1));
    const auto parsedFirst = oracleInteger(evalValue("i"));
    ASSERT_TRUE(parsedFirst);
    const int first = *parsedFirst;
    ASSERT_TRUE(m_session->continueRun());
    ASSERT_TRUE(waitStop(2));
    EXPECT_EQ(evalValue("i"), std::to_string(first + 1));
}

TEST_F(DapReal, LogpointWritesItsInterpolatedMessage) {
    const io::ProcessResult expected = oracle({"values"});
    BreakpointLine logpoint = at("values-return");
    logpoint.logMessage = "log total={total}";
    launch({"values"}, {logpoint});
    ASSERT_TRUE(waitEnd());
    EXPECT_TRUE(contains(output(), "log total=" + field(expected.stdoutBytes, "total"))) << report();
}

TEST_F(DapReal, LogpointDoesNotStop) {
    const io::ProcessResult expected = oracle({"values"});
    BreakpointLine logpoint = at("loop");
    logpoint.logMessage = "iteration";
    launch({"values"}, {logpoint});
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome && outcome->exitCode) << report();
    EXPECT_EQ(*outcome->exitCode, expected.exitCode);
    EXPECT_EQ(stops(), 0U);
}

TEST_F(DapReal, LogpointFiresOnEveryIteration) {
    BreakpointLine logpoint = at("loop");
    logpoint.logMessage = "frost-iteration={i}";
    launch({"values"}, {logpoint});
    ASSERT_TRUE(waitEnd());
    const std::vector<std::string> values = linesStartingWith(output(), "frost-iteration=");
    ASSERT_GT(values.size(), 1U) << report();
    for (std::size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(values[i], std::to_string(i));
    }
}

TEST_F(DapReal, LogpointWithAConditionLogsOnlyWhenTrue) {
    BreakpointLine logpoint = at("loop");
    logpoint.logMessage = "frost-conditional={i}";
    logpoint.condition = "i == 6";
    launch({"values"}, {logpoint});
    ASSERT_TRUE(waitEnd());
    EXPECT_EQ(linesStartingWith(output(), "frost-conditional="), std::vector<std::string>{"6"}) << report();
}

// --- Function breakpoints ---

TEST_F(DapReal, FunctionBreakpointStopsInTheFunction) {
    launch({"values"}, {}, Extras{.functions = {{.id = 1, .name = "frost_function_target"}}});
    const auto stop = waitStop(1);
    ASSERT_TRUE(stop);
    EXPECT_TRUE(contains(frame().name, "frost_function_target")) << frame().name;
    EXPECT_TRUE(samePath(frame().path, m_sourcePath));
}

TEST_F(DapReal, FunctionBreakpointIsVerified) {
    launch({"values"}, {}, Extras{.functions = {{.id = 1, .name = "frost_function_target"}}});
    EXPECT_TRUE(waitVerified(1));
}

TEST_F(DapReal, FunctionBreakpointOnAnUnknownNameIsNotVerified) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {}, Extras{.functions = {{.id = 1, .name = "frost_no_such_function_anywhere"}}});
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome && outcome->exitCode);
    EXPECT_EQ(*outcome->exitCode, expected.exitCode);
    const auto result = resultFor(1);
    ASSERT_TRUE(result) << report();
    EXPECT_FALSE(result->verified);
    EXPECT_EQ(stops(), 0U);
}

TEST_F(DapReal, ConditionalFunctionBreakpointSeesTheArgument) {
    const io::ProcessResult expected = oracle({"values"});
    const std::string size = field(expected.stdoutBytes, "numbers.size");
    launch({"values"}, {},
           Extras{.functions = {{.id = 1, .name = "frost_function_target", .condition = "value == " + size}}});
    ASSERT_TRUE(waitStop(1));
    EXPECT_EQ(evalValue("value"), size);
}

TEST_F(DapReal, FunctionBreakpointHitCountSkipsEarlierCalls) {
    launch({"values"}, {}, Extras{.functions = {{.id = 1, .name = "accumulate", .hitCondition = "5"}}});
    ASSERT_TRUE(waitStop(1));
    EXPECT_TRUE(contains(frame().name, "accumulate")) << frame().name;
    EXPECT_EQ(evalValue("i"), "4");
}

TEST_F(DapReal, FunctionBreakpointAddedWhileStoppedIsHit) {
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    m_session->setFunctionBreakpoints({{.id = 2, .name = "frost_function_target"}});
    ASSERT_TRUE(waitVerified(2));
    ASSERT_TRUE(m_session->continueRun());
    ASSERT_TRUE(waitStop(2));
    EXPECT_TRUE(contains(frame().name, "frost_function_target")) << frame().name;
}

TEST_F(DapReal, FunctionBreakpointRemovedWhileStoppedIsNotHit) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("values")}, Extras{.functions = {{.id = 2, .name = "frost_function_target"}}});
    ASSERT_TRUE(waitStop(1));
    m_session->setFunctionBreakpoints({});
    EXPECT_EQ(continueToExit(), expected.exitCode);
    EXPECT_EQ(stops(), 1U);
}

// --- Exception filters ---

TEST_F(DapReal, CapabilitiesListTheCppExceptionFilters) {
    launch({"values"});
    const dap::Capabilities caps =
        waitCapabilities([](const dap::Capabilities& found) { return !found.exceptionBreakpointFilters.empty(); });
    const auto has = [&](std::string_view id) {
        return std::ranges::any_of(caps.exceptionBreakpointFilters, [&](const dap::ExceptionFilter& filter) {
            return filter.filter == id && !filter.label.empty();
        });
    };
    EXPECT_TRUE(has("cpp_throw"));
    EXPECT_TRUE(has("cpp_catch"));
}

TEST_F(DapReal, CppThrowStopsAtTheThrow) {
    launch({"throw"}, {}, Extras{.exceptionFilters = {"cpp_throw"}});
    const auto stop = waitStop(1);
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->reason, "exception");
    EXPECT_EQ(stop->exceptionFilter, "cpp_throw");
    EXPECT_FALSE(stop->isCrash());
    EXPECT_EQ(frame().line, line("throw")) << frame().name << "\n" << report();
    EXPECT_TRUE(contains(frame().name, "thrower")) << frame().name;
}

TEST_F(DapReal, CppThrowStopSummaryNamesTheFilter) {
    launch({"throw"}, {}, Extras{.exceptionFilters = {"cpp_throw"}});
    const auto stop = waitStop(1);
    ASSERT_TRUE(stop);
    const dap::Capabilities caps = m_session->capabilities();
    const auto filter =
        std::ranges::find(caps.exceptionBreakpointFilters, std::string("cpp_throw"), &dap::ExceptionFilter::filter);
    ASSERT_NE(filter, caps.exceptionBreakpointFilters.end());
    EXPECT_EQ(stop->summary(), std::format("Paused on exception ({})", filter->label));
}

TEST_F(DapReal, CppThrowStopsOncePerThrow) {
    const io::ProcessResult expected = oracle({"throw"});
    launch({"throw"}, {}, Extras{.exceptionFilters = {"cpp_throw"}});
    ASSERT_TRUE(waitStop(1));
    EXPECT_EQ(continueToExit(), expected.exitCode);
    EXPECT_EQ(stops(), 1U) << report();
}

TEST_F(DapReal, CppThrowLetsTheProgramCatchIt) {
    const io::ProcessResult expected = oracle({"throw"});
    launch({"throw"}, {}, Extras{.exceptionFilters = {"cpp_throw"}});
    ASSERT_TRUE(waitStop(1));
    ASSERT_TRUE(continueToExit());
    EXPECT_EQ(field(output(), "caught"), field(expected.stdoutBytes, "caught")) << report();
}

TEST_F(DapReal, WithoutAFilterAThrowDoesNotStop) {
    const io::ProcessResult expected = oracle({"throw"});
    launch({"throw"});
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome && outcome->exitCode);
    EXPECT_EQ(*outcome->exitCode, expected.exitCode);
    EXPECT_EQ(stops(), 0U) << report();
    EXPECT_EQ(field(output(), "caught"), field(expected.stdoutBytes, "caught"));
}

TEST_F(DapReal, CppThrowEnabledWhileStoppedStopsAtTheThrow) {
    launch({"throw"}, {at("before-throw")});
    ASSERT_TRUE(waitStop(1));
    m_session->setExceptionFilters({"cpp_throw"});
    ASSERT_TRUE(m_session->continueRun());
    const auto stop = waitStop(2);
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->exceptionFilter, "cpp_throw");
    EXPECT_EQ(frame().line, line("throw"));
}

TEST_F(DapReal, CppThrowDisabledWhileStoppedNoLongerStops) {
    const io::ProcessResult expected = oracle({"throw"});
    launch({"throw"}, {at("before-throw")}, Extras{.exceptionFilters = {"cpp_throw"}});
    ASSERT_TRUE(waitStop(1));
    m_session->setExceptionFilters({});
    EXPECT_EQ(continueToExit(), expected.exitCode);
    EXPECT_EQ(stops(), 1U) << report();
}

TEST_F(DapReal, CppThrowCallStackReachesMain) {
    launch({"throw"}, {}, Extras{.exceptionFilters = {"cpp_throw"}});
    ASSERT_TRUE(waitStop(1));
    const std::vector<DebugFrame> frames = stack();
    EXPECT_TRUE(std::ranges::any_of(frames, [](const DebugFrame& f) { return contains(f.name, "main"); })) << report();
}

TEST_F(DapReal, CppCatchStopsAtTheCatchOrSaysWhyItCannot) {
    const io::ProcessResult expected = oracle({"throw"});
    launch({"throw"}, {}, Extras{.exceptionFilters = {"cpp_catch"}});
    if (m_session->msvcExceptions()) {
        // MSVC ABI: lldb cannot stop where a C++ exception is caught; the product says so (DBG-2b).
        const auto why = msvcUnsupportedFilter("cpp_catch");
        ASSERT_TRUE(why.has_value());
        EXPECT_TRUE(contains(*why, "DBG-2b"));
        const auto outcome = waitEnd();
        ASSERT_TRUE(outcome && outcome->exitCode);
        EXPECT_EQ(*outcome->exitCode, expected.exitCode);
        EXPECT_EQ(stops(), 0U);
        return;
    }
    const auto stop = waitStop(1);
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->exceptionFilter, "cpp_catch");
    EXPECT_TRUE(contains(frame().name, "main")) << frame().name;
    EXPECT_EQ(field(output(), "caught"), "") << "the catch handler ran before the stop";
}

// --- Stepping ---

TEST_F(DapReal, StepOverMovesToTheNextLine) {
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    ASSERT_TRUE(m_session->next());
    const auto stop = waitStop(2);
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->reason, "step");
    EXPECT_EQ(frame().line, line("values") + 1);
}

TEST_F(DapReal, StepOverACallStaysInTheCaller) {
    launch({"values"}, {at("loop")});
    ASSERT_TRUE(waitStop(1));
    const std::size_t depth = stack().size();
    const std::string caller = frame().name;
    ASSERT_TRUE(m_session->next());
    ASSERT_TRUE(waitStop(2));
    EXPECT_EQ(frame().name, caller);
    EXPECT_EQ(stack().size(), depth);
}

TEST_F(DapReal, StepIntoEntersTheCallee) {
    launch({"values"}, {at("loop")});
    ASSERT_TRUE(waitStop(1));
    ASSERT_TRUE(m_session->stepIn());
    ASSERT_TRUE(waitStop(2));
    EXPECT_TRUE(contains(frame().name, "accumulate")) << frame().name;
    EXPECT_EQ(frame().line, line("accumulate"));
}

TEST_F(DapReal, StepOutReturnsToTheCaller) {
    launch({"values"}, {at("accumulate")});
    ASSERT_TRUE(waitStop(1));
    ASSERT_TRUE(m_session->stepOut());
    ASSERT_TRUE(waitStop(2));
    EXPECT_TRUE(contains(frame().name, "run_values")) << frame().name;
    EXPECT_EQ(frame().line, line("loop"));
}

TEST_F(DapReal, StepOutOfARecursionGoesUpOneLevel) {
    launch({"deep"}, {at("recurse-bottom")});
    ASSERT_TRUE(waitStop(1));
    const std::size_t depth = stack().size();
    ASSERT_TRUE(m_session->stepOut());
    ASSERT_TRUE(waitStop(2));
    EXPECT_TRUE(contains(frame().name, "recurse")) << frame().name;
    EXPECT_EQ(frame().line, line("recurse-call"));
    EXPECT_EQ(stack().size() + 1, depth);
}

TEST_F(DapReal, StepIntoThenStepOutComesBack) {
    launch({"values"}, {at("loop")});
    ASSERT_TRUE(waitStop(1));
    const std::string caller = frame().name;
    ASSERT_TRUE(m_session->stepIn());
    ASSERT_TRUE(waitStop(2));
    ASSERT_TRUE(m_session->stepOut());
    ASSERT_TRUE(waitStop(3));
    EXPECT_EQ(frame().name, caller);
}

TEST_F(DapReal, StepsReportReasonStep) {
    launch({"values"}, {at("loop")});
    ASSERT_TRUE(waitStop(1));
    ASSERT_TRUE(m_session->stepIn());
    const auto in = waitStop(2);
    ASSERT_TRUE(in);
    ASSERT_TRUE(m_session->next());
    const auto over = waitStop(3);
    ASSERT_TRUE(over);
    EXPECT_EQ(in->reason, "step");
    EXPECT_EQ(over->reason, "step");
}

// --- Stack ---

TEST_F(DapReal, StackNamesTheCallChain) {
    launch({"values"}, {at("accumulate")});
    ASSERT_TRUE(waitStop(1));
    const std::vector<DebugFrame> frames = stack();
    ASSERT_GE(frames.size(), 3U) << report();
    EXPECT_TRUE(contains(frames[0].name, "accumulate")) << frames[0].name;
    EXPECT_TRUE(contains(frames[1].name, "run_values")) << frames[1].name;
    EXPECT_TRUE(contains(frames[2].name, "main")) << frames[2].name;
}

TEST_F(DapReal, StackFramesCarryTheSourceAndLines) {
    launch({"values"}, {at("accumulate")});
    ASSERT_TRUE(waitStop(1));
    const std::vector<DebugFrame> frames = stack();
    ASSERT_GE(frames.size(), 2U);
    EXPECT_TRUE(samePath(frames[0].path, m_sourcePath)) << frames[0].path;
    EXPECT_EQ(frames[0].line, line("accumulate"));
    EXPECT_TRUE(samePath(frames[1].path, m_sourcePath)) << frames[1].path;
    EXPECT_EQ(frames[1].line, line("loop"));
}

TEST_F(DapReal, RecursionDepthMatchesTheProgram) {
    const io::ProcessResult expected = oracle({"deep"});
    const auto parsedDepth = oracleInteger(field(expected.stdoutBytes, "depth"));
    ASSERT_TRUE(parsedDepth);
    const int depth = *parsedDepth;
    ASSERT_GT(depth, 0);
    launch({"deep"}, {at("recurse-bottom")});
    ASSERT_TRUE(waitStop(1));
    const std::vector<DebugFrame> frames = stack();
    const auto levels = std::ranges::count_if(frames, [](const DebugFrame& f) { return contains(f.name, "recurse"); });
    EXPECT_EQ(levels, depth + 1) << report();
}

TEST_F(DapReal, SelectingACallerFrameShowsItsLocals) {
    launch({"values"}, {at("accumulate")});
    ASSERT_TRUE(waitStop(1));
    const std::vector<DebugFrame> frames = stack();
    ASSERT_GE(frames.size(), 2U);
    ASSERT_TRUE(m_session->selectFrame(*m_session->selectedThread(), frames[1].id));
    EXPECT_EQ(frame().id, frames[1].id);
    const VariableList list = locals();
    EXPECT_TRUE(named(list, "numbers").has_value());
    EXPECT_TRUE(named(list, "text").has_value());
}

TEST_F(DapReal, EvaluateUsesTheSelectedFrame) {
    launch({"values"}, {at("accumulate")});
    ASSERT_TRUE(waitStop(1));
    const std::vector<DebugFrame> frames = stack();
    ASSERT_GE(frames.size(), 2U);
    // outer is a local of run_values (frame 1) only, not of accumulate (frame 0).
    EXPECT_FALSE(eval("outer.inner.count").has_value());
    ASSERT_TRUE(m_session->selectFrame(*m_session->selectedThread(), frames[1].id));
    const io::ProcessResult expected = oracle({"values"});
    EXPECT_EQ(evalValue("outer.inner.count"), field(expected.stdoutBytes, "outer.inner.count"));
}

// --- Scopes and variables ---

TEST_F(DapReal, ScopesStartWithCheapLocals) {
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    const auto scope = localsScope();
    ASSERT_TRUE(scope);
    EXPECT_FALSE(scope->expensive);
    EXPECT_GT(scope->variablesReference, 0);
}

TEST_F(DapReal, LocalsListTheFunctionsVariables) {
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    const VariableList list = locals();
    for (const std::string_view name : {"outer", "text", "numbers"}) {
        EXPECT_TRUE(named(list, name).has_value()) << name;
    }
}

TEST_F(DapReal, NestedStructMemberMatchesTheProgram) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    const dap::Variable outer = local("outer");
    ASSERT_GT(outer.variablesReference, 0);
    const auto inner = named(variablesOf(outer.variablesReference), "inner");
    ASSERT_TRUE(inner);
    ASSERT_GT(inner->variablesReference, 0);
    const auto count = named(variablesOf(inner->variablesReference), "count");
    ASSERT_TRUE(count);
    EXPECT_EQ(count->value, field(expected.stdoutBytes, "outer.inner.count"));
}

TEST_F(DapReal, StructChildrenAreItsMembers) {
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    const dap::Variable outer = local("outer");
    const VariableList children = variablesOf(outer.variablesReference);
    for (const std::string_view name : {"inner", "values", "label"}) {
        EXPECT_TRUE(named(children, name).has_value()) << name;
    }
}

TEST_F(DapReal, ArrayMemberElementMatchesTheProgram) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    const auto values = named(variablesOf(local("outer").variablesReference), "values");
    ASSERT_TRUE(values);
    const auto third = named(variablesOf(values->variablesReference), "[2]");
    ASSERT_TRUE(third) << report();
    EXPECT_EQ(third->value, field(expected.stdoutBytes, "outer.values[2]"));
}

TEST_F(DapReal, CharPointerMemberShowsItsText) {
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    const auto label = named(variablesOf(local("outer").variablesReference), "label");
    ASSERT_TRUE(label);
    const std::string printed = evalValue("(const char*)outer.label");
    EXPECT_TRUE(contains(label->value, "\"frost\"")) << label->value;
    EXPECT_TRUE(contains(printed, "\"frost\"")) << printed;
}

TEST_F(DapReal, StdStringShowsItsText) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    EXPECT_TRUE(contains(local("text").value, "\"" + field(expected.stdoutBytes, "text") + "\""))
        << local("text").value;
}

TEST_F(DapReal, StdVectorShowsItsSizeOrSaysItCannot) {
    const io::ProcessResult expected = oracle({"values"});
    const std::string size = field(expected.stdoutBytes, "numbers.size");
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    const dap::Variable numbers = local("numbers");
#ifdef __linux__
    // No Python in the pinned Linux lldb-dap (lldb_dap.hpp, "Values on Linux"): an honest summary,
    // never lldb's wrong "size=0".
    EXPECT_TRUE(contains(numbers.value, "not shown")) << numbers.value;
    EXPECT_FALSE(contains(numbers.value, "size=")) << numbers.value;
#else
    EXPECT_EQ(numbers.value, "size=" + size);
    EXPECT_EQ(std::to_string(numbers.indexedVariables), size);
#endif
}

TEST_F(DapReal, StdVectorElementsMatchTheProgram) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    const dap::Variable numbers = local("numbers");
    const VariableList children =
        numbers.variablesReference > 0 ? variablesOf(numbers.variablesReference) : VariableList{};
    const auto last = named(children, "[5]");
#ifdef __linux__
    // Without lldb's Python formatters (DBG-4a) lldb-dap may list the raw members instead of the
    // elements; it must never list elements that are not the program's.
    if (last) {
        EXPECT_EQ(last->value, field(expected.stdoutBytes, "numbers[5]"));
    }
    EXPECT_FALSE(named(children, "[6]").has_value());
#else
    ASSERT_TRUE(last) << report();
    EXPECT_EQ(last->value, field(expected.stdoutBytes, "numbers[5]"));
#endif
}

TEST_F(DapReal, StdVectorElementsThroughEvaluate) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    EXPECT_EQ(evalValue("numbers[5]"), field(expected.stdoutBytes, "numbers[5]"));
}

// --- Evaluate and set variable ---

TEST_F(DapReal, EvaluateComputesFromLocals) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    const auto parsedCount = oracleInteger(field(expected.stdoutBytes, "outer.inner.count"));
    ASSERT_TRUE(parsedCount);
    const int count = *parsedCount;
    const auto parsedThird = oracleInteger(field(expected.stdoutBytes, "outer.values[2]"));
    ASSERT_TRUE(parsedThird);
    const int third = *parsedThird;
    EXPECT_EQ(evalValue("outer.inner.count + outer.values[2]"), std::to_string(count + third));
}

TEST_F(DapReal, EvaluateInTheReplContext) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    auto value = eval("outer.inner.count", "repl");
    ASSERT_TRUE(value) << value.error().describe();
    EXPECT_TRUE(value->value.ends_with("= " + field(expected.stdoutBytes, "outer.inner.count"))) << value->value;
}

TEST_F(DapReal, EvaluateOfAnUnknownNameFails) {
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    auto value = eval("frost_no_such_variable_anywhere + 1", "watch");
    EXPECT_FALSE(value.has_value());
}

TEST_F(DapReal, EvaluateOfAStructGivesChildren) {
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    auto value = eval("outer.inner", "watch");
    ASSERT_TRUE(value) << value.error().describe();
    ASSERT_GT(value->variablesReference, 0);
    EXPECT_TRUE(named(variablesOf(value->variablesReference), "ratio").has_value());
}

TEST_F(DapReal, SetVariableChangesWhatTheProgramPrints) {
    const io::ProcessResult expected = oracle({"setvar"});
    const std::string original = field(expected.stdoutBytes, "changed");
    const auto parsedOriginal = oracleInteger(original);
    ASSERT_TRUE(parsedOriginal);
    const std::string changed = std::to_string(*parsedOriginal + 72);
    launch({"setvar"}, {at("setvar")});
    ASSERT_TRUE(waitStop(1));
    const auto scope = localsScope();
    ASSERT_TRUE(scope);
    auto set = await<dap::Variable>(
        [&](const auto& done) { m_session->setVariable(scope->variablesReference, "changed", changed, done); });
    ASSERT_TRUE(set) << set.error().describe();
    ASSERT_TRUE(continueToExit());
    EXPECT_EQ(field(output(), "changed"), changed) << report();
}

TEST_F(DapReal, SetVariableAnswersWithTheNewValue) {
    launch({"setvar"}, {at("setvar")});
    ASSERT_TRUE(waitStop(1));
    const auto scope = localsScope();
    ASSERT_TRUE(scope);
    auto set = await<dap::Variable>(
        [&](const auto& done) { m_session->setVariable(scope->variablesReference, "changed", "1234", done); });
    ASSERT_TRUE(set) << set.error().describe();
    EXPECT_EQ(set->value, "1234");
    EXPECT_EQ(local("changed").value, "1234");
}

TEST_F(DapReal, AssignmentThroughEvaluateReleasesTheProgram) {
    launch({"spin"}, {at("spin")});
    ASSERT_TRUE(waitStop(1));
    ASSERT_TRUE(eval("g_release = 1")) << report();
    m_session->setBreakpoints(m_sourcePath, {});
    // spin's documented contract: 0 when released, 3 when it timed out.
    EXPECT_EQ(continueToExit(), 0);
}

// --- Threads ---

TEST_F(DapReal, ThreadsListTheWorkers) {
    launch({"threads"}, {at("worker")});
    const auto stop = waitStop(1);
    ASSERT_TRUE(stop && stop->threadId);
    ASSERT_TRUE(test::waitUntil([&] { return m_session->threads().size() >= 2; }, kWait));
    const auto threads = m_session->threads();
    EXPECT_TRUE(std::ranges::any_of(threads, [&](const DebugThread& t) { return t.id == *stop->threadId; }));
}

TEST_F(DapReal, EachWorkerStopsOnItsOwnThread) {
    const io::ProcessResult expected = oracle({"threads"});
    launch({"threads"}, {at("worker")});
    const auto first = waitStop(1);
    ASSERT_TRUE(first && first->threadId);
    ASSERT_TRUE(m_session->continueRun());
    const auto second = waitStop(2);
    ASSERT_TRUE(second && second->threadId);
    EXPECT_NE(*first->threadId, *second->threadId);
    m_session->setBreakpoints(m_sourcePath, {});
    EXPECT_EQ(continueToExit(), expected.exitCode);
    EXPECT_EQ(field(output(), "counter"), field(expected.stdoutBytes, "counter"));
}

TEST_F(DapReal, SelectingAnotherThreadLoadsItsFrames) {
    launch({"threads"}, {at("worker")});
    const auto stop = waitStop(1);
    ASSERT_TRUE(stop && stop->threadId);
    ASSERT_TRUE(test::waitUntil([&] { return m_session->threads().size() >= 2; }, kWait));
    std::optional<std::int64_t> other;
    for (const DebugThread& thread : m_session->threads()) {
        if (thread.id != *stop->threadId) {
            other = thread.id;
            break;
        }
    }
    ASSERT_TRUE(other);
    ASSERT_TRUE(m_session->selectThread(*other));
    ASSERT_TRUE(test::waitUntil([&] { return !m_session->frames(*other).frames.empty(); }, kWait)) << report();
    EXPECT_EQ(m_session->selectedThread(), other);
}

TEST_F(DapReal, WorkerStackShowsTheWorkerFunction) {
    launch({"threads"}, {at("worker")});
    ASSERT_TRUE(waitStop(1));
    EXPECT_TRUE(contains(frame().name, "worker_body")) << frame().name;
    EXPECT_EQ(frame().line, line("worker"));
}

// --- Pause and continue ---

TEST_F(DapReal, PauseStopsARunningProgramWithReasonPause) {
    launchSpinning();
    ASSERT_TRUE(m_session->pause());
    const auto stop = waitStop(1);
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->reason, "pause") << report();
    EXPECT_FALSE(stop->isCrash());
}

TEST_F(DapReal, PauseSelectsTheProgramsSourceFrame) {
    launchSpinning();
    ASSERT_TRUE(m_session->pause());
    ASSERT_TRUE(waitStop(1));
    // A frame with source on the program's thread (it may be a library header's, such as <thread>),
    // never the break-in thread Windows injects for the pause.
    EXPECT_TRUE(frame().hasSource() && !frame().subtle) << frame().name << "\n" << report();
    const std::vector<DebugFrame> frames = stack();
    EXPECT_TRUE(std::ranges::any_of(frames, [&](const DebugFrame& f) {
        return contains(f.name, "spin_until_released") && samePath(f.path, m_sourcePath);
    })) << report();
}

TEST_F(DapReal, PauseThenContinueThenPauseAgain) {
    launchSpinning();
    ASSERT_TRUE(m_session->pause());
    ASSERT_TRUE(waitStop(1));
    ASSERT_TRUE(m_session->continueRun());
    ASSERT_TRUE(test::waitUntil([&] { return m_session->state() == DebugState::Running; }, kWait));
    ASSERT_TRUE(m_session->pause());
    const auto stop = waitStop(2);
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->reason, "pause");
}

TEST_F(DapReal, PauseThenReleaseRunsToTheExit) {
    launchSpinning();
    ASSERT_TRUE(m_session->pause());
    ASSERT_TRUE(waitStop(1));
    ASSERT_TRUE(eval("g_release = 1")) << report();
    EXPECT_EQ(continueToExit(), 0);
}

TEST_F(DapReal, ContinueFromABreakpointRunsToTheExit) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    EXPECT_EQ(continueToExit(), expected.exitCode);
    EXPECT_EQ(field(output(), "total"), field(expected.stdoutBytes, "total"));
}

// --- Terminate, disconnect, kill ---

TEST_F(DapReal, ExplicitPidBoundTerminationAfterRealPauseAndContinueEndsTheObservedProcess) {
    launchSpinning();
    ASSERT_TRUE(test::waitUntil([&] { return m_session->processId().has_value(); }, kWait)) << report();
    const auto pid = *m_session->processId();
    EXPECT_TRUE(processAlive(pid));
    auto wrong = m_session->terminateDebuggee(pid + 1);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code(), ErrorCode::PermissionDenied);
    ASSERT_TRUE(m_session->pause()) << report();
    ASSERT_TRUE(waitStop(1)) << report();
    EXPECT_EQ(m_session->state(), DebugState::Stopped);
    EXPECT_EQ(m_session->processId(), pid);
    ASSERT_TRUE(m_session->continueRun()) << report();
    ASSERT_TRUE(test::waitUntil([&] { return m_session->state() == DebugState::Running; }, kWait)) << report();
    ASSERT_TRUE(m_session->pause()) << report();
    ASSERT_TRUE(waitStop(2)) << report();
    ASSERT_TRUE(m_session->canTerminateDebuggee()) << report();
    ASSERT_TRUE(m_session->terminateDebuggee(pid)) << report();
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome) << report();
    EXPECT_TRUE(outcome->stopped);
    EXPECT_TRUE(test::waitUntil([&] { return !processAlive(pid); }, kWait)) << "the observed native target still runs";
}

TEST_F(DapReal, StopWhileStoppedEndsTheSessionAndTheProgram) {
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    const auto pid = m_session->processId();
    ASSERT_TRUE(pid);
    m_session->stop();
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->stopped);
    EXPECT_TRUE(test::waitUntil([&] { return !processAlive(*pid); }, kWait)) << "the debuggee still runs";
}

TEST_F(DapReal, StopWhileRunningEndsTheSessionAndTheProgram) {
    launch({"spin"});
    ASSERT_TRUE(test::waitUntil([&] { return m_session->processId().has_value(); }, kWait)) << report();
    const std::int64_t pid = *m_session->processId();
    ASSERT_TRUE(processAlive(pid));
    m_session->stop();
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->stopped);
    EXPECT_TRUE(test::waitUntil([&] { return !processAlive(pid); }, kWait)) << "the debuggee still runs";
}

TEST_F(DapReal, StopConfirmsTheDisconnect) {
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    m_session->stop();
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->disconnectConfirmed) << report();
    EXPECT_FALSE(outcome->error.has_value());
}

TEST_F(DapReal, KillEndsTheSessionAndTheProgram) {
    launch({"spin"});
    ASSERT_TRUE(test::waitUntil([&] { return m_session->processId().has_value(); }, kWait)) << report();
    const std::int64_t pid = *m_session->processId();
    m_session->kill();
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->stopped);
    EXPECT_TRUE(test::waitUntil([&] { return !processAlive(pid); }, kWait)) << "the debuggee still runs";
}

// --- Restart ---

TEST_F(DapReal, RestartRunsAFreshProcessToTheBreakpointAgain) {
    const io::ProcessResult expected = oracle({"values"});
    launch({"values"}, {at("values")});
    ASSERT_TRUE(waitStop(1));
    ASSERT_TRUE(test::waitUntil([&] { return m_session->processId().has_value(); }, kWait));
    const std::int64_t first = *m_session->processId();
    waitCapabilities([](const dap::Capabilities& caps) { return caps.supportsRestartRequest; });
    ASSERT_TRUE(m_session->restart()) << report();
    const auto stop = waitStop(2);
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->reason, "breakpoint");
    EXPECT_EQ(frame().line, line("values"));
    ASSERT_TRUE(test::waitUntil([&] { return m_session->processId().has_value(); }, kWait));
    EXPECT_NE(*m_session->processId(), first);
    EXPECT_EQ(continueToExit(), expected.exitCode);
}

TEST_F(DapReal, RestartIsRefusedForAnAttachedProgram) {
    io::ProcessSpec spec;
    spec.executable = m_program;
    spec.arguments = {"wait"};
    auto process = io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(process) << process.error().describe();
    attach(static_cast<std::int64_t>((*process)->pid()));
    ASSERT_TRUE(test::waitUntil([&] { return m_session->state() != DebugState::Starting; }, kWait)) << report();
    const auto restarted = m_session->restart();
    ASSERT_FALSE(restarted.has_value());
    EXPECT_EQ(restarted.error().code(), ErrorCode::Unsupported);
    m_session->kill();
    static_cast<void>((*process)->terminate());
}

// --- Attach ---

TEST_F(DapReal, AttachStopsTheRunningProgramAtABreakpoint) {
    io::ProcessSpec spec;
    spec.executable = m_program;
    spec.arguments = {"wait"};
    auto process = io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(process) << process.error().describe();
    attach(static_cast<std::int64_t>((*process)->pid()), {at("spin")});
    const auto stop = waitStop(1);
    ASSERT_TRUE(stop) << report();
    EXPECT_EQ(frame().line, line("spin"));
    m_session->kill();
    static_cast<void>((*process)->terminate());
}

TEST_F(DapReal, AttachThenReleaseAndDisconnectLetsTheProgramFinish) {
    io::ProcessSpec spec;
    spec.executable = m_program;
    spec.arguments = {"wait"};
    auto process = io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(process) << process.error().describe();
    attach(static_cast<std::int64_t>((*process)->pid()), {at("spin")});
    ASSERT_TRUE(waitStop(1));
    ASSERT_TRUE(eval("g_release = 1")) << report();
    m_session->setBreakpoints(m_sourcePath, {});
    m_session->stop();
    ASSERT_TRUE(waitEnd());
    auto status = (*process)->wait(std::chrono::seconds(90));
    ASSERT_TRUE(status) << status.error().describe();
    EXPECT_EQ(status->exitCode, 0); // spin's contract: released.
}

TEST_F(DapReal, AttachDisconnectLeavesTheProgramRunning) {
    io::ProcessSpec spec;
    spec.executable = m_program;
    spec.arguments = {"wait"};
    auto process = io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(process) << process.error().describe();
    const auto pid = static_cast<std::int64_t>((*process)->pid());
    attach(pid);
    ASSERT_TRUE(
        test::waitUntil([&] { return m_session->state() == DebugState::Running || m_recorder.hasEnded(); }, kWait))
        << report();
    m_session->stop();
    ASSERT_TRUE(waitEnd());
    EXPECT_TRUE(processAlive(pid));
    auto status = (*process)->wait(std::chrono::milliseconds(0));
    EXPECT_FALSE(status.has_value()) << "the attached program ended with the session";
    static_cast<void>((*process)->terminate());
}

TEST_F(DapReal, AttachToAMissingProcessFailsAndSaysWhy) {
    // A process id that names no process: one that ran and was waited for.
    io::ProcessSpec spec;
    spec.executable = m_program;
    spec.arguments = {"exit", "0"};
    auto process = io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(process) << process.error().describe();
    const auto pid = static_cast<std::int64_t>((*process)->pid());
    ASSERT_TRUE((*process)->wait(std::chrono::seconds(60)));
    process->reset();
    ASSERT_FALSE(processAlive(pid));
    attach(pid);
    const auto outcome = waitEnd();
    ASSERT_TRUE(outcome);
    ASSERT_TRUE(outcome->error.has_value()) << report();
    EXPECT_TRUE(contains(outcome->error->message(), "Could not attach")) << outcome->error->message();
    const std::string hint = ptraceAttachHint(io::realFileSystem());
    if (!hint.empty()) {
        EXPECT_TRUE(contains(outcome->error->message(), hint)) << outcome->error->message();
    }
}

} // namespace
} // namespace frost::debug
