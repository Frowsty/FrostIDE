// SPDX-License-Identifier: Apache-2.0
// DBG-10 acceptance through Workbench and pinned real lldb-DAP. No simulated frames or outcomes.
#include "frost/debug/lldb_dap.hpp"
#include "frost/io/process.hpp"
#include "frost/ui/debug_retirement.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/debug_sessions_window.hpp"
#include "frost/ui/debug_tool_window.hpp"
#include "frost/ui/run_controller.hpp"

#include "workbench_rig.hpp"
#include <QPushButton>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <future>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>

#include <unistd.h>
#endif
#ifndef FROST_SCENARIOS_PATH
#error "The real multi-session target needs debug_test_scenarios and its source definitions"
#endif
namespace frost::app {
namespace {
using namespace std::chrono_literals;
bool alive(std::int64_t pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process)
        return false;
    const bool result = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return result;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}
class MultiSessionReal : public ::testing::Test {
protected:
    void SetUp() override {
        directory = std::make_unique<test::WorkDir>("multi-real");
        directory->write("workspace/README.txt", "isolated debugger acceptance workspace");
        root = directory->join("workspace");
        rig = std::make_unique<test::WorkbenchRig>(*directory);
        ASSERT_TRUE(rig->trust->trust(root));
        ASSERT_TRUE(rig->workbench->openPath(root));
        program = io::normalizePath(FROST_SCENARIOS_PATH).value();
        source = io::normalizePath(FROST_SCENARIOS_SOURCE).value();
        auto text = io::realFileSystem().readFile(source);
        ASSERT_TRUE(text);
        // The real source is the breakpoint oracle; no copied line number can silently drift.
        std::int64_t number = 1;
        std::size_t start = 0;
        while (start < text->size()) {
            const auto end = text->find('\n', start);
            const auto line =
                std::string_view(*text).substr(start, end == std::string::npos ? text->size() - start : end - start);
            if (line.find("// MARK: spin") != std::string_view::npos) {
                spinLine = number;
                break;
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
            ++number;
        }
        ASSERT_GT(spinLine, 0);
        auto point = rig->workbench->debugController()->breakpoints().add(source, spinLine);
        ASSERT_TRUE(point);
        breakpoint = *point;
    }
    void TearDown() override {
        rig.reset();
        directory.reset();
    }
    ui::DebugSessionRegistry& sessions() { return *rig->workbench->debugSessions(); }
    debug::DebugTarget target(std::string name) {
        return {.name = std::move(name),
                .program = program,
                .arguments = {"wait"},
                .workingDirectory = root,
                .trustRoots = {root}};
    }
    bool stopped(ui::DebugSessionId id) {
        auto* controller = sessions().controller(id);
        return controller && controller->paused() && controller->selectedFrame() && controller->session()->processId();
    }
    std::int64_t pid(ui::DebugSessionId id) { return *sessions().controller(id)->session()->processId(); }
    std::unique_ptr<test::WorkDir> directory;
    std::unique_ptr<test::WorkbenchRig> rig;
    std::string root, program, source;
    std::int64_t spinLine = 0;
    debug::BreakpointId breakpoint = 0;
};

TEST_F(MultiSessionReal, CompoundRunsTwoRealAdaptersAndSelectedResumeCannotResumeTheOther) {
    std::vector<run::RunConfiguration> configurations;
    for (const auto id : {"native.first", "native.second"}) {
        run::RunConfiguration member;
        member.id = id;
        member.name = id;
        member.program = program;
        member.arguments = {"wait"};
        member.workingDirectory = root;
        configurations.push_back(std::move(member));
    }
    run::RunConfiguration compound;
    compound.id = "compound.native";
    compound.name = "Native pair";
    compound.type = run::RunType::Compound;
    compound.configurations = {"native.first", "native.second"};
    configurations.push_back(compound);
    ASSERT_TRUE(rig->workbench->runController()->replaceConfigurations(configurations));
    ASSERT_TRUE(rig->workbench->runController()->selectConfiguration(compound.id));
    ASSERT_EQ(rig->window->invoke("run.debug"), input::InvokeResult::Ran);
    std::vector<ui::DebugSessionId> children;
    ASSERT_TRUE(test::waitFor(
        [&] {
            children.clear();
            for (const auto& entry : sessions().sessions())
                if (entry.group && stopped(entry.id))
                    children.push_back(entry.id);
            return children.size() == compound.configurations.size();
        },
        90s));
    const auto first = children[0], second = children[1];
    const auto firstPid = pid(first), secondPid = pid(second);
    ASSERT_NE(firstPid, secondPid);
    ASSERT_TRUE(alive(firstPid));
    ASSERT_TRUE(alive(secondPid));
    EXPECT_EQ(sessions().controller(first)->selectedFrame()->line, spinLine);
    EXPECT_EQ(sessions().controller(second)->selectedFrame()->line, spinLine);
    EXPECT_NE(sessions().controller(first)->console(), sessions().controller(second)->console());
    ASSERT_TRUE(sessions().select(first));
    rig->workbench->debugController()->breakpoints().remove(breakpoint);
    ASSERT_EQ(rig->window->invoke("debug.resume"), input::InvokeResult::Ran);
    ASSERT_TRUE(
        test::waitFor([&] { return sessions().controller(first)->state() == debug::DebugState::Running; }, 90s));
    EXPECT_TRUE(sessions().controller(second)->paused());
    EXPECT_EQ(sessions().controller(second)->selectedFrame()->line, spinLine);
    ASSERT_TRUE(sessions().controller(first)->pause());
    ASSERT_TRUE(test::waitFor([&] { return sessions().controller(first)->paused(); }, 90s));
    auto* view = rig->window->findChild<ui::DebugSessionsWindow*>();
    ASSERT_NE(view, nullptr);
    auto* stopAll = view->findChild<QPushButton*>(QStringLiteral("debugStopAll"));
    ASSERT_NE(stopAll, nullptr);
    stopAll->click();
    ASSERT_TRUE(test::waitFor([&] { return !sessions().anyDebugging(); }, 90s));
    EXPECT_TRUE(test::waitFor([&] { return !alive(firstPid) && !alive(secondPid); }, 90s));
}

TEST_F(MultiSessionReal, FailedSecondRealLaunchStopsOnlyItsCompoundSiblings) {
    auto unrelated = sessions().startTarget(target("Unrelated"));
    ASSERT_TRUE(unrelated);
    ASSERT_TRUE(test::waitFor([&] { return stopped(*unrelated); }, 90s));
    const auto unrelatedPid = pid(*unrelated);
    const auto group = sessions().createGroup();
    ASSERT_NE(group, 0U);
    auto first = sessions().startTarget(target("First member"), group);
    ASSERT_TRUE(first);
    ASSERT_TRUE(test::waitFor([&] { return stopped(*first); }, 90s));
    const auto firstPid = pid(*first);
    auto missing = target("Missing member");
    missing.program = directory->join("does-not-exist.exe");
    ASSERT_FALSE(io::realFileSystem().exists(missing.program).value_or(true));
    auto second = sessions().startTarget(missing, group);
    if (second) {
        ASSERT_TRUE(test::waitFor(
            [&] {
                auto* controller = sessions().controller(*second);
                return controller && !controller->debugging() && controller->lastOutcome() &&
                       controller->lastOutcome()->error;
            },
            90s));
    }
    sessions().finishPreparing(group);
    ASSERT_TRUE(test::waitFor([&] { return !sessions().controller(*first)->debugging(); }, 90s));
    EXPECT_TRUE(test::waitFor([&] { return !alive(firstPid); }, 90s));
    EXPECT_TRUE(sessions().controller(*unrelated)->paused());
    EXPECT_TRUE(alive(unrelatedPid));
    sessions().stopAll();
    ASSERT_TRUE(test::waitFor([&] { return !sessions().anyDebugging(); }, 90s));
    EXPECT_TRUE(test::waitFor([&] { return !alive(unrelatedPid); }, 90s));
}

TEST_F(MultiSessionReal, ShutdownEndsOwnedLaunchButDetachesTheActualExternalHost) {
    io::ProcessSpec spec;
    spec.executable = program;
    spec.arguments = {"wait"};
    spec.killOnClose = true;
    auto external = io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(external);
    struct Cleanup {
        io::IProcess* process;
        ~Cleanup() {
            static_cast<void>(process->terminate());
            static_cast<void>(process->wait(10s));
        }
    } cleanup{external->get()};
    std::array<char, 256> bytes{};
    auto printed = std::async(std::launch::async, [&] { return (*external)->readStdout(bytes); });
    const auto ready = printed.wait_for(10s);
    if (ready != std::future_status::ready)
        static_cast<void>((*external)->terminate());
    ASSERT_EQ(ready, std::future_status::ready);
    auto count = printed.get();
    ASSERT_TRUE(count);
    ASSERT_NE(std::string_view(bytes.data(), *count).find("pid="), std::string_view::npos);
    const auto externalPid = static_cast<std::int64_t>((*external)->pid());
    auto owned = sessions().startTarget(target("Owned"));
    ASSERT_TRUE(owned);
    ASSERT_TRUE(test::waitFor([&] { return stopped(*owned); }, 90s));
    const auto ownedPid = pid(*owned);
    auto adapter = debug::findLldbDap(io::realFileSystem(), debug::defaultLldbDapSearch());
    ASSERT_TRUE(adapter);
    auto attached =
        sessions().attachTarget(target("Attached"), debug::lldbDapAdapterFactory(io::realProcessLauncher(), *adapter),
                                rpc::Json::object()
                                    .set("type", "lldb-dap")
                                    .set("request", "attach")
                                    .set("pid", externalPid)
                                    .set("program", program),
                                [] { return true; });
    ASSERT_TRUE(attached);
    ASSERT_TRUE(test::waitFor([&] { return stopped(*attached); }, 90s));
    EXPECT_EQ(pid(*attached), externalPid);
    sessions().shutdown();
    ASSERT_TRUE(test::waitFor([&] { return !alive(ownedPid); }, 90s));
    // Retirement completion is an adapter-resource oracle, not an invented host outcome.
    ASSERT_TRUE(test::waitFor([] { return ui::pendingDebugDetachments() == 0; }, 90s));
    EXPECT_TRUE(alive(externalPid));
    const auto status = (*external)->wait(0ms);
    EXPECT_FALSE(status.has_value());
    if (!status)
        EXPECT_EQ(status.error().code(), ErrorCode::Timeout);
}
} // namespace
} // namespace frost::app
