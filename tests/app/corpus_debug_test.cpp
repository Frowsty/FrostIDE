// SPDX-License-Identifier: Apache-2.0
//
// UAT-1 steps 5 and 6 on the pinned FrostEngine corpus (label integration corpus; verify -Corpus):
// FrostEditor debugged through the workbench's own Debug path (Run/Debug configuration, Debug on the
// toolbar, DebugController, the Breakpoints tab, the pinned lldb-dap found as the product finds it)
// in a WorkbenchRig that opened and configured the clone. tests/debug/corpus_debug_test.cpp drives
// DebugSession directly on an engine source; this test drives the product's composition instead.
/*
 * What it checks, in one debug session of FrostEditor (arguments --offscreen --no-automation
 * --config-dir <scratch>, edited into its run configuration as the Run/Debug Configurations dialog
 * does, so no window opens):
 *   1. a line breakpoint in FrostEditor's main() (Frost-Editor/src/EditorApp.cpp, the line that
 *      calls GetCommandLineArgsUtf8) stops there; the call stack's top frames, the Locals and argc
 *      are lldb's;
 *   2. Step Over, Step Into and Step Out each land where lldb itself lands;
 *   3. with a conditional breakpoint whose condition is false, a logpoint and a conditional
 *      breakpoint whose condition is true further down main(), Resume stops only at the true one,
 *      and the logpoint's message (with its interpolated argc) is in the debug console;
 *   4. the C++ throw row of the Breakpoints tab, checked, stops at the first throw of FrostEditor's
 *      start: the scratch config directory holds a Preferences.json that is not JSON, so
 *      EditorPreferences::Load's JSON parse throws (and catches) as it does for any user whose file
 *      is broken; no corpus source changes;
 *   5. Stop ends the debuggee.
 *
 * Oracles. The lines come from the source text (the statements searched for below, never line
 * numbers). Where the debugger lands comes from the pinned lldb CLI (lldb-<pin>/bin beside the pinned
 * lldb-dap), run in batch mode on the same FrostEditor with the same arguments and an equal scratch
 * directory: its stop line, `bt`, `frame variable`, `next`, `step` (with MSVC's incremental linking
 * lldb's step-in stops in the callee's one-jmp thunk, which has no source; one `thread step-inst`
 * then enters the callee, which is where lldb's own stepping reaches it), `finish`, and the stack at
 * its first C++ throw (MSVC:
 * _CxxThrowException; Itanium: `breakpoint set -E c++`). The conditions' truth comes from the argc
 * lldb printed. A missing corpus, lldb-dap or lldb fails the test; nothing is skipped.
 *
 * The clone. FrostEditor is built in the shared build tree under the corpus build lock (on a tree
 * where it is current this is Ninja's no-op). The workbench writes the clone's .frost/ (breakpoints,
 * run configurations); a guard restores it, so the clone is git-clean at the end.
 */

#include "frost/build/cmake_service.hpp"
#include "frost/debug/debug_session.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_types.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_tool_window.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/vcs/git_locator.hpp"

#include "support/crash_dialogs.hpp"
#include "support/engine_lock.hpp"
#include "support/scratch_dir.hpp"
#include "workbench_rig.hpp"
#include <QTreeWidget>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
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

namespace {

using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

constexpr std::string_view kTarget = "FrostEditor";
// The first stop waits for lldb to read FrostEditor's debug information (over a gigabyte in Debug).
constexpr auto kFirstStop = 30min;
constexpr auto kWait = 5min;

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// <$FROST_CORPORA_ROOT>/frostengine-<sha12>, as corpus_doctest_test.cpp checks it; "" (with a
// failure added) when it is missing, so the corpus tier fails instead of skipping.
std::string pinnedCorpus() {
    const std::string root = qEnvironmentVariable("FROST_CORPORA_ROOT").toStdString();
    if (root.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is not set; run the corpus tier through tools\\verify.ps1 -Corpus or "
                         "tools/verify.sh --corpus";
        return {};
    }
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    const std::string corpus = frost::io::normalizePath(root + "/frostengine-" + sha.substr(0, 12)).value_or("");
    const auto head = frost::io::realFileSystem().readFile(corpus + "/.git/HEAD");
    if (!head || head->substr(0, 40) != sha) {
        ADD_FAILURE() << corpus << " is missing or not detached at its pin (python tools/corpora.py verify)";
        return {};
    }
    return corpus;
}

frost::io::ProcessResult runTool(const std::string& program, std::vector<std::string> arguments,
                                 const std::string& directory, std::chrono::seconds timeout = 10min) {
    frost::io::ProcessSpec spec;
    spec.executable = program;
    spec.arguments = std::move(arguments);
    spec.workingDirectory = directory;
    auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, timeout);
    EXPECT_TRUE(result.has_value()) << program << ": " << (result ? "" : result.error().describe());
    // A process that did not start never passes as a clean exit.
    return result ? std::move(*result) : frost::io::ProcessResult{.exitCode = -1};
}

std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> out;
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

// The 1-based number of the first line after line `after` that contains `needle`; 0 when none does.
int lineOf(const std::vector<std::string>& source, std::string_view needle, int after = 0) {
    for (int i = after; i < static_cast<int>(source.size()); ++i)
        if (contains(source[static_cast<std::size_t>(i)], needle))
            return i + 1;
    return 0;
}

// Two absolute paths name one file (case and separators aside on Windows).
bool samePath(std::string_view a, std::string_view b) {
    const auto normal = [](std::string_view path) {
        std::error_code error;
        std::string out = std::filesystem::weakly_canonical(
                              frost::io::toStdPath(path).value_or(std::filesystem::path(std::string(path))), error)
                              .generic_string();
#ifdef _WIN32
        std::ranges::transform(out, out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
#endif
        return out;
    };
    return normal(a) == normal(b);
}

// `path` with the platform's separators (FrostEditor's --config-dir as a user would type it).
std::string nativeSeparators(std::string path) {
#ifdef _WIN32
    std::ranges::replace(path, '/', '\\');
#endif
    return path;
}

// --- The lldb CLI oracle --------------------------------------------------------------------------

// One frame as the oracle's frame-format prints it ("FROSTFRAME|index|function|file|line").
struct OracleFrame {
    int index = -1;
    std::string function;
    std::string path;
    int line = 0;
};

constexpr std::string_view kFrameFormat =
    R"(settings set frame-format "FROSTFRAME|${frame.index}|${function.name}|${line.file.fullpath}|${line.number}\n")";

/*
 * The lldb batch run: `commands` in order (each with -o), then `--` and the program's command line.
 * section(i) is what command i printed: lldb echoes each command as "(lldb) <command>", so the
 * output between the i-th echo and the next one of these commands is its output (lldb's
 * asynchronous "(lldb) 1 location added" lines never equal one of them).
 */
class LldbRun {
public:
    LldbRun(const std::string& lldb, std::vector<std::string> commands, const std::string& program,
            const std::vector<std::string>& arguments, const std::string& directory)
        : m_commands(std::move(commands)) {
        std::vector<std::string> argv;
        for (const std::string& command : m_commands)
            argv.insert(argv.end(), {"-o", command});
        argv.insert(argv.end(), {"-b", "--", program});
        argv.insert(argv.end(), arguments.begin(), arguments.end());
        m_result = runTool(lldb, std::move(argv), directory, 30min);
        const std::vector<std::string> all = lines(m_result.stdoutBytes);
        std::size_t at = 0;
        std::vector<std::size_t> echoes;
        for (const std::string& command : m_commands) {
            while (at < all.size() && all[at] != "(lldb) " + command)
                ++at;
            echoes.push_back(at);
            if (at < all.size())
                ++at;
        }
        for (std::size_t i = 0; i < m_commands.size(); ++i) {
            std::vector<std::string> section;
            const std::size_t end = i + 1 < echoes.size() ? echoes[i + 1] : all.size();
            for (std::size_t line = echoes[i] + 1; line < end && line < all.size(); ++line)
                section.push_back(all[line]);
            m_sections.push_back(std::move(section));
        }
    }

    [[nodiscard]] const frost::io::ProcessResult& result() const noexcept { return m_result; }
    [[nodiscard]] const std::vector<std::string>& section(std::size_t command) const { return m_sections[command]; }
    [[nodiscard]] std::string text(std::size_t command) const {
        std::string out;
        for (const std::string& line : section(command))
            out += line + "\n";
        return out;
    }
    // The frames command `command` printed in the oracle's frame-format, in order.
    [[nodiscard]] std::vector<OracleFrame> frames(std::size_t command) const {
        std::vector<OracleFrame> out;
        static const std::regex frame(R"(FROSTFRAME\|(\d+)\|(.*)\|(.*)\|(\d*)$)");
        for (const std::string& line : section(command)) {
            std::smatch match;
            if (std::regex_search(line, match, frame))
                out.push_back({std::stoi(match[1].str()), match[2].str(), match[3].str(),
                               match[4].str().empty() ? 0 : std::stoi(match[4].str())});
        }
        return out;
    }
    // The line a `breakpoint set` command resolved to ("Breakpoint N: where = ... at <file>:<line>"); 0 when none.
    [[nodiscard]] int resolvedLine(std::size_t command) const {
        static const std::regex where(R"(^Breakpoint \d+: where = .* at [^:]+:(\d+))");
        for (const std::string& line : section(command)) {
            std::smatch match;
            if (std::regex_search(line, match, where))
                return std::stoi(match[1].str());
        }
        return 0;
    }
    // The top-level names `frame variable` printed ("(<type>) <name> = ..." at the line's start).
    [[nodiscard]] std::set<std::string> variableNames(std::size_t command) const {
        std::set<std::string> out;
        static const std::regex variable(R"(^\(.+?\) ([A-Za-z_]\w*) =)");
        for (const std::string& line : section(command)) {
            std::smatch match;
            if (std::regex_search(line, match, variable))
                out.insert(match[1].str());
        }
        return out;
    }
    // `frame variable <name>`'s value ("(int) argc = 5" -> "5"); "" when it printed none.
    [[nodiscard]] std::string value(std::size_t command, std::string_view name) const {
        const std::regex variable(std::format(R"(^\(.+?\) {} = (.*)$)", name));
        for (const std::string& line : section(command)) {
            std::smatch match;
            if (std::regex_search(line, match, variable))
                return match[1].str();
        }
        return {};
    }

private:
    std::vector<std::string> m_commands;
    frost::io::ProcessResult m_result;
    std::vector<std::vector<std::string>> m_sections;
};

// The first call a statement makes, by the source text: the first "<qualified name>(" that is not a
// cast or a keyword ("(void)Frost::GraphicsDevice::UseBuildTreeValidationLayer();" names the
// function). "" when there is none.
std::string firstCall(const std::string& statement) {
    static const std::regex call(R"(([A-Za-z_][\w]*(?:::[A-Za-z_]\w*)*)\s*\()");
    static const std::set<std::string> keywords{"if", "for", "while", "switch", "return", "sizeof", "static_cast"};
    for (auto it = std::sregex_iterator(statement.begin(), statement.end(), call); it != std::sregex_iterator(); ++it)
        if (!keywords.contains((*it)[1].str()))
            return (*it)[1].str();
    return {};
}

// --- The clone's .frost/ and the debuggee --------------------------------------------------------

// Puts the clone's .frost/ back as it was (removed when the test created it; the two files the
// workbench writes restored or removed otherwise), even when the test stopped early, then runs
// `restored` (the git-clean check).
class WorkspaceGuard {
public:
    WorkspaceGuard(std::string corpus, std::function<void()> restored)
        : m_dir(std::move(corpus) + "/.frost"), m_restored(std::move(restored)) {
        auto& fs = frost::io::realFileSystem();
        m_existed = fs.exists(m_dir).value_or(false);
        for (const char* relative : {"/workspace.json", "/runConfigurations/configurations.json"}) {
            const std::string path = m_dir + relative;
            auto bytes = fs.readFile(path);
            m_files.emplace_back(path, bytes ? std::optional<std::string>(std::move(*bytes)) : std::nullopt);
        }
    }
    ~WorkspaceGuard() {
        auto& fs = frost::io::realFileSystem();
        if (!m_existed) {
            EXPECT_TRUE(frost::test::removeTree(fs, m_dir)) << m_dir;
        } else {
            for (const auto& [path, bytes] : m_files) {
                if (bytes)
                    EXPECT_TRUE(fs.writeFileAtomic(path, *bytes).has_value()) << path;
                else if (fs.exists(path).value_or(false))
                    EXPECT_TRUE(fs.removeFile(path).has_value()) << path;
            }
        }
        m_restored();
    }
    WorkspaceGuard(const WorkspaceGuard&) = delete ("restores once");
    WorkspaceGuard& operator=(const WorkspaceGuard&) = delete ("restores once");

private:
    std::string m_dir;
    std::function<void()> m_restored;
    bool m_existed = false;
    std::vector<std::pair<std::string, std::optional<std::string>>> m_files;
};

// Whether process `pid` still runs.
bool processAlive(std::int64_t pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

// Ends the debuggee if the test stopped before Stop did (only that one, identified by its handle).
class DebuggeeGuard {
public:
    DebuggeeGuard() = default;
    ~DebuggeeGuard() {
#ifdef _WIN32
        if (m_process != nullptr) {
            if (WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT) {
                ADD_FAILURE() << "the debuggee outlived the test; terminating it";
                TerminateProcess(m_process, 99);
                WaitForSingleObject(m_process, 10000);
            }
            CloseHandle(m_process);
        }
#else
        if (m_pid > 0 && processAlive(m_pid)) {
            ADD_FAILURE() << "the debuggee outlived the test; killing it";
            ::kill(static_cast<pid_t>(m_pid), SIGKILL);
        }
#endif
    }
    DebuggeeGuard(const DebuggeeGuard&) = delete ("owns a process handle");
    DebuggeeGuard& operator=(const DebuggeeGuard&) = delete ("owns a process handle");
    void watch(std::int64_t pid) {
        m_pid = pid;
#ifdef _WIN32
        m_process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
#endif
    }

private:
    std::int64_t m_pid = 0;
#ifdef _WIN32
    HANDLE m_process = nullptr;
#endif
};

// --- The test ------------------------------------------------------------------------------------

TEST(CorpusFrostEngine, DebuggingFrostEditorStopsStepsInspectsAndHonoursConditionsLogpointsAndThrows) {
    frost::test::disableCrashDialogs(); // cmake, the compilers, lldb and FrostEditor inherit the error mode.
    const std::string corpus = pinnedCorpus();
    ASSERT_FALSE(corpus.empty());
    auto& fs = frost::io::realFileSystem();
    auto adapter = frost::debug::findLldbDap(fs, frost::debug::defaultLldbDapSearch());
    ASSERT_TRUE(adapter.has_value()) << "the pinned lldb-dap is required (FND-6): " << adapter.error().describe();
#ifdef _WIN32
    const std::string lldb = std::string(frost::io::parentPath(*adapter)) + "/lldb.exe";
#else
    const std::string lldb = std::string(frost::io::parentPath(*adapter)) + "/lldb";
#endif
    ASSERT_TRUE(fs.exists(lldb).value_or(false)) << "the pinned lldb CLI is missing beside " << *adapter;
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    const frost::test::EngineBuildLock buildLock(qEnvironmentVariable("FROST_CORPORA_ROOT").toStdString() +
                                                 "/frostengine-" + sha.substr(0, 12) + ".build.lock");
    ASSERT_TRUE(buildLock.held()) << "the corpus build lock stayed taken (tools/corpora.py BuildLock)";
    auto git = frost::vcs::findGit(fs, frost::vcs::defaultGitSearch());
    ASSERT_TRUE(git.has_value()) << git.error().describe();
    const auto gitStatus = [&]() -> std::string {
        const auto status = runTool(git->path, {"-C", corpus, "status", "--porcelain"}, corpus);
        return status.exitCode == 0 ? status.stdoutBytes
                                    : std::format("git status exited {}: {}", status.exitCode, status.stderrBytes);
    };
    ASSERT_EQ(gitStatus(), "") << "the clone is not clean before the test";

    // The statements the breakpoints go on, from FrostEditor's main() as the pin has it.
    const std::string mainPath = frost::io::normalizePath(corpus + "/Frost-Editor/src/EditorApp.cpp").value_or("");
    const auto mainText = fs.readFile(mainPath);
    ASSERT_TRUE(mainText.has_value()) << mainPath;
    const std::vector<std::string> mainSource = lines(*mainText);
    const int mainLine = lineOf(mainSource, "int main(int argc, char** argv)");
    const int argsLine = lineOf(mainSource, "Frost::Platform::GetCommandLineArgsUtf8(argc, argv)", mainLine);
    const int falseLine = lineOf(mainSource, "if (!Frost::RegisterAutomationHostModules())", argsLine);
    const int logLine = lineOf(mainSource, "cli.Registry = &registry;", falseLine);
    const int trueLine = lineOf(mainSource, "Frost::RunCli(args, cli)", logLine);
    ASSERT_TRUE(mainLine > 0 && argsLine > 0 && falseLine > 0 && logLine > 0 && trueLine > 0)
        << "the pinned EditorApp.cpp changed shape; update the statements this test searches for";

    // FrostEditor's scratch config directories (the oracle's and the debugged run's), each holding a
    // Preferences.json that is not JSON (header comment, 4).
    const WorkDir oracleConfig("corpus-frostengine-debug-oracle");
    const WorkDir editorConfig("corpus-frostengine-debug-editor");
    oracleConfig.write("Preferences.json", "{ \"FrostIDE UAT-1\": not JSON");
    editorConfig.write("Preferences.json", "{ \"FrostIDE UAT-1\": not JSON");
    const auto argumentsFor = [](const WorkDir& config) {
        return std::vector<std::string>{"--offscreen", "--no-automation", "--config-dir",
                                        nativeSeparators(config.path())};
    };

    // Destroyed after the rig, which writes .frost/; the clone is git-clean again then.
    const WorkspaceGuard workspace(corpus,
                                   [&] { EXPECT_EQ(gitStatus(), "") << "the clone is not clean after the test"; });
    DebuggeeGuard debuggee;
    WorkDir profileDir("corpus-frostengine-debug");
    WorkbenchRig rig(profileDir);
    ASSERT_TRUE(rig.trust->trust(corpus).has_value());
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(corpus).has_value());
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_EQ(service->activeProfile()->name, FROST_CORPUS_FROSTENGINE_PRESET);
    const auto idle = [&] {
        return service->state() == frost::build::BuildState::Idle && service->lastConfigure().has_value() &&
               service->model() != nullptr;
    };
    ASSERT_TRUE(waitFor(idle, 40min)) << "the open's configure never ended";
    ASSERT_TRUE(service->lastConfigure()->succeeded())
        << (service->lastConfigure()->error ? service->lastConfigure()->error->describe() : "failed");

    // FrostEditor, built first so the oracle and the debug session run the same image.
    const auto model = service->model();
    const auto editorTarget = std::ranges::find(model->targets, kTarget, &frost::project::CMakeTarget::name);
    ASSERT_NE(editorTarget, model->targets.end());
    const auto program = frost::run::cmakeTargetProgram(*editorTarget);
    ASSERT_TRUE(program.has_value()) << program.error().describe();
    {
        auto done = std::make_shared<std::promise<bool>>();
        auto future = done->get_future();
        auto started = service->build(frost::build::BuildAction::Build,
                                      static_cast<std::size_t>(editorTarget - model->targets.begin()),
                                      [done](const frost::build::CMakeBuildOutcome& outcome) {
                                          done->set_value(outcome.result.success && !outcome.error);
                                      });
        ASSERT_TRUE(started.has_value()) << started.error().describe();
        ASSERT_TRUE(waitFor([&] { return future.wait_for(0s) == std::future_status::ready; }, 4h))
            << "the FrostEditor build never ended";
        ASSERT_TRUE(future.get()) << "FrostEditor did not build in the configured engine tree";
        ASSERT_TRUE(waitFor(idle, 5min));
    }

    // The oracle: the pinned lldb CLI on the same FrostEditor (header comment).
    const std::string mainFile = std::string(frost::io::fileName(mainPath));
    // The first run learns where `next` lands; Step Into's check needs the call that line makes.
    enum : std::size_t { kFormat, kArgsBreak, kRun, kBacktrace, kVariables, kArgc, kNext, kAfterNext, kCount };
    LldbRun first(lldb,
                  {std::string(kFrameFormat), std::format("breakpoint set -f {} -l {}", mainFile, argsLine), "run",
                   "bt 8", "frame variable", "frame variable argc", "next", "frame info", "process kill"},
                  *program, argumentsFor(oracleConfig), profileDir.path());
    ASSERT_EQ(first.resolvedLine(kArgsBreak), argsLine) << first.text(kArgsBreak);
    const std::vector<OracleFrame> oracleStack = first.frames(kBacktrace);
    ASSERT_FALSE(oracleStack.empty()) << first.result().stdoutBytes << first.result().stderrBytes;
    const std::set<std::string> oracleLocals = first.variableNames(kVariables);
    const std::string argc = first.value(kArgc, "argc");
    ASSERT_FALSE(argc.empty()) << first.text(kArgc);
    const std::vector<OracleFrame> afterNext = first.frames(kAfterNext);
    ASSERT_EQ(afterNext.size(), 1u) << first.text(kAfterNext);
    const int stepOverLine = afterNext.front().line;
    ASSERT_GT(stepOverLine, argsLine);
    const std::string callee = firstCall(mainSource[static_cast<std::size_t>(stepOverLine - 1)]);
    ASSERT_FALSE(callee.empty()) << "line " << stepOverLine
                                 << " calls nothing: " << mainSource[static_cast<std::size_t>(stepOverLine - 1)];

#ifdef _WIN32
    const std::string throwBreak = "breakpoint set -n _CxxThrowException";
#else
    const std::string throwBreak = "breakpoint set -E c++";
#endif
    enum : std::size_t {
        kFormat2,
        kArgsBreak2,
        kFalseBreak,
        kLogBreak,
        kTrueBreak,
        kDisable,
        kRun2,
        kNext2,
        kStep,
        kAtStep,
        kStepInstruction,
        kAtStepInstruction,
        kFinish,
        kAfterFinish,
        kThrowBreak,
        kToThrow,
        kThrowStack,
        kKill
    };
    LldbRun second(lldb,
                   {std::string(kFrameFormat), std::format("breakpoint set -f {} -l {}", mainFile, argsLine),
                    std::format("breakpoint set -f {} -l {}", mainFile, falseLine),
                    std::format("breakpoint set -f {} -l {}", mainFile, logLine),
                    std::format("breakpoint set -f {} -l {}", mainFile, trueLine), "breakpoint disable 2 3 4", "run",
                    "next", "step", "frame info", "thread step-inst", "frame info", "finish", "frame info", throwBreak,
                    "continue", "bt 24", "process kill"},
                   *program, argumentsFor(oracleConfig), profileDir.path());
    ASSERT_EQ(second.resolvedLine(kFalseBreak), falseLine) << second.text(kFalseBreak);
    ASSERT_EQ(second.resolvedLine(kLogBreak), logLine) << second.text(kLogBreak);
    ASSERT_EQ(second.resolvedLine(kTrueBreak), trueLine) << second.text(kTrueBreak);
    // Where lldb's stepping enters the callee: `step`'s stop when it has a source, else (the MSVC
    // incremental-link thunk) the instruction step after it.
    std::vector<OracleFrame> inCallee = second.frames(kAtStep);
    if (inCallee.empty())
        inCallee = second.frames(kAtStepInstruction);
    const std::vector<OracleFrame> afterFinish = second.frames(kAfterFinish);
    ASSERT_EQ(inCallee.size(), 1u) << second.text(kStep) << second.text(kAtStep) << second.text(kAtStepInstruction);
    // The callee is the one the source names on that line.
    ASSERT_TRUE(contains(inCallee.front().function, callee.substr(callee.rfind(':') + 1)))
        << inCallee.front().function << " is not " << callee;
    ASSERT_EQ(afterFinish.size(), 1u) << second.text(kAfterFinish);
    // The throw site: the first frame with a source below the runtime's throw function.
    const std::vector<OracleFrame> throwStack = second.frames(kThrowStack);
    const auto throwSite = std::ranges::find_if(
        throwStack, [](const OracleFrame& frame) { return !frame.path.empty() && frame.line > 0; });
    ASSERT_NE(throwSite, throwStack.end()) << second.text(kThrowStack);
    // The throw is the seeded one: EditorPreferences::Load is on its stack (the source says it parses there).
    const auto preferencesLoad = std::ranges::find_if(
        throwStack, [](const OracleFrame& frame) { return contains(frame.function, "EditorPreferences::Load"); });
    ASSERT_NE(preferencesLoad, throwStack.end()) << second.text(kThrowStack);
    // FrostEditor took the oracle's config directory: it wrote its run log there.
    EXPECT_TRUE(fs.exists(oracleConfig.join("Logs")).value_or(false));

    // The run configuration, edited as the Run/Debug Configurations dialog's OK does (RUN-3).
    auto* runs = rig.workbench->runController();
    ASSERT_NE(runs, nullptr);
    const std::string configurationId = frost::run::runConfigurationIdFor(frost::run::kCMakeIdPrefix, kTarget);
    ASSERT_TRUE(waitFor(
        [&] {
            const auto all = runs->configurations().all();
            return rig.workbench->cmakeTargetsCurrent() &&
                   std::ranges::contains(all, configurationId, &frost::run::RunConfiguration::id);
        },
        2min));
    std::vector<frost::run::RunConfiguration> edited = runs->configurations().all();
    const auto configuration = std::ranges::find(edited, configurationId, &frost::run::RunConfiguration::id);
    ASSERT_NE(configuration, edited.end());
    configuration->arguments = argumentsFor(editorConfig);
    ASSERT_TRUE(runs->replaceConfigurations(edited).has_value());
    ASSERT_TRUE(runs->selectConfiguration(configurationId).has_value());

    // 1. The line breakpoint in main(), Debug on the toolbar's action, the stop.
    auto* debug = rig.workbench->debugController();
    ASSERT_NE(debug, nullptr);
    ASSERT_TRUE(debug->breakpoints().add(mainPath, argsLine).has_value());
    int stops = 0;
    QObject::connect(debug, &frost::ui::DebugController::stopped, [&] { ++stops; });
    const auto stoppedAgain = [&](int previous, std::chrono::milliseconds timeout) {
        return waitFor(
                   [&] {
                       return !debug->debugging() || (stops > previous && debug->paused() && debug->selectedFrame());
                   },
                   timeout) &&
               debug->debugging();
    };
    const auto where = [&] {
        const auto& frame = debug->selectedFrame();
        return frame ? std::format("{} at {}:{}", frame->name, frame->path, frame->line) : std::string("no frame");
    };
    ASSERT_EQ(rig.window->invoke("run.debug"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(stoppedAgain(0, kFirstStop)) << rig.workbench->runConsole()->text().toStdString();
    ASSERT_NE(debug->session(), nullptr);
    const auto pid = debug->session()->processId();
    ASSERT_TRUE(pid.has_value());
    debuggee.watch(*pid);
    ASSERT_TRUE(debug->stopInfo().has_value());
    EXPECT_EQ(debug->stopInfo()->reason, "breakpoint");
    EXPECT_TRUE(samePath(debug->selectedFrame()->path, mainPath)) << where();
    EXPECT_EQ(debug->selectedFrame()->line, argsLine) << where();
    ASSERT_TRUE(debug->breakpoints().at(mainPath, argsLine).has_value());
    EXPECT_EQ(debug->breakpoints().at(mainPath, argsLine)->status, frost::debug::BreakpointStatus::Verified);
    // The call stack's top frames are lldb's (name, file and line of each frame that has a source).
    const std::vector<frost::debug::DebugFrame> stack = debug->frames().frames;
    std::size_t compared = 0;
    for (const OracleFrame& expected : oracleStack) {
        if (expected.path.empty() || compared == 5)
            break;
        // Every frame lldb printed with a source must be in the session's call stack too.
        ASSERT_LT(expected.index, static_cast<int>(stack.size()))
            << "lldb printed frame " << expected.index << " with a source; the session has " << stack.size();
        const auto& frame = stack[static_cast<std::size_t>(expected.index)];
        EXPECT_EQ(frame.name, expected.function) << "frame " << expected.index;
        EXPECT_TRUE(samePath(frame.path, expected.path)) << frame.path << " vs " << expected.path;
        EXPECT_EQ(frame.line, expected.line) << "frame " << expected.index;
        ++compared;
    }
    /*
     * lldb prints a frame in the oracle's frame-format only when the frame has line information. On
     * Windows, main()'s caller is the CRT's invoke_main, which has a source, so at least two frames are
     * compared. On Linux main() is called by the C library, which has no debug information, so main()
     * is the only frame lldb prints (seen in the R2 WSL corpus run: lldb 1 frame, the session 4).
     */
#ifdef _WIN32
    constexpr std::size_t kSourceFramesAtLeast = 2;
#else
    constexpr std::size_t kSourceFramesAtLeast = 1;
#endif
    EXPECT_GE(compared, kSourceFramesAtLeast)
        << "lldb printed " << oracleStack.size() << " frames, the session has " << stack.size();
    // The Locals: the names lldb's `frame variable` printed, and lldb's argc (the edited arguments arrived).
    {
        auto scopes = std::make_shared<std::optional<frost::Result<frost::debug::ScopeList>>>();
        debug->scopes(debug->selectedFrame()->id, [scopes](auto result) { *scopes = std::move(result); });
        ASSERT_TRUE(waitFor([&] { return scopes->has_value(); }, kWait));
        ASSERT_TRUE(scopes->value().has_value()) << scopes->value().error().describe();
        std::set<std::string> names;
        for (const auto& scope : *scopes->value()) {
            if (scope.name != "Locals" && scope.name != "Arguments" && scope.presentationHint != "locals" &&
                scope.presentationHint != "arguments")
                continue;
            auto variables = std::make_shared<std::optional<frost::Result<frost::debug::VariableList>>>();
            debug->variables(scope.variablesReference, 0, 0,
                             [variables](auto result) { *variables = std::move(result); });
            ASSERT_TRUE(waitFor([&] { return variables->has_value(); }, kWait));
            ASSERT_TRUE(variables->value().has_value()) << variables->value().error().describe();
            for (const auto& variable : *variables->value())
                names.insert(variable.name);
        }
        EXPECT_FALSE(names.empty());
        EXPECT_EQ(names, oracleLocals);
        auto value = std::make_shared<std::optional<frost::Result<frost::debug::dap::Variable>>>();
        debug->evaluate(QStringLiteral("argc"), QStringLiteral("watch"),
                        [value](auto result) { *value = std::move(result); });
        ASSERT_TRUE(waitFor([&] { return value->has_value(); }, kWait));
        ASSERT_TRUE(value->value().has_value()) << value->value().error().describe();
        EXPECT_EQ(value->value()->value, argc);
    }

    // 2. Step Over (F8), Step Into (F7), Step Out (Shift+F8) land where lldb lands.
    int previous = stops;
    ASSERT_EQ(rig.window->invoke("debug.stepOver"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(stoppedAgain(previous, kWait)) << where();
    EXPECT_TRUE(samePath(debug->selectedFrame()->path, mainPath)) << where();
    EXPECT_EQ(debug->selectedFrame()->line, stepOverLine) << where();
    previous = stops;
    ASSERT_EQ(rig.window->invoke("debug.stepInto"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(stoppedAgain(previous, kWait)) << where();
    EXPECT_EQ(debug->selectedFrame()->name, inCallee.front().function) << where();
    EXPECT_TRUE(samePath(debug->selectedFrame()->path, inCallee.front().path)) << where();
    EXPECT_EQ(debug->selectedFrame()->line, inCallee.front().line) << where();
    previous = stops;
    ASSERT_EQ(rig.window->invoke("debug.stepOut"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(stoppedAgain(previous, kWait)) << where();
    EXPECT_EQ(debug->selectedFrame()->name, afterFinish.front().function) << where();
    EXPECT_TRUE(samePath(debug->selectedFrame()->path, afterFinish.front().path)) << where();
    EXPECT_EQ(debug->selectedFrame()->line, afterFinish.front().line) << where();

    // 3. A false condition, a logpoint and a true condition further down main(), set while paused
    //    as the Edit Breakpoint dialog sets them.
    auto& breakpoints = debug->breakpoints();
    const auto falseBreak = breakpoints.add(mainPath, falseLine);
    const auto logBreak = breakpoints.add(mainPath, logLine);
    const auto trueBreak = breakpoints.add(mainPath, trueLine);
    ASSERT_TRUE(falseBreak && logBreak && trueBreak);
    const std::string logPrefix = "frost-uat1-logpoint argc=";
    ASSERT_TRUE(breakpoints.setProperties(*falseBreak, "argc != " + argc, "", ""));
    ASSERT_TRUE(breakpoints.setProperties(*logBreak, "", "", logPrefix + "{argc}"));
    ASSERT_TRUE(breakpoints.setProperties(*trueBreak, "argc == " + argc, "", ""));
    // The adapter bound all three (a breakpoint that never bound would "not stop" for nothing).
    ASSERT_TRUE(waitFor(
        [&] {
            return std::ranges::all_of(std::array{falseLine, logLine, trueLine}, [&](int line) {
                const auto found = breakpoints.at(mainPath, line);
                return found && found->status == frost::debug::BreakpointStatus::Verified;
            });
        },
        kWait))
        << "the adapter did not verify the breakpoints on lines " << falseLine << ", " << logLine << ", " << trueLine;
    auto* window = rig.window->findChild<frost::ui::DebugToolWindow*>();
    ASSERT_NE(window, nullptr);
    previous = stops;
    ASSERT_EQ(rig.window->invoke("debug.resume"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(stoppedAgain(previous, kWait)) << where();
    EXPECT_EQ(stops, previous + 1) << "one stop: the logpoint and the false condition do not stop";
    EXPECT_EQ(debug->stopInfo()->reason, "breakpoint");
    EXPECT_TRUE(samePath(debug->selectedFrame()->path, mainPath)) << where();
    EXPECT_EQ(debug->selectedFrame()->line, trueLine) << where();
    EXPECT_TRUE(
        waitFor([&] { return window->console()->text().contains(QString::fromStdString(logPrefix + argc)); }, kWait))
        << window->console()->text().toStdString();

    // 4. The C++ throw row of the Breakpoints tab, checked, then Resume: the first throw of the start.
    QTreeWidget* tree = window->breakpointsView();
    ASSERT_NE(tree, nullptr);
    QTreeWidgetItem* throwRow = nullptr;
    ASSERT_TRUE(waitFor(
        [&] {
            for (int row = 0; row < tree->topLevelItemCount(); ++row)
                if (QTreeWidgetItem* item = tree->topLevelItem(row);
                    item->toolTip(1) == QStringLiteral("cpp_throw") && (item->flags() & Qt::ItemIsUserCheckable))
                    throwRow = item;
            return throwRow != nullptr;
        },
        kWait))
        << "the Breakpoints tab lists no checkable cpp_throw row";
    throwRow->setCheckState(0, Qt::Checked);
    ASSERT_TRUE(std::ranges::contains(breakpoints.sessionExceptionFilters(), std::string("cpp_throw")));
    previous = stops;
    ASSERT_EQ(rig.window->invoke("debug.resume"), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(stoppedAgain(previous, kWait)) << where();
    EXPECT_EQ(debug->stopInfo()->reason, "exception");
    EXPECT_EQ(debug->stopInfo()->exceptionFilter, "cpp_throw");
    EXPECT_FALSE(debug->stopInfo()->isCrash());
    EXPECT_EQ(debug->selectedFrame()->name, throwSite->function) << where();
    EXPECT_TRUE(samePath(debug->selectedFrame()->path, throwSite->path)) << where();
    EXPECT_EQ(debug->selectedFrame()->line, throwSite->line) << where();
    EXPECT_TRUE(std::ranges::any_of(debug->frames().frames,
                                    [&](const frost::debug::DebugFrame& frame) {
                                        return frame.name == preferencesLoad->function &&
                                               samePath(frame.path, preferencesLoad->path) &&
                                               frame.line == preferencesLoad->line;
                                    }))
        << "EditorPreferences::Load at " << preferencesLoad->path << ":" << preferencesLoad->line;

    // 5. Stop ends the session and the debuggee.
    debug->stop();
    ASSERT_TRUE(waitFor([&] { return !debug->debugging(); }, kWait));
    EXPECT_TRUE(waitFor([&] { return !processAlive(*pid); }, kWait)) << "FrostEditor " << *pid << " still runs";
    EXPECT_TRUE(fs.exists(editorConfig.join("Logs")).value_or(false)) << "FrostEditor did not get --config-dir";
}

} // namespace
