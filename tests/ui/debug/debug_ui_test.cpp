// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost::ui_debug (ADR 0050), offscreen, against the fake-dap double's scripted program:
// DebugController (the trust gate, breakpoints from the gutter and the caret, their persistence,
// the stop's source and execution point, stepping, Stop, a crash stop), the Debug tool window
// (actions, reason, threads, frames, the lazy Variables tree with paged arrays, Watches, the
// console), the Evaluate dialog, the debugger's actions and the tool window registration.

#include "frost/debug/breakpoints.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/input/action.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/rpc/framing.hpp"
#include "frost/ui/debug_actions.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_models.hpp"
#include "frost/ui/debug_tool_window.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/shell/shell_settings.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "debug/debug_test_util.hpp"
#include "editor/editor_harness.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTimer>
#include <QTreeView>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace frost::ui {
namespace {

using namespace std::chrono_literals;
using debug::test::workPath;

// Processes Qt events until `done` or 20 s.
bool spinUntil(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// 30 numbered lines.
std::string sourceText() {
    std::string text;
    for (int line = 1; line <= 30; ++line) {
        text += "line " + std::to_string(line) + "\n";
    }
    return text;
}

class DebugUiTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(fs.createDirectories(workPath("Trusted/x64/Debug")).has_value());
        ASSERT_TRUE(fs.createDirectories(workPath("Untrusted/x64/Debug")).has_value());
        auto store = project::TrustStore::open(fs, workPath("data/trusted-roots.txt"));
        ASSERT_TRUE(store.has_value()) << store.error().describe();
        trust = std::move(*store);
        ASSERT_TRUE(trust->trust(workPath("Trusted")).has_value());
        controller = makeController(breakpoints);
    }

    void TearDown() override {
        controller.reset(); // Before the editor: it tracks the editor's document.
        editor.reset();
    }

    std::unique_ptr<DebugController> makeController(debug::BreakpointManager& manager) {
        DebugControllerOptions options{.trust = trust.get(),
                                       .fs = &fs,
                                       .launcher = nullptr,
                                       .lldbDapSetting = {},
                                       .adapterFactory = [this](debug::DapHandlers handlers)
                                           -> Result<std::unique_ptr<debug::IDebugAdapterClient>> {
                                           ++factoryCalls;
                                           return debug::makeDapClient(adapter.transport(), std::move(handlers));
                                       }};
        DebugControllerHooks hooks{
            .openSource = [this](const QString& path, int line) -> EditorView* {
                opened.emplace_back(path, line);
                return editor ? editor->view.get() : nullptr;
            },
            .currentEditor = [this]() -> EditorView* { return editor ? editor->view.get() : nullptr; },
            .notify = [this](const QString&, const QString& text, bool error) { notes.emplace_back(text, error); },
            .showToolWindow = [this] { ++shown; }};
        return std::make_unique<DebugController>(manager, std::move(options), std::move(hooks));
    }

    void openEditor() {
        test::HarnessOptions options;
        options.path = source;
        editor = std::make_unique<test::EditorHarness>(sourceText(), options);
    }

    [[nodiscard]] debug::DebugTarget target() const {
        return {.name = "App [Debug|x64]",
                .program = workPath("Trusted/x64/Debug/App.exe"),
                .trustRoots = {workPath("Trusted")}};
    }

    // Starts the scripted program and waits for its first stop.
    void startAndStop(debug::FakeProgram program) {
        program.sourcePath = source;
        debug::scriptProgram(adapter, std::move(program));
        auto started = controller->startTarget(target());
        ASSERT_TRUE(started.has_value()) << started.error().describe();
        ASSERT_TRUE(spinUntil([&] { return controller->paused() && controller->selectedFrame().has_value(); }));
    }

    // Stops the session and waits for its end.
    void stopSession() {
        controller->stop();
        ASSERT_TRUE(spinUntil([&] { return !controller->debugging(); }));
    }

    const std::string source = workPath("Trusted/main.cpp");
    ManualClock clock;
    io::MemFs fs{clock};
    std::unique_ptr<project::TrustStore> trust;
    debug::FakeDebugAdapter adapter;
    debug::BreakpointManager breakpoints;
    std::unique_ptr<test::EditorHarness> editor;
    std::vector<std::pair<QString, int>> opened;
    std::vector<std::pair<QString, bool>> notes;
    int shown = 0;
    int factoryCalls = 0;
    std::unique_ptr<DebugController> controller;
};

TEST_F(DebugUiTest, ShutdownRejectsADelayedAlternateBuildCompletion) {
    std::function<void(Result<debug::DebugTarget>)> completion;
    int stopped = 0;
    DebugControllerOptions options{
        .trust = trust.get(),
        .fs = &fs,
        .adapterFactory = [this](debug::DapHandlers handlers) -> Result<std::unique_ptr<debug::IDebugAdapterClient>> {
            ++factoryCalls;
            return debug::makeDapClient(adapter.transport(), std::move(handlers));
        }};
    DebugControllerHooks hooks;
    hooks.alternateAvailable = [] { return true; };
    hooks.startAlternate = [&](auto done) -> Result<void> {
        completion = std::move(done);
        return {};
    };
    hooks.stopAlternate = [&] { ++stopped; };
    DebugController pending(breakpoints, std::move(options), std::move(hooks));
    ASSERT_TRUE(pending.debug().has_value());
    ASSERT_TRUE(pending.debugging());
    pending.shutdown();
    ASSERT_TRUE(completion);
    completion(target());
    EXPECT_EQ(stopped, 1);
    EXPECT_EQ(factoryCalls, 0);
    EXPECT_FALSE(pending.debugging());
    EXPECT_EQ(pending.debug().error().code(), ErrorCode::Cancelled);
}

TEST_F(DebugUiTest, OpeningAWorkspaceDoesNotRewriteItsBreakpointsWhileLoading) {
    ASSERT_TRUE(controller->setWorkspaceRoot(workPath("Trusted")).has_value());
    controller->toggleBreakpoint(source, 2);
    controller->toggleBreakpoint(source, 8);
    const std::string path = workPath("Trusted/.frost/workspace.json");
    auto before = fs.readFile(path);
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(controller->setWorkspaceRoot({}).has_value());
    ASSERT_TRUE(controller->setWorkspaceRoot(workPath("Trusted")).has_value());
    ASSERT_EQ(breakpoints.size(), 2U);
    EXPECT_EQ(fs.readFile(path).value(), *before);
}

TEST_F(DebugUiTest, AnUntrustedRootIsRefusedBeforeLldbDapStarts) {
    debug::DebugTarget untrusted = target();
    untrusted.program = workPath("Untrusted/x64/Debug/App.exe");
    untrusted.trustRoots = {workPath("Untrusted")};
    auto refused = controller->startTarget(untrusted);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(factoryCalls, 0) << "no adapter was started";
    EXPECT_FALSE(controller->debugging());
    EXPECT_EQ(controller->debug().error().code(), ErrorCode::InvalidArgument) << "no solution is open";

    // Without a test factory the controller looks for lldb-dap; a missing one is an error, not a launch.
    DebugController plain(breakpoints, DebugControllerOptions{.trust = trust.get(), .fs = &fs});
    EXPECT_FALSE(plain.startTarget(target()).has_value());
    EXPECT_FALSE(plain.debugging());
}

TEST_F(DebugUiTest, AStopOpensTheSourceShowsTheExecutionPointAndSteps) {
    FROST_REQUIRE_OFFSCREEN();
    openEditor();
    controller->attachEditor(editor->view.get());
    EXPECT_TRUE(editor->view->breakpointGutterEnabled());
    emit editor->view->breakpointToggleRequested(9);  // Line 10: code.
    emit editor->view->breakpointToggleRequested(10); // Line 11: no code there.
    ASSERT_EQ(breakpoints.size(), 2U);
    ASSERT_EQ(editor->view->breakpointMarkers().size(), 2U);

    DebugToolWindow window(*controller);
    EXPECT_FALSE(window.resumeAction()->isEnabled());
    EXPECT_FALSE(window.stopAction()->isEnabled());
    startAndStop(debug::FakeProgram{.codeLines = {10, 20}});
    EXPECT_GE(shown, 1) << "the Debug tool window opens";
    ASSERT_FALSE(opened.empty());
    EXPECT_EQ(opened.back(), (std::pair{QString::fromStdString(source), 10}));
    EXPECT_EQ(editor->view->executionPoint(), std::optional<LineIndex>(LineIndex{9}));
    EXPECT_EQ(controller->statusText(), QStringLiteral("Paused on breakpoint"));
    EXPECT_EQ(controller->title(), QStringLiteral("App [Debug|x64]"));

    // The adapter's verdicts reach the gutter: a filled disc, and a hollow one with the reason.
    ASSERT_TRUE(spinUntil([&] { return breakpoints.at(source, 11)->status == debug::BreakpointStatus::Unverified; }));
    EXPECT_EQ(breakpoints.at(source, 10)->status, debug::BreakpointStatus::Verified);
    const std::vector<BreakpointMarker> markers = editor->view->breakpointMarkers();
    ASSERT_EQ(markers.size(), 2U);
    EXPECT_EQ(markers[0].kind, BreakpointKind::Verified);
    EXPECT_EQ(markers[1].kind, BreakpointKind::Unverified);
    EXPECT_EQ(markers[1].message, "No code is generated for this line");

    // The tool window: actions, the reason, the threads, the frames.
    EXPECT_TRUE(window.resumeAction()->isEnabled());
    EXPECT_FALSE(window.pauseAction()->isEnabled());
    EXPECT_TRUE(window.stopAction()->isEnabled());
    EXPECT_TRUE(window.stepOverAction()->isEnabled());
    EXPECT_TRUE(window.stepIntoAction()->isEnabled());
    EXPECT_TRUE(window.stepOutAction()->isEnabled());
    EXPECT_EQ(window.reasonLabel()->text(), QStringLiteral("Paused on breakpoint"));
    ASSERT_TRUE(spinUntil([&] { return window.threadCombo()->count() == 2; }));
    EXPECT_EQ(window.threadCombo()->currentText(), QStringLiteral("main (1)"));
    ASSERT_EQ(window.framesModel()->rowCount(), 3);
    EXPECT_EQ(window.framesModel()->selectedRow(), 0);
    EXPECT_EQ(window.framesModel()->data(window.framesModel()->index(0), Qt::DisplayRole).toString(),
              QStringLiteral("main(), main.cpp:10"));

    // Variables: the scopes, Locals' children, a paged array.
    VariablesModel& variables = *window.variablesModel();
    ASSERT_TRUE(spinUntil([&] { return variables.rowCount() == 2 && variables.pendingRequests() == 0; }));
    const QModelIndex locals = variables.index(0, 0);
    EXPECT_EQ(variables.data(locals, Qt::DisplayRole).toString(), QStringLiteral("Locals"));
    EXPECT_TRUE(variables.hasChildren(locals));
    variables.fetchMore(locals); // The tool window may have expanded it already; either way it loads once.
    ASSERT_TRUE(spinUntil([&] { return variables.rowCount(locals) == 3 && variables.pendingRequests() == 0; }));
    const QModelIndex x = variables.index(0, 0, locals);
    EXPECT_EQ(variables.data(x, Qt::DisplayRole).toString(), QStringLiteral("x"));
    EXPECT_EQ(variables.data(x.siblingAtColumn(1), Qt::DisplayRole).toString(), QStringLiteral("42"));
    EXPECT_EQ(variables.data(x.siblingAtColumn(2), Qt::DisplayRole).toString(), QStringLiteral("int"));
    EXPECT_FALSE(variables.hasChildren(x));
    window.variablesView()->setCurrentIndex(x);
    window.copySelectedValue();
    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("42"));

    const QModelIndex values = variables.index(2, 0, locals);
    ASSERT_TRUE(variables.canFetchMore(values));
    variables.fetchMore(values);
    ASSERT_TRUE(spinUntil([&] { return variables.rowCount(values) == 101 && variables.pendingRequests() == 0; }));
    ASSERT_TRUE(variables.isLoadMoreRow(variables.index(100, 0, values)));
    variables.loadMore(variables.index(100, 0, values));
    ASSERT_TRUE(spinUntil([&] { return variables.rowCount(values) == 201 && variables.pendingRequests() == 0; }));
    variables.loadMore(variables.index(200, 0, values));
    ASSERT_TRUE(spinUntil([&] { return variables.rowCount(values) == 250 && variables.pendingRequests() == 0; }));
    EXPECT_EQ(variables.data(variables.index(249, 1, values), Qt::DisplayRole).toString(), QStringLiteral("2490"));
    EXPECT_FALSE(variables.isLoadMoreRow(variables.index(249, 0, values)));

    // Watches: added from the input line, evaluated at once; a failing one shows the reason.
    VariablesModel& watches = *window.watchesModel();
    window.watchInput()->setText(QStringLiteral("x + 1"));
    emit window.watchInput()->returnPressed();
    watches.addWatch(QStringLiteral("nope"));
    ASSERT_TRUE(spinUntil([&] { return watches.rowCount() == 2 && watches.pendingRequests() == 0; }));
    EXPECT_TRUE(window.watchInput()->text().isEmpty());
    EXPECT_EQ(watches.watches(), (QStringList{QStringLiteral("x + 1"), QStringLiteral("nope")}));
    EXPECT_EQ(watches.data(watches.index(0, 1), Qt::DisplayRole).toString(), QStringLiteral("43"));
    EXPECT_EQ(watches.data(watches.index(1, 1), Qt::DisplayRole).toString(),
              QStringLiteral("evaluate: use of undeclared identifier 'nope'"));
    watches.removeWatch(1);
    EXPECT_EQ(watches.rowCount(), 1);

    // The program's output is in the console.
    EXPECT_TRUE(window.console()->text().contains(QStringLiteral("hello from the debuggee")))
        << window.console()->text().toStdString();

    // Step Over: the next line, the watches evaluated again.
    const std::size_t evaluations = adapter.argumentsOf("evaluate").size();
    window.stepOverAction()->trigger();
    ASSERT_TRUE(spinUntil([&] {
        return controller->paused() && controller->selectedFrame() && controller->selectedFrame()->line == 11;
    }));
    EXPECT_EQ(controller->stopInfo()->reason, "step");
    EXPECT_EQ(editor->view->executionPoint(), std::optional<LineIndex>(LineIndex{10}));
    EXPECT_EQ(window.reasonLabel()->text(), QStringLiteral("Paused after a step"));
    ASSERT_TRUE(spinUntil([&] { return adapter.argumentsOf("evaluate").size() > evaluations; }))
        << "the watches are evaluated on each stop";

    // Stop ends the session: no execution point, the breakpoints back to "no session".
    window.stopAction()->trigger();
    ASSERT_TRUE(spinUntil([&] { return !controller->debugging(); }));
    EXPECT_TRUE(adapter.waitForRequest("disconnect"));
    EXPECT_EQ(adapter.argumentsOf("disconnect").front().boolAt("terminateDebuggee"), true);
    EXPECT_FALSE(editor->view->executionPoint().has_value());
    EXPECT_EQ(breakpoints.at(source, 11)->status, debug::BreakpointStatus::Unknown);
    EXPECT_EQ(editor->view->breakpointMarkers()[1].kind, BreakpointKind::Verified)
        << "without a session every breakpoint is drawn filled";
    EXPECT_FALSE(window.stopAction()->isEnabled());
    EXPECT_FALSE(window.resumeAction()->isEnabled());
    EXPECT_TRUE(window.console()->text().contains(QStringLiteral("Process stopped")));
    EXPECT_FALSE(window.console()->text().contains(QStringLiteral("before the program started")));
    EXPECT_EQ(window.variablesModel()->rowCount(), 0);
    EXPECT_TRUE(notes.empty()) << notes.front().first.toStdString();
}

TEST_F(DebugUiTest, OldWatchRepliesCannotOverwriteNewFrameOrRunningState) {
    FROST_REQUIRE_OFFSCREEN();
    controller->toggleBreakpoint(source, 9);
    startAndStop(debug::FakeProgram{.codeLines = {10}, .runsForever = true});
    adapter.on("evaluate", [](const rpc::Json&) { return debug::FakeDebugAdapter::Reply{.respond = false}; });
    VariablesModel watches(*controller, VariablesModel::Mode::Watches);
    watches.addWatch(QStringLiteral("x"));
    ASSERT_TRUE(adapter.waitForRequest("evaluate", 1));
    controller->selectFrame(1001);
    ASSERT_TRUE(spinUntil([&] { return adapter.argumentsOf("evaluate").size() >= 2; }));
    const auto answer = [&](int which, const char* value) {
        std::vector<rpc::Json> requests;
        for (const auto& request : adapter.requests())
            if (request.stringAt("command") == "evaluate")
                requests.push_back(request);
        const auto seq = requests.at(static_cast<std::size_t>(which)).integerAt("seq").value();
        adapter.sendRaw(
            rpc::encodeFrame(rpc::Json::object()
                                 .set("seq", 900 + which)
                                 .set("type", "response")
                                 .set("request_seq", seq)
                                 .set("command", "evaluate")
                                 .set("success", true)
                                 .set("body", rpc::Json::object().set("result", value).set("variablesReference", 0))
                                 .dump()));
    };
    answer(1, "new");
    ASSERT_TRUE(spinUntil([&] { return watches.valueAt(watches.index(0, 1)) == QStringLiteral("new"); }));
    answer(0, "old");
    ASSERT_TRUE(spinUntil([&] { return watches.pendingRequests() == 0; }));
    EXPECT_EQ(watches.valueAt(watches.index(0, 1)), QStringLiteral("new"));
    watches.reload();
    ASSERT_TRUE(adapter.waitForRequest("evaluate", 3));
    ASSERT_TRUE(controller->resume());
    ASSERT_TRUE(spinUntil([&] { return !controller->paused(); }));
    answer(2, "stale");
    ASSERT_TRUE(spinUntil([&] { return watches.pendingRequests() == 0; }));
    EXPECT_NE(watches.valueAt(watches.index(0, 1)), QStringLiteral("stale"));
    stopSession();
}

TEST_F(DebugUiTest, OldScopesAndWatchChildrenAreDiscardedAfterFrameChange) {
    FROST_REQUIRE_OFFSCREEN();
    controller->toggleBreakpoint(source, 9);
    startAndStop(debug::FakeProgram{.codeLines = {10}, .runsForever = true});
    adapter.on("scopes", [](const rpc::Json&) { return debug::FakeDebugAdapter::Reply{.respond = false}; });
    adapter.on("variables", [](const rpc::Json&) { return debug::FakeDebugAdapter::Reply{.respond = false}; });
    adapter.on("evaluate", [](const rpc::Json&) { return debug::FakeDebugAdapter::Reply{.respond = false}; });
    const auto answer = [&](std::string_view command, std::size_t which, rpc::Json body) {
        std::vector<rpc::Json> requests;
        for (const auto& request : adapter.requests())
            if (request.stringAt("command") == command)
                requests.push_back(request);
        const auto seq = requests.at(which).integerAt("seq").value();
        adapter.sendRaw(rpc::encodeFrame(rpc::Json::object()
                                             .set("seq", static_cast<int>(900 + which))
                                             .set("type", "response")
                                             .set("request_seq", seq)
                                             .set("command", command)
                                             .set("success", true)
                                             .set("body", std::move(body))
                                             .dump()));
    };
    VariablesModel scopes(*controller, VariablesModel::Mode::Scopes);
    scopes.reload();
    ASSERT_TRUE(adapter.waitForRequest("scopes", 1));
    controller->selectFrame(1001);
    ASSERT_TRUE(spinUntil([&] { return adapter.argumentsOf("scopes").size() >= 2; }));
    const auto scopeBody = [](const char* name) {
        return rpc::Json::object().set(
            "scopes", rpc::Json::array({rpc::Json::object().set("name", name).set("variablesReference", 5)}));
    };
    answer("scopes", 1, scopeBody("new scope"));
    ASSERT_TRUE(spinUntil([&] { return scopes.rowCount() == 1; }));
    answer("scopes", 0, scopeBody("old scope"));
    ASSERT_TRUE(spinUntil([&] { return scopes.pendingRequests() == 0; }));
    EXPECT_EQ(scopes.data(scopes.index(0, 0), Qt::DisplayRole).toString(), QStringLiteral("new scope"));

    VariablesModel watches(*controller, VariablesModel::Mode::Watches);
    watches.addWatch(QStringLiteral("obj"));
    ASSERT_TRUE(adapter.waitForRequest("evaluate", 1));
    const auto watchBody = [](const char* value) {
        return rpc::Json::object().set("result", value).set("variablesReference", 55);
    };
    answer("evaluate", 0, watchBody("old object"));
    ASSERT_TRUE(spinUntil([&] { return watches.canFetchMore(watches.index(0, 0)); }));
    watches.fetchMore(watches.index(0, 0));
    ASSERT_TRUE(adapter.waitForRequest("variables", 1));
    controller->selectFrame(1000);
    ASSERT_TRUE(spinUntil([&] { return adapter.argumentsOf("evaluate").size() >= 2; }));
    answer("evaluate", 1, watchBody("new object"));
    ASSERT_TRUE(spinUntil([&] { return watches.valueAt(watches.index(0, 1)) == QStringLiteral("new object"); }));
    answer("variables", 0,
           rpc::Json::object().set(
               "variables",
               rpc::Json::array(
                   {rpc::Json::object().set("name", "old child").set("value", "wrong").set("variablesReference", 0)})));
    ASSERT_TRUE(spinUntil([&] { return watches.pendingRequests() == 0; }));
    EXPECT_EQ(watches.rowCount(watches.index(0, 0)), 0);
    stopSession();
}

TEST_F(DebugUiTest, CopyValueSurvivesModelResetWhileContextMenuIsOpen) {
    FROST_REQUIRE_OFFSCREEN();
    DebugToolWindow window(*controller);
    window.show();
    controller->toggleBreakpoint(source, 9);
    startAndStop(debug::FakeProgram{.codeLines = {10}, .runsForever = true});
    auto& model = *window.variablesModel();
    ASSERT_TRUE(spinUntil([&] { return model.rowCount() > 0; }));
    const QModelIndex root = model.index(0, 0);
    model.fetchMore(root);
    ASSERT_TRUE(spinUntil([&] { return model.rowCount(root) > 0; }));
    auto* view = window.variablesView();
    view->expand(root);
    const QModelIndex child = model.index(0, 0, root);
    const QString expected = model.valueAt(child.siblingAtColumn(1));
    const QPoint at = view->visualRect(child).center();
    QTimer::singleShot(0, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        ASSERT_NE(menu, nullptr);
        model.reload();
        menu->actions().front()->trigger();
        menu->close();
    });
    emit view->customContextMenuRequested(at);
    EXPECT_EQ(QApplication::clipboard()->text(), expected);
    stopSession();
}

TEST_F(DebugUiTest, ACrashStopSelectsTheFaultingFrameAndNotifies) {
    FROST_REQUIRE_OFFSCREEN();
    openEditor();
    DebugToolWindow window(*controller);
    QString stopSummary;
    bool crashed = false;
    QObject::connect(controller.get(), &DebugController::stopped, [&](const QString& summary, bool crash) {
        stopSummary = summary;
        crashed = crash;
    });
    startAndStop(
        debug::FakeProgram{.codeLines = {},
                           .entryLine = 25,
                           .crashReason = "Exception 0xc0000005 encountered at address 0x0: Access violation"});
    EXPECT_TRUE(crashed);
    EXPECT_EQ(stopSummary,
              QStringLiteral("Exception: Exception 0xc0000005 encountered at address 0x0: Access violation"));
    EXPECT_EQ(window.reasonLabel()->text(), stopSummary);
    EXPECT_EQ(controller->selectedFrame()->name, "crash()") << "the frame with a source, not the runtime's";
    EXPECT_EQ(window.framesModel()->selectedRow(), 1);
    EXPECT_TRUE(window.framesModel()
                    ->data(window.framesModel()->index(0), Qt::DisplayRole)
                    .toString()
                    .contains(QStringLiteral("ucrtbase.dll")));
    ASSERT_FALSE(opened.empty());
    EXPECT_EQ(opened.back().second, 25);
    EXPECT_EQ(editor->view->executionPoint(), std::optional<LineIndex>(LineIndex{24}));
    ASSERT_EQ(notes.size(), 1U);
    EXPECT_TRUE(notes[0].second);
    EXPECT_TRUE(notes[0].first.contains(QStringLiteral("Access violation")));
    stopSession();
}

TEST_F(DebugUiTest, BreakpointsToggleAtTheCaretMoveWithTypingAndPersist) {
    FROST_REQUIRE_OFFSCREEN();
    ASSERT_TRUE(controller->setWorkspaceRoot(workPath("Trusted")).has_value());
    openEditor();
    for (int i = 0; i < 4; ++i) {
        editor->press(Qt::Key_Down);
    }
    EXPECT_TRUE(controller->toggleBreakpointAtCaret());
    ASSERT_TRUE(breakpoints.at(source, 5).has_value()) << "Ctrl+F8 on the caret's line";
    const std::string workspace = workPath("Trusted/.frost/workspace.json");
    EXPECT_NE(fs.readFile(workspace).value_or("").find("{\"file\": \"main.cpp\", \"line\": 5}"), std::string::npos)
        << fs.readFile(workspace).value_or("");

    // A line typed above moves it down, and the new line is saved.
    editor->press(Qt::Key_Home, Qt::ControlModifier);
    editor->type("x");
    editor->press(Qt::Key_Return);
    ASSERT_TRUE(spinUntil([&] { return breakpoints.at(source, 6).has_value(); })) << editor->text().substr(0, 30);
    EXPECT_NE(fs.readFile(workspace).value_or("").find("{\"file\": \"main.cpp\", \"line\": 6}"), std::string::npos);

    // Another controller over a fresh manager loads it back.
    debug::BreakpointManager reloaded;
    auto other = makeController(reloaded);
    ASSERT_TRUE(other->setWorkspaceRoot(workPath("Trusted")).has_value());
    ASSERT_EQ(reloaded.size(), 1U);
    EXPECT_TRUE(reloaded.at(source, 6).has_value());
    other.reset();

    // Toggling it off (the gutter's 0-based line) removes it from the file.
    controller->toggleBreakpoint(source, 5);
    EXPECT_EQ(breakpoints.size(), 0U);
    EXPECT_EQ(fs.readFile(workspace).value_or("").find("breakpoints"), std::string::npos);
}

TEST_F(DebugUiTest, EvaluateShowsAnExpandableResult) {
    FROST_REQUIRE_OFFSCREEN();
    ASSERT_TRUE(breakpoints.add(source, 10).has_value());
    startAndStop(debug::FakeProgram{.codeLines = {10}});
    EvaluateDialog dialog(*controller, QStringLiteral("p"));
    EXPECT_EQ(dialog.expression()->text(), QStringLiteral("p"));
    dialog.evaluate();
    VariablesModel& result = *dialog.result();
    ASSERT_TRUE(spinUntil([&] { return result.rowCount() == 1 && result.pendingRequests() == 0; }));
    const QModelIndex root = result.index(0, 0);
    EXPECT_EQ(result.data(root.siblingAtColumn(1), Qt::DisplayRole).toString(), QStringLiteral("{x:1, y:2}"));
    EXPECT_EQ(result.data(root.siblingAtColumn(2), Qt::DisplayRole).toString(), QStringLiteral("Point"));
    ASSERT_TRUE(result.canFetchMore(root));
    result.fetchMore(root);
    ASSERT_TRUE(spinUntil([&] { return result.rowCount(root) == 2 && result.pendingRequests() == 0; }));
    EXPECT_EQ(adapter.argumentsOf("evaluate").back().stringAt("context"), "repl");

    // A second expression replaces the result.
    dialog.expression()->setText(QStringLiteral("x"));
    dialog.evaluate();
    ASSERT_TRUE(spinUntil([&] {
        return result.rowCount() == 1 && result.pendingRequests() == 0 &&
               result.data(result.index(0, 1), Qt::DisplayRole).toString() == QStringLiteral("42");
    }));
    stopSession();
}

TEST_F(DebugUiTest, TheActionsFollowTheSessionAndTheToolWindowRegisters) {
    FROST_REQUIRE_OFFSCREEN();
    input::ActionRegistry registry;
    ASSERT_TRUE(registerDebugActions(registry, *controller).has_value());
    EXPECT_EQ(registerDebugActions(registry, *controller).error().code(), ErrorCode::AlreadyExists);
    for (const DebugActionInfo& info : kDebugActions) {
        EXPECT_TRUE(registry.find(info.id).has_value()) << info.id;
    }
    const input::ActionContext context{};
    const auto enabled = [&](std::string_view id) { return registry.isEnabled(*registry.find(id), context); };
    const auto invoke = [&](std::string_view id) { return registry.tryInvoke(*registry.find(id), context); };
    EXPECT_FALSE(enabled("run.debug")) << "no run service";
    EXPECT_FALSE(enabled("debug.resume"));
    EXPECT_FALSE(enabled("debug.stepOver"));

    openEditor();
    EXPECT_EQ(invoke("debug.toggleBreakpoint"), input::InvokeResult::Ran);
    EXPECT_TRUE(breakpoints.at(source, 1).has_value());
    debug::scriptProgram(adapter, debug::FakeProgram{.sourcePath = source, .codeLines = {1, 4}, .runsForever = true});
    ASSERT_TRUE(controller->startTarget(target()).has_value());
    ASSERT_TRUE(spinUntil([&] { return controller->paused() && controller->selectedFrame().has_value(); }));
    EXPECT_TRUE(enabled("debug.resume"));
    EXPECT_TRUE(enabled("debug.stepInto"));
    EXPECT_FALSE(enabled("debug.pause"));
    EXPECT_FALSE(enabled("run.debug")) << "one session at a time";
    EXPECT_EQ(invoke("debug.stepInto"), input::InvokeResult::Ran);
    ASSERT_TRUE(spinUntil(
        [&] { return controller->paused() && controller->selectedFrame() && controller->selectedFrame()->line == 2; }));
    // Run to Cursor: the caret's line (it is on line 1; move it to line 4).
    for (int i = 0; i < 3; ++i) {
        editor->press(Qt::Key_Down);
    }
    EXPECT_EQ(invoke("debug.runToCursor"), input::InvokeResult::Ran);
    ASSERT_TRUE(spinUntil(
        [&] { return controller->paused() && controller->selectedFrame() && controller->selectedFrame()->line == 4; }));
    EXPECT_EQ(controller->stopInfo()->reason, "breakpoint");
    EXPECT_FALSE(breakpoints.at(source, 4).has_value()) << "Run to Cursor adds no breakpoint of the user's";

    // The tool window: id "debug", window.debug shows it with a DebugToolWindow inside.
    QWidget host;
    host.resize(800, 600);
    auto* center = new QWidget(&host);
    InMemoryShellSettings shellSettings;
    ToolWindowManager windows(host, *center, shellSettings);
    ASSERT_TRUE(registerDebugToolWindow(windows, registry, *controller).has_value());
    EXPECT_TRUE(windows.contains(kDebugToolWindowId));
    EXPECT_EQ(invoke(kDebugWindowActionId), input::InvokeResult::Ran);
    EXPECT_NE(dynamic_cast<DebugToolWindow*>(windows.content(kDebugToolWindowId)), nullptr);
    stopSession();
}

TEST(DebugTargetFor, CarriesTheResolvedRun) {
    build::RunOutcome outcome;
    outcome.run.projectName = "App";
    outcome.run.configuration = {.configuration = "Debug", .platform = "x64"};
    build::DebugLaunch launch;
    launch.command.executable = workPath("Run/x64/Debug/App.exe");
    launch.command.arguments = "--flag \"two words\"";
    launch.command.workingDirectory = workPath("Run/App");
    launch.command.environment = {{"FROST_RUN_TEST", "1"}};
    launch.command.mergeEnvironment = false;
    launch.trustRoots = {workPath("Run")};
    const debug::DebugTarget target = debugTargetFor(outcome, launch);
    EXPECT_EQ(target.name, "App [Debug|x64]");
    EXPECT_EQ(target.program, launch.command.executable);
    EXPECT_EQ(target.arguments, (std::vector<std::string>{"--flag", "two words"}));
    EXPECT_EQ(target.workingDirectory, workPath("Run/App"));
    EXPECT_EQ(target.environment, launch.command.environment);
    EXPECT_FALSE(target.inheritEnvironment);
    EXPECT_EQ(target.trustRoots, launch.trustRoots);
}

} // namespace
} // namespace frost::ui
