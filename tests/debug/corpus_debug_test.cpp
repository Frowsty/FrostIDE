// SPDX-License-Identifier: Apache-2.0
//
// The corpus debug test (DBG-1 and DBG-2a acceptance; label "integration", tags "corpus" and "lldb";
// Windows only, see tests/debug/CMakeLists.txt): FrostEditor of the pinned FrostEngine clone, built
// with `cmake --build --target FrostEditor` under the corpus build lock, debugged through DebugSession
// and the pinned lldb-dap as the product launches it.
/*
 * What it checks. A line breakpoint in an engine .cpp on FrostEditor's start-up path
 * (Frost/src/Frost/Platform/Windows/WindowsPlatform.cpp, GetCommandLineArgsUtf8, which main() calls
 * before anything else runs) is hit; the call stack and the locals come from lldb-dap; step over,
 * into and out move as the source says; a conditional breakpoint stops on its one true iteration and
 * a logpoint writes its message without stopping; the program then exits as an undebugged run does.
 *
 * Oracles. Lines come from the engine's own source text (searched for the statements named below,
 * never line numbers); argument counts and values from the command line the test passes; the exit
 * code from the same command run without a debugger. The corpus path comes from $FROST_CORPORA_ROOT
 * (tools\verify.ps1 -Corpus sets it), the pin and preset from tests/corpus/corpora.lock.json and the
 * MSVC toolset from sdk/sdk.lock.json, compiled in by CMake. A missing corpus, lldb-dap or toolset
 * fails the test; nothing is skipped.
 */

#include "frost/build/cmake_toolchain.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/debug/debug_session.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "debug_test_util.hpp"
#include "support/crash_dialogs.hpp"
#include "support/engine_lock.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef FROST_CORPUS_FROSTENGINE_SHA
#error "FROST_CORPUS_FROSTENGINE_SHA comes from tests/debug/CMakeLists.txt"
#endif

namespace frost::debug {
namespace {

using namespace std::chrono_literals;

// The first stop waits for lldb to read FrostEditor's PDB (over a gigabyte in Debug); later events
// come from a loaded target.
constexpr auto kFirstStop = 20min;
constexpr auto kWait = 5min;

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// Two absolute Windows paths name one file (case and separators aside).
bool samePath(std::string_view a, std::string_view b) {
    const auto normal = [](std::string_view path) {
        std::string out = io::normalizePath(path).value_or(std::string(path));
        std::ranges::transform(out, out.begin(), [](char c) {
            return c == '/' ? '\\' : (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        });
        return out;
    };
    return normal(a) == normal(b);
}

// The 1-based number of the first line after line `after` that contains `needle`; 0 when none does.
std::int64_t lineOf(std::string_view text, std::string_view needle, std::int64_t after = 0) {
    std::int64_t number = 1;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        if (number > after && contains(text.substr(start, end - start), needle)) {
            return number;
        }
        start = end + 1;
        ++number;
    }
    return 0;
}

std::string readOrFail(const std::string& path) {
    auto text = io::realFileSystem().readFile(path);
    if (!text) {
        ADD_FAILURE() << path << ": " << text.error().describe();
        return {};
    }
    return std::move(*text);
}

// $FROST_CORPORA_ROOT ("" when unset). Windows only, as this test is.
std::string corporaRoot() {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "FROST_CORPORA_ROOT") != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
}

// <root>/frostengine-<sha12> from $FROST_CORPORA_ROOT, checked to be the pinned clone; "" (with a
// failure) otherwise, as tests/app/corpus_test.cpp does.
std::string checkedCorpus() {
    const std::string root = corporaRoot();
    if (root.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is not set; run the corpus tier through tools\\verify.ps1 -Corpus, "
                         "which sets it from tools/corpora.py root";
        return {};
    }
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    const std::string corpus = io::normalizePath(root + "/frostengine-" + sha.substr(0, 12)).value_or("");
    const auto head = io::realFileSystem().readFile(corpus + "/.git/HEAD");
    if (!head || head->substr(0, 40) != sha) {
        ADD_FAILURE() << corpus << " is not the pinned FrostEngine clone (python tools/corpora.py fetch, verify)";
        return {};
    }
    return corpus;
}

// The vcvarsall environment of the pinned MSVC toolset (as tools/corpora.py's msvc_environment).
Result<project::CMakeEnvironment> pinnedMsvcEnvironment() {
    auto& fs = io::realFileSystem();
    auto& launcher = io::realProcessLauncher();
    auto instances = build::listVsInstances(launcher, fs);
    if (!instances) {
        return std::unexpected(std::move(instances.error()));
    }
    const std::string pinned = FROST_PINNED_VC_TOOLS;
    for (const build::VsInstance& instance : *instances) {
        if (!fs.exists(instance.installationPath + "\\VC\\Tools\\MSVC\\" + pinned + "\\include").value_or(false)) {
            continue;
        }
        auto captured = build::captureMsvcEnvironment(launcher, fs, instance, "x64");
        if (!captured) {
            return std::unexpected(std::move(captured.error()));
        }
        // vcvarsall picks the instance's default toolset; the build must use the pinned one.
        const auto version = std::ranges::find_if(
            captured->changes, [](const auto& entry) { return _stricmp(entry.first.c_str(), "VCToolsVersion") == 0; });
        if (version == captured->changes.end() || !version->second || !contains(*version->second, pinned)) {
            return fail(ErrorCode::NotFound,
                        std::format("{}'s default MSVC toolset is not the pinned {}", instance.displayName, pinned));
        }
        return std::move(captured->changes);
    }
    return fail(ErrorCode::NotFound, std::format("no Visual Studio instance has the pinned MSVC toolset {}", pinned));
}

class CorpusFrostEditor : public testing::Test {
protected:
    void TearDown() override {
        if (m_session) {
            m_session->kill(); // lldb-dap's exit ends the debuggee (Windows kills it with its debugger).
            m_session.reset();
        }
    }

    std::string report() const {
        LockGuard lock(m_recorder.mutex);
        std::string text = "events: " + testing::PrintToString(m_recorder.events) + "\nbreakpoints:";
        for (const BreakpointResult& result : m_recorder.breakpoints) {
            text += std::format(" [{} verified={} line={} '{}']", result.id, result.verified, result.line.value_or(0),
                                result.message);
        }
        for (const StopInfo& stop : m_recorder.stops) {
            text += std::format("\nstop: {} '{}' '{}'", stop.reason, stop.description, stop.text);
        }
        text += "\noutput: " + m_recorder.output.substr(0, 4000);
        if (m_recorder.ended && m_recorder.ended->error) {
            text += "\nerror: " + m_recorder.ended->error->describe();
        }
        return text;
    }

    bool waitStop(std::size_t count, std::chrono::milliseconds timeout = kWait) {
        const bool ok = test::waitUntil(
            [&] {
                return m_recorder.hasEnded() ||
                       (m_recorder.stopCount() >= count && m_session->state() == DebugState::Stopped &&
                        m_session->selectedFrame().has_value());
            },
            timeout);
        if (!ok || m_recorder.stopCount() < count || !m_session->selectedFrame()) {
            ADD_FAILURE() << "no stop " << count << (ok ? " (the session ended)" : " (timed out)") << "\n" << report();
            return false;
        }
        return true;
    }

    DebugFrame frame() const { return m_session->selectedFrame().value_or(DebugFrame{}); }

    std::vector<DebugFrame> stack() const {
        const auto thread = m_session->selectedThread();
        return thread ? m_session->frames(*thread).frames : std::vector<DebugFrame>{};
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

    // The names in the selected frame's Locals scope.
    std::vector<std::string> localNames() {
        auto scopes = await<ScopeList>([&](auto done) { m_session->scopes(frame().id, done); });
        if (!scopes) {
            ADD_FAILURE() << scopes.error().describe();
            return {};
        }
        const auto locals = std::ranges::find_if(*scopes, [](const dap::Scope& scope) {
            return scope.presentationHint == "locals" || scope.name == "Locals";
        });
        if (locals == scopes->end()) {
            ADD_FAILURE() << "no Locals scope";
            return {};
        }
        auto list =
            await<VariableList>([&](auto done) { m_session->variables(locals->variablesReference, 0, 0, done); });
        if (!list) {
            ADD_FAILURE() << list.error().describe();
            return {};
        }
        std::vector<std::string> names;
        for (const dap::Variable& variable : *list) {
            names.push_back(variable.name);
        }
        return names;
    }

    std::string evalValue(std::string expression) {
        auto value = await<dap::Variable>(
            [&](auto done) { m_session->evaluate(std::move(expression), frame().id, "watch", done); });
        if (!value) {
            ADD_FAILURE() << value.error().describe();
            return {};
        }
        return value->value;
    }

    test::SessionRecorder m_recorder;
    std::unique_ptr<DebugSession> m_session;
};

TEST_F(CorpusFrostEditor, StartupBreakpointsStepsLocalsStackConditionAndLogpointComeFromLldbDap) {
    frost::test::disableCrashDialogs(); // The compilers and FrostEditor inherit the error mode.
    const std::string corpus = checkedCorpus();
    ASSERT_FALSE(corpus.empty());
    auto adapter = findLldbDap(io::realFileSystem(), defaultLldbDapSearch());
    ASSERT_TRUE(adapter.has_value()) << "the pinned lldb-dap is required: " << adapter.error().message();

    // Built in place, so under the corpus build lock (tools/corpora.py BuildLock, tests/journeys).
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    const frost::test::EngineBuildLock buildLock(corporaRoot() + "/frostengine-" + sha.substr(0, 12) + ".build.lock");
    ASSERT_TRUE(buildLock.held()) << "the corpus build lock stayed taken (tools/corpora.py BuildLock)";
    const std::string buildDir = corpus + "\\build\\" + FROST_CORPUS_FROSTENGINE_PRESET;
    ASSERT_TRUE(io::realFileSystem().exists(buildDir + "\\CMakeCache.txt").value_or(false))
        << buildDir << " is not configured (python tools/corpora.py engine --configure)";
    auto msvc = pinnedMsvcEnvironment();
    ASSERT_TRUE(msvc.has_value()) << msvc.error().describe();
    io::ProcessSpec buildSpec;
    buildSpec.executable = io::normalizePath(FROST_TEST_CMAKE).value_or("");
    buildSpec.arguments = {"--build", buildDir, "--target", "FrostEditor"};
    buildSpec.workingDirectory = corpus;
    buildSpec.environment = *msvc;
    auto built = io::runToCompletion(io::realProcessLauncher(), buildSpec, {}, 3h);
    ASSERT_TRUE(built.has_value()) << built.error().describe();
    ASSERT_EQ(built->exitCode, 0) << built->stdoutBytes.substr(
                                         built->stdoutBytes.size() > 8000 ? built->stdoutBytes.size() - 8000 : 0)
                                  << built->stderrBytes;
    const std::string program = buildDir + "\\bin\\FrostEditor.exe";
    ASSERT_TRUE(io::realFileSystem().exists(program).value_or(false)) << program;

    // The engine source the breakpoints go into, and the editor's main() that calls it.
    const std::string platformPath = corpus + "\\Frost\\src\\Frost\\Platform\\Windows\\WindowsPlatform.cpp";
    const std::string mainPath = corpus + "\\Frost-Editor\\src\\EditorApp.cpp";
    const std::string platform = readOrFail(platformPath);
    const std::string mainSource = readOrFail(mainPath);
    const std::int64_t function = lineOf(platform, "std::vector<std::string> GetCommandLineArgsUtf8(");
    ASSERT_GT(function, 0);
    const std::int64_t toArgv = lineOf(platform, "::CommandLineToArgvW(", function);
    const std::int64_t nullCheck = lineOf(platform, "if (wideArgs == nullptr)", toArgv);
    const std::int64_t pushBack = lineOf(platform, "args.push_back(Utf8::FromWide(wideArgs[i]));", nullCheck);
    const std::int64_t freeArgs = lineOf(platform, "::LocalFree(wideArgs);", pushBack);
    const std::int64_t mainCall = lineOf(mainSource, "Frost::Platform::GetCommandLineArgsUtf8(argc, argv)");
    ASSERT_TRUE(toArgv > 0 && nullCheck > 0 && pushBack > 0 && freeArgs > 0 && mainCall > 0)
        << "the pinned engine source changed shape; update the statements this test searches for";

    // FrostEditor's CLI help runs main() to its exit without a window; the oracle is the same run
    // without a debugger.
    const std::vector<std::string> arguments = {"help"};
    io::ProcessSpec oracleSpec;
    oracleSpec.executable = program;
    oracleSpec.arguments = arguments;
    auto expected = io::runToCompletion(io::realProcessLauncher(), oracleSpec, {}, 5min);
    ASSERT_TRUE(expected.has_value()) << expected.error().describe();
    const std::string argumentCount = std::to_string(arguments.size() + 1); // argv[0] and the arguments.
    const std::string lastIndex = std::to_string(arguments.size());

    BreakpointLine conditional{.id = 2, .line = pushBack};
    conditional.condition = "i == " + lastIndex;
    BreakpointLine logpoint{.id = 3, .line = freeArgs};
    logpoint.logMessage = "frost-corpus-wideCount={wideCount}";
    std::map<std::string, std::vector<BreakpointLine>> breakpoints;
    breakpoints.emplace(platformPath, std::vector<BreakpointLine>{{.id = 1, .line = toArgv}, conditional, logpoint});
    DebugTarget target{.name = "FrostEditor", .program = program};
    target.arguments = arguments;
    target.workingDirectory = buildDir + "\\bin";
    m_session = DebugSession::start(lldbDapAdapterFactory(io::realProcessLauncher(), *adapter),
                                    lldbDapLaunchArguments(target), std::move(breakpoints), m_recorder.callbacks(),
                                    lldbDapSessionOptions(DebugTargetMode::Launch));

    // 1. The line breakpoint, with the stack and the locals from lldb-dap.
    ASSERT_TRUE(waitStop(1, kFirstStop));
    {
        LockGuard lock(m_recorder.mutex);
        EXPECT_EQ(m_recorder.stops.front().reason, "breakpoint");
    }
    EXPECT_TRUE(contains(frame().name, "GetCommandLineArgsUtf8")) << frame().name;
    EXPECT_TRUE(samePath(frame().path, platformPath)) << frame().path;
    EXPECT_EQ(frame().line, toArgv);
    const std::vector<DebugFrame> frames = stack();
    EXPECT_TRUE(std::ranges::any_of(frames,
                                    [&](const DebugFrame& caller) {
                                        return contains(caller.name, "main") && samePath(caller.path, mainPath) &&
                                               caller.line == mainCall;
                                    }))
        << "main() of EditorApp.cpp calls it at line " << mainCall;
    const std::vector<std::string> names = localNames();
    for (const char* name : {"wideCount", "wideArgs", "args"}) {
        EXPECT_NE(std::ranges::find(names, name), names.end()) << name << " in " << testing::PrintToString(names);
    }
    EXPECT_EQ(evalValue("argc"), argumentCount);

    // 2. Step over CommandLineToArgvW: the next statement, with its result.
    ASSERT_TRUE(m_session->next());
    ASSERT_TRUE(waitStop(2));
    EXPECT_EQ(frame().line, nullCheck);
    EXPECT_EQ(evalValue("wideCount"), argumentCount);

    // 3. The conditional breakpoint stops on its one true iteration.
    ASSERT_TRUE(m_session->continueRun());
    ASSERT_TRUE(waitStop(3));
    EXPECT_EQ(frame().line, pushBack);
    EXPECT_EQ(evalValue("i"), lastIndex);
    EXPECT_TRUE(contains(evalValue("wideArgs[" + lastIndex + "]"), arguments.back()));
    const std::size_t depth = stack().size();

    // 4. Step into the call the line makes (std:: helpers are stepped through), then out again.
    ASSERT_TRUE(m_session->stepIn());
    ASSERT_TRUE(waitStop(4));
    EXPECT_TRUE(contains(frame().name, "FromWide")) << frame().name;
    EXPECT_EQ(stack().size(), depth + 1);
    ASSERT_TRUE(m_session->stepOut());
    ASSERT_TRUE(waitStop(5));
    EXPECT_TRUE(contains(frame().name, "GetCommandLineArgsUtf8")) << frame().name;
    EXPECT_EQ(frame().line, pushBack);
    EXPECT_EQ(stack().size(), depth);

    // 5. The logpoint writes without stopping; the run ends as the undebugged one did.
    ASSERT_TRUE(m_session->continueRun());
    ASSERT_TRUE(test::waitUntil([&] { return m_recorder.hasEnded(); }, kWait)) << report();
    const auto outcome = m_session->outcome();
    ASSERT_TRUE(outcome && outcome->exitCode) << report();
    EXPECT_EQ(*outcome->exitCode, expected->exitCode);
    EXPECT_EQ(m_recorder.stopCount(), 5U) << report();
    LockGuard lock(m_recorder.mutex);
    EXPECT_TRUE(contains(m_recorder.output, "frost-corpus-wideCount=" + argumentCount)) << m_recorder.output;
}

} // namespace
} // namespace frost::debug
