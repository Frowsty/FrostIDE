// SPDX-License-Identifier: Apache-2.0
//
// MSVC-built debuggees under the pinned lldb-dap (label "integration", tags "lldb" and "msvc";
// Windows only): DBG-2b's type-filtered C++ throws, DBG-4a's STL display and where Step Into ends
// through an incremental-link thunk.
/*
 * The debuggee. tests/debug/fixtures/msvc_debuggee.cpp is compiled by the test itself with cl.exe of
 * the pinned MSVC toolset (sdk/sdk.lock.json, msvc.vcToolsVersion), run through IProcessLauncher in
 * the vcvarsall environment, as a Visual Studio Debug build would be: /Od /Zi /RTC1 /EHsc and an
 * incremental /DEBUG link. Two variants: the DLL runtime (/MDd, the default, where _CxxThrowException
 * is an import with a thunk and no symbols) and the static runtime (/MTd, where the CRT's own symbols
 * are in the PDB and lldb skips the function's prologue). The Step Into test also runs on a new Visual
 * Studio project's Debug build (/ZI /JMC /MDd: Edit and Continue, whose thunks lie under the
 * __enc$textbss$end symbol, and Just My Code), where lldb's own step-in steps over the call unless
 * step-in-avoid-nodebug is off (lldb_dap.hpp, "Stepping on Windows").
 *
 * Oracles. Lines and expected values come from the fixture's source (its FROST_* markers and
 * literals). The thrown types also come from the pinned lldb CLI: at each _CxxThrowException stop
 * `register read rdx` names the ThrowInfo symbol ("_TI2?AVruntime_error@std@@"), whose suffix is the
 * type's decorated RTTI name. The STL summaries come from `frame variable` of the same lldb, and the
 * Step Into landing line from lldb's `step` + `thread step-inst` (the session's own route through the
 * thunk). Nothing is a number written into the test.
 *
 * DBG-4a gaps (lldb 23.1.4 with this cl.exe build; reported by StlContainersShowTheirElements, not
 * asserted): std::array has no formatter (a raw _Elems child); std::string, std::wstring and
 * std::string_view show their text but only raw members as children (no size or capacity); map and
 * unordered_map elements show the pair's type name until expanded (first and second are there);
 * std::optional and std::variant summaries carry padding spaces (" Has Value=true "). vector, deque,
 * list, set, map, unordered_map, tuple, unique_ptr, shared_ptr, optional and variant show their
 * elements. LLDB's MSVC STL formatters are on by default (the C++ category); FrostIDE sends no setting.
 */

#include "frost/build/cmake_toolchain.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/debug/debug_session.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/debug/msvc_throw.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "debug_test_util.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <future>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#ifndef FROST_MSVC_DEBUGGEE_SOURCE
#error "FROST_MSVC_DEBUGGEE_SOURCE comes from tests/debug/CMakeLists.txt"
#endif

namespace frost::debug {
namespace {

using namespace std::chrono_literals;
using test::SessionRecorder;
using test::waitUntil;

// How long one thing lldb-dap does may take under parallel load.
constexpr auto kWait = 90s;

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

bool sameText(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        std::string line(text.substr(start, end - start));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(std::move(line));
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

// Runs a request and waits for its completion.
template<typename T, typename Send>
Result<T> awaitResult(Send send) {
    auto promise = std::make_shared<std::promise<Result<T>>>();
    auto future = promise->get_future();
    send([promise](Result<T> result) { promise->set_value(std::move(result)); });
    if (future.wait_for(kWait) != std::future_status::ready) {
        return fail(ErrorCode::Timeout, "lldb-dap did not answer in time");
    }
    return future.get();
}

// The vcvarsall environment of the pinned MSVC toolset, or why there is none.
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
        const auto version = std::ranges::find_if(
            captured->changes, [](const auto& entry) { return sameText(entry.first, "VCToolsVersion"); });
        if (version == captured->changes.end() || !version->second || !contains(*version->second, pinned)) {
            return fail(ErrorCode::NotFound,
                        std::format("{}'s default MSVC toolset is not the pinned {}", instance.displayName, pinned));
        }
        return std::move(captured->changes);
    }
    return fail(ErrorCode::NotFound, std::format("no Visual Studio instance has the pinned MSVC toolset {}", pinned));
}

// cl.exe on the captured PATH.
Result<std::string> findCl(const project::CMakeEnvironment& environment) {
    auto& fs = io::realFileSystem();
    for (const auto& [name, value] : environment) {
        if (!sameText(name, "PATH") || !value) {
            continue;
        }
        std::size_t start = 0;
        while (start <= value->size()) {
            const std::size_t end = std::min(value->find(';', start), value->size());
            const std::string directory = value->substr(start, end - start);
            if (!directory.empty()) {
                if (auto cl = io::joinPath(directory, "cl.exe"); cl && fs.exists(*cl).value_or(false)) {
                    return io::normalizePath(*cl);
                }
            }
            start = end + 1;
        }
    }
    return fail(ErrorCode::NotFound, "cl.exe is not on the vcvarsall PATH");
}

// The tools and the fixture's builds (header comment). ctest runs each test in its own process, so
// a build is made the first time a test asks for it, in a scratch directory removed at exit.
struct Toolchain {
    std::string error;   // Why the tools are missing; "" when they are there.
    std::string adapter; // The pinned lldb-dap.
    std::string lldb;    // The pinned lldb CLI beside it.
    std::string cl;      // The pinned toolset's cl.exe.
    project::CMakeEnvironment environment;
    std::unique_ptr<frost::test::ScratchDir> dir;
    std::map<std::string, Result<std::string>> builds; // Runtime flag -> program, or why it failed.
};

Toolchain& toolchain() {
    static Toolchain tools = [] {
        Toolchain out;
        auto adapter = findLldbDap(io::realFileSystem(), defaultLldbDapSearch());
        if (!adapter) {
            out.error = "the pinned lldb-dap is required: " + adapter.error().message();
            return out;
        }
        out.adapter = *adapter;
        out.lldb = std::string(io::parentPath(*adapter)) + "/lldb.exe";
        auto environment = pinnedMsvcEnvironment();
        if (!environment) {
            out.error = "the pinned MSVC toolset is required: " + environment.error().describe();
            return out;
        }
        auto cl = findCl(*environment);
        if (!cl) {
            out.error = cl.error().describe();
            return out;
        }
        out.cl = *cl;
        out.environment = std::move(*environment);
        out.dir = std::make_unique<frost::test::ScratchDir>("msvc-debuggee");
        return out;
    }();
    return tools;
}

// The fixture's builds (header comment).
enum class Variant : std::uint8_t {
    DllCrt,       // /Zi /MDd: CMake's Debug flags (FrostEngine's).
    StaticCrt,    // /Zi /MTd.
    VisualStudio, // /ZI /JMC /MDd: a new Visual Studio C++ project's Debug|x64 (Edit and Continue, Just My Code).
};

// The fixture built as `variant`: the program's path.
Result<std::string> program(Variant variant) {
    Toolchain& tools = toolchain();
    if (!tools.error.empty()) {
        return fail(ErrorCode::NotFound, tools.error);
    }
    const std::string name = variant == Variant::StaticCrt ? "mt" : (variant == Variant::DllCrt ? "md" : "vs");
    if (const auto known = tools.builds.find(name); known != tools.builds.end()) {
        return known->second;
    }
    io::ProcessSpec spec;
    spec.executable = tools.cl;
    spec.workingDirectory = tools.dir->path();
    spec.environment = tools.environment;
    spec.arguments = {"/nologo", "/EHsc", "/Od", "/RTC1", "/std:c++20"};
    switch (variant) {
    case Variant::DllCrt: spec.arguments.insert(spec.arguments.end(), {"/Zi", "/MDd"}); break;
    case Variant::StaticCrt: spec.arguments.insert(spec.arguments.end(), {"/Zi", "/MTd"}); break;
    case Variant::VisualStudio: spec.arguments.insert(spec.arguments.end(), {"/ZI", "/JMC", "/MDd"}); break;
    }
    spec.arguments.insert(spec.arguments.end(),
                          {io::normalizePath(FROST_MSVC_DEBUGGEE_SOURCE).value(), std::format("/Fe{}.exe", name),
                           std::format("/Fd{}_compile.pdb", name), std::format("/Fo{}.obj", name), "/link", "/DEBUG"});
    auto result = io::runToCompletion(io::realProcessLauncher(), spec, {}, 10min);
    Result<std::string> built = tools.dir->join(name + ".exe");
    if (!result) {
        built = std::unexpected(std::move(result.error()));
    } else if (result->exitCode != 0) {
        built = fail(ErrorCode::IoError, std::format("cl.exe ({}) failed ({}): {}{}", name, result->exitCode,
                                                     result->stdoutBytes, result->stderrBytes));
    }
    tools.builds.emplace(name, built);
    return built;
}

// The lldb CLI in batch mode over `program` with `commands`; its stdout.
std::string lldbBatch(const std::vector<std::string>& commands, const std::string& program) {
    io::ProcessSpec spec;
    spec.executable = toolchain().lldb;
    for (const std::string& command : commands) {
        spec.arguments.insert(spec.arguments.end(), {"-o", command});
    }
    spec.arguments.insert(spec.arguments.end(), {"-b", "--", program});
    spec.workingDirectory = std::string(io::parentPath(program));
    auto result = io::runToCompletion(io::realProcessLauncher(), spec, {}, 10min);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().describe());
    return result ? result->stdoutBytes : std::string();
}

class MsvcDebug : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(toolchain().error.empty()) << toolchain().error;
        auto dll = program(Variant::DllCrt);
        ASSERT_TRUE(dll.has_value()) << dll.error().describe();
        m_dllRuntime = *dll;
        m_sourcePath = io::normalizePath(FROST_MSVC_DEBUGGEE_SOURCE).value();
        auto source = io::realFileSystem().readFile(m_sourcePath);
        ASSERT_TRUE(source.has_value()) << source.error().describe();
        m_source = splitLines(*source);
    }

    void TearDown() override {
        if (m_session) {
            m_session->kill(); // lldb-dap's tree ends, the debuggee with it.
            m_session.reset();
        }
    }

    // The 1-based line holding `marker`; 0 when there is none.
    [[nodiscard]] std::int64_t lineOf(std::string_view marker) const {
        for (std::size_t i = 0; i < m_source.size(); ++i) {
            if (contains(m_source[i], marker)) {
                return static_cast<std::int64_t>(i + 1);
            }
        }
        return 0;
    }

    // The thrown types in throw order, from the FROST_THROW markers.
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::string>> throwSites() const {
        std::vector<std::pair<std::int64_t, std::string>> out;
        for (std::size_t i = 0; i < m_source.size(); ++i) {
            const std::size_t at = m_source[i].find("// FROST_THROW ");
            if (at != std::string::npos) {
                out.emplace_back(static_cast<std::int64_t>(i + 1), m_source[i].substr(at + 15));
            }
        }
        return out;
    }

    void start(const std::string& program, std::map<std::string, std::vector<BreakpointLine>> lines,
               std::vector<std::string> filters, std::vector<std::string> types) {
        DebugTarget target{.name = "msvc_debuggee", .program = program};
        DebugSessionOptions options = lldbDapSessionOptions(DebugTargetMode::Launch);
        options.exceptionFilters = std::move(filters);
        options.throwTypes = std::move(types);
        m_session = DebugSession::start(lldbDapAdapterFactory(io::realProcessLauncher(), toolchain().adapter),
                                        lldbDapLaunchArguments(target), std::move(lines), m_recorder.callbacks(),
                                        std::move(options));
        ASSERT_TRUE(m_session->msvcExceptions()) << "lldb-dap on Windows translates C++ throws";
    }

    [[nodiscard]] std::string report() const {
        LockGuard lock(m_recorder.mutex);
        std::string text = "events: " + testing::PrintToString(m_recorder.events) + "\noutput: " + m_recorder.output;
        if (m_recorder.ended && m_recorder.ended->error) {
            text += "\nerror: " + m_recorder.ended->error->describe();
        }
        return text;
    }

    // Waits for stop number `count` (1-based) and its selected frame, or the session's end.
    bool waitForStop(std::size_t count) {
        return waitUntil(
            [&] {
                return (m_recorder.stopCount() >= count && m_recorder.selectedCount() >= count) ||
                       m_recorder.hasEnded();
            },
            kWait);
    }

    [[nodiscard]] StopInfo stopAt(std::size_t index) const {
        LockGuard lock(m_recorder.mutex);
        return m_recorder.stops.at(index);
    }

    // The Locals of the selected frame.
    [[nodiscard]] VariableList locals() {
        const auto frame = m_session->selectedFrame();
        EXPECT_TRUE(frame.has_value());
        if (!frame) {
            return {};
        }
        auto scopes = awaitResult<ScopeList>([&](const auto& done) { m_session->scopes(frame->id, done); });
        EXPECT_TRUE(scopes.has_value()) << (scopes ? "" : scopes.error().describe());
        if (!scopes) {
            return {};
        }
        const auto scope = std::ranges::find_if(*scopes, [](const dap::Scope& s) { return s.name == "Locals"; });
        EXPECT_NE(scope, scopes->end());
        if (scope == scopes->end()) {
            return {};
        }
        auto list = awaitResult<VariableList>(
            [&](const auto& done) { m_session->variables(scope->variablesReference, 0, 0, done); });
        EXPECT_TRUE(list.has_value()) << (list ? "" : list.error().describe());
        return list ? *list : VariableList{};
    }

    [[nodiscard]] VariableList children(const dap::Variable& variable) {
        if (variable.variablesReference <= 0) {
            return {};
        }
        auto list = awaitResult<VariableList>(
            [&](const auto& done) { m_session->variables(variable.variablesReference, 0, 0, done); });
        EXPECT_TRUE(list.has_value()) << (list ? "" : list.error().describe());
        return list ? *list : VariableList{};
    }

    std::string m_dllRuntime; // The /MDd build (header comment).
    std::string m_sourcePath;
    std::vector<std::string> m_source;
    SessionRecorder m_recorder;
    std::unique_ptr<DebugSession> m_session;
};

// The decorated names lldb's `register read rdx` shows at each _CxxThrowException stop, in order,
// consecutive repeats (the import thunk's stop, then the runtime's) folded.
std::vector<std::string> oracleThrownTypes(const std::string& program, std::size_t throws) {
    std::vector<std::string> commands{"breakpoint set -n _CxxThrowException", "run"};
    for (std::size_t i = 0; i < throws * 2; ++i) {
        commands.insert(commands.end(), {"register read rdx", "continue"});
    }
    const std::string output = lldbBatch(commands, program);
    std::vector<std::string> out;
    static const std::regex symbol(R"(rdx = 0x[0-9A-Fa-f]+\s+_TI\d+(\S+))");
    for (auto it = std::sregex_iterator(output.begin(), output.end(), symbol); it != std::sregex_iterator(); ++it) {
        std::string decorated = "." + (*it)[1].str();
        if (out.empty() || out.back() != decorated) {
            out.push_back(std::move(decorated));
        }
    }
    return out;
}

// DBG-2b acceptance: a cpp_throw filtered to std::runtime_error stops only for that throw.
class MsvcThrowFilter : public MsvcDebug, public testing::WithParamInterface<bool> {};

TEST_P(MsvcThrowFilter, ARuntimeErrorFilterStopsOnlyForThatType) {
    auto built = program(GetParam() ? Variant::StaticCrt : Variant::DllCrt);
    ASSERT_TRUE(built.has_value()) << built.error().describe();
    const auto sites = throwSites();
    const auto runtime =
        std::ranges::find(sites, std::string("std::runtime_error"), &std::pair<std::int64_t, std::string>::second);
    ASSERT_NE(runtime, sites.end());
    start(*built, {}, {"cpp_throw"}, {"std::runtime_error"});
    ASSERT_TRUE(waitForStop(1)) << report();
    ASSERT_FALSE(m_recorder.hasEnded()) << "no stop for std::runtime_error\n" << report();
    const StopInfo stop = stopAt(0);
    EXPECT_EQ(stop.reason, "exception");
    EXPECT_EQ(stop.exceptionFilter, "cpp_throw");
    EXPECT_EQ(stop.exceptionType, "std::runtime_error");
    EXPECT_EQ(stop.description, "C++ exception thrown: std::runtime_error");
    const auto frame = m_session->selectedFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(contains(frame->name, "throwKind")) << frame->name;
    EXPECT_EQ(frame->line, runtime->first) << "the std::runtime_error throw, not another one";

    // The other four throws (std::range_error derives from std::runtime_error) do not stop.
    ASSERT_TRUE(m_session->continueRun().has_value());
    ASSERT_TRUE(waitUntil([&] { return m_recorder.hasEnded(); }, kWait)) << report();
    EXPECT_EQ(m_recorder.stopCount(), 1U) << report();
    const auto outcome = m_session->outcome();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->exitCode, 0) << "the fixture caught all five throws\n" << report();
}

INSTANTIATE_TEST_SUITE_P(Runtimes, MsvcThrowFilter, testing::Values(false, true),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? std::string("StaticCrt") : std::string("DllCrt");
                         });

// Without a type list every throw stops and names its type as lldb's own symbols do.
TEST_F(MsvcDebug, EveryThrowNamesTheTypeLldbSees) {
    const auto sites = throwSites();
    ASSERT_EQ(sites.size(), 5U) << "the fixture's FROST_THROW markers";
    const std::vector<std::string> oracle = oracleThrownTypes(m_dllRuntime, sites.size());
    ASSERT_EQ(oracle.size(), sites.size()) << testing::PrintToString(oracle);
    start(m_dllRuntime, {}, {"cpp_throw"}, {});
    for (std::size_t i = 0; i < sites.size(); ++i) {
        ASSERT_TRUE(waitForStop(i + 1)) << report();
        ASSERT_FALSE(m_recorder.hasEnded()) << "throw " << i << " did not stop\n" << report();
        const StopInfo stop = stopAt(i);
        EXPECT_EQ(stop.exceptionType, sites[i].second) << "the source's throw " << i;
        EXPECT_EQ(stop.exceptionType, msvc::undecorateTypeName(oracle[i]).value_or(oracle[i]))
            << "lldb's ThrowInfo symbol " << oracle[i];
        EXPECT_EQ(m_session->selectedFrame()->line, sites[i].first);
        ASSERT_TRUE(m_session->continueRun().has_value());
    }
    ASSERT_TRUE(waitUntil([&] { return m_recorder.hasEnded(); }, kWait)) << report();
    EXPECT_EQ(m_recorder.stopCount(), sites.size());
}

// DBG-4a acceptance: vector, string, map, unique_ptr and optional show their elements.
TEST_F(MsvcDebug, StlContainersShowTheirElements) {
    const std::int64_t line = lineOf("// FROST_STL_BREAK");
    ASSERT_GT(line, 0);
    start(m_dllRuntime, {{m_sourcePath, {BreakpointLine{.id = 1, .line = line}}}}, {}, {});
    ASSERT_TRUE(waitForStop(1)) << report();
    ASSERT_FALSE(m_recorder.hasEnded()) << report();
    ASSERT_EQ(m_session->selectedFrame()->line, line);

    // The oracle: lldb's own summaries of the same variables at the same line.
    const std::vector<std::string> names{"numbers", "text", "longText", "table", "owned", "maybe", "none"};
    std::vector<std::string> commands{std::format("breakpoint set -f msvc_debuggee.cpp -l {}", line), "run"};
    for (const std::string& name : names) {
        commands.push_back("frame variable " + name);
    }
    commands.emplace_back("process kill");
    const std::string oracle = lldbBatch(commands, m_dllRuntime);

    const VariableList variables = locals();
    const auto get = [&](std::string_view name) -> std::optional<dap::Variable> {
        const auto it = std::ranges::find(variables, std::string(name), &dap::Variable::name);
        return it == variables.end() ? std::nullopt : std::optional(*it);
    };
    const auto values = [&](const dap::Variable& variable) {
        std::vector<std::string> out;
        for (const dap::Variable& child : children(variable)) {
            out.push_back(child.value);
        }
        return out;
    };
    for (const std::string& name : names) {
        const auto variable = get(name);
        ASSERT_TRUE(variable.has_value()) << name << " is not in Locals";
        // lldb prints "(<type>) <name> = <summary>"; the summary DAP shows is the same one.
        EXPECT_TRUE(contains(oracle, std::format(") {} = {}", name, variable->value)))
            << name << " = " << variable->value << "\nlldb:\n"
            << oracle;
    }
    // Elements, against the fixture's literals.
    EXPECT_EQ(values(*get("numbers")), (std::vector<std::string>{"1", "2", "3"}));
    EXPECT_EQ(get("text")->value, "\"hello msvc\"");
    EXPECT_EQ(get("longText")->value, "\"a string longer than the small string buffer\"");
    const VariableList table = children(*get("table"));
    ASSERT_EQ(table.size(), 2U);
    std::vector<std::string> entries;
    for (const dap::Variable& entry : table) {
        std::string pair;
        for (const dap::Variable& member : children(entry)) {
            pair += member.name + "=" + member.value + ";";
        }
        entries.push_back(pair);
    }
    EXPECT_EQ(entries, (std::vector<std::string>{"first=1;second=\"one\";", "first=2;second=\"two\";"}));
    EXPECT_EQ(get("owned")->value, "5");
    EXPECT_EQ(values(*get("maybe")), (std::vector<std::string>{"9"}));
    EXPECT_TRUE(contains(get("none")->value, "Has Value=false")) << get("none")->value;
    EXPECT_TRUE(children(*get("none")).empty());

    // The gaps of the header comment, as lldb shows them today (reported, not asserted).
    for (const char* name :
         {"fixed", "wide", "view", "shared", "queue", "linked", "sorted", "hashed", "either", "pair"}) {
        if (const auto variable = get(name)) {
            RecordProperty(std::string("stl.") + name,
                           variable->value + " [" + testing::PrintToString(values(*variable)) + "]");
        }
    }
}

// Step Into through MSVC's incremental-link thunk ends on the callee's opening line, where lldb's
// own instruction step after the thunk does (DebugSession, "Step into a thunk"), in CMake's Debug
// build and in Visual Studio's (/ZI, whose thunk lldb's own step-in steps over).
class MsvcStepInto : public MsvcDebug, public testing::WithParamInterface<Variant> {};

TEST_P(MsvcStepInto, StepIntoThroughTheThunkStopsOnTheOpeningLine) {
    auto built = program(GetParam());
    ASSERT_TRUE(built.has_value()) << built.error().describe();
    const std::int64_t line = lineOf("// FROST_STL_BREAK");
    const std::int64_t open = lineOf("// FROST_CALLEE_OPEN");
    const std::int64_t first = lineOf("// FROST_CALLEE_FIRST");
    ASSERT_GT(line, 0);
    ASSERT_GT(open, 0);
    ASSERT_GT(first, open);
    // The oracle: lldb as FrostIDE launches it on Windows (lldbDapWindowsStepCommand), its step
    // (which ends in the thunk) and one instruction step.
    const std::string oracle = lldbBatch({"settings set frame-format \"FROSTFRAME|${function.name}|${line.number}\\n\"",
                                          std::string(lldbDapWindowsStepCommand),
                                          std::format("breakpoint set -f msvc_debuggee.cpp -l {}", line), "run", "step",
                                          "thread step-inst", "frame info", "process kill"},
                                         *built);
    std::smatch match;
    static const std::regex frame(R"(FROSTFRAME\|(.*callee.*)\|(\d+))");
    ASSERT_TRUE(std::regex_search(oracle, match, frame)) << oracle;
    const std::int64_t oracleLine = std::stoll(match[2].str());
    EXPECT_EQ(oracleLine, open) << "lldb lands on the callee's opening line\n" << oracle;

    start(*built, {{m_sourcePath, {BreakpointLine{.id = 1, .line = line}}}}, {}, {});
    ASSERT_TRUE(waitForStop(1)) << report();
    ASSERT_FALSE(m_recorder.hasEnded()) << report();
    ASSERT_TRUE(m_session->stepIn().has_value());
    ASSERT_TRUE(waitForStop(2)) << report();
    ASSERT_FALSE(m_recorder.hasEnded()) << report();
    auto stopped = m_session->selectedFrame();
    ASSERT_TRUE(stopped.has_value());
    EXPECT_TRUE(contains(stopped->name, "callee")) << stopped->name;
    EXPECT_EQ(stopped->line, oracleLine);
    // The next Step Over reaches the first statement.
    ASSERT_TRUE(m_session->next().has_value());
    ASSERT_TRUE(waitForStop(3)) << report();
    stopped = m_session->selectedFrame();
    ASSERT_TRUE(stopped.has_value());
    EXPECT_TRUE(contains(stopped->name, "callee")) << stopped->name;
    EXPECT_EQ(stopped->line, first);
}

INSTANTIATE_TEST_SUITE_P(Builds, MsvcStepInto, testing::Values(Variant::DllCrt, Variant::VisualStudio),
                         [](const testing::TestParamInfo<Variant>& info) {
                             return info.param == Variant::DllCrt ? std::string("CMakeDebug")
                                                                  : std::string("VisualStudioDebug");
                         });

} // namespace
} // namespace frost::debug
