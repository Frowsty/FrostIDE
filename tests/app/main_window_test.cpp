// SPDX-License-Identifier: Apache-2.0
//
// Offscreen tests for the main window (with its navigation workbench) and the in-process GUI
// self-test checks. They run on Qt's offscreen platform (see tests/support/qt_test_main.cpp).

#include "frost/app/main_window.hpp"
#include "frost/app/selftest.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "support/scratch_dir.hpp"
#include <QAction>
#include <QApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QWindow>
#include <gtest/gtest.h>

namespace {

TEST(MainWindow, HasTitleMenuAndStatusBar) {
    frost::app::MainWindow window;
    EXPECT_EQ(window.windowTitle(), QStringLiteral("FrostIDE"));
    EXPECT_NE(window.findChild<QAction*>(QStringLiteral("actionExit")), nullptr);
    EXPECT_NE(window.findChild<QLabel*>(QStringLiteral("placeholder")), nullptr);
    EXPECT_EQ(window.statusBar()->currentMessage(), QStringLiteral("Ready"));
}

TEST(MainWindow, ShowsAndClosesThroughExitAction) {
    frost::app::MainWindow window;
    window.show();
    // isVisible() turns true synchronously in show(); exposure means the platform
    // plugin created and exposed a native window.
    const QDeadlineTimer deadline(5000);
    while (!(window.windowHandle() != nullptr && window.windowHandle()->isExposed()) && !deadline.hasExpired()) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    ASSERT_NE(window.windowHandle(), nullptr);
    EXPECT_TRUE(window.windowHandle()->isExposed());
    EXPECT_TRUE(window.isVisible());
    window.findChild<QAction*>(QStringLiteral("actionExit"))->trigger();
    QApplication::processEvents();
    EXPECT_FALSE(window.isVisible());
}

// The default window carries the navigation workbench (ADR 0042): its File System tool window and
// its actions are there, and Go to File runs without a project.
TEST(MainWindow, HasTheNavigationWorkbench) {
    frost::app::MainWindow window;
    ASSERT_NE(window.workbench(), nullptr);
    EXPECT_TRUE(window.toolWindows().contains(frost::ui::kFileSystemToolWindow));
    EXPECT_EQ(window.invoke(frost::ui::workbench_actions::kGoToFile), frost::input::InvokeResult::Ran);
    ASSERT_NE(window.activePopup(), nullptr);
    EXPECT_EQ(window.activePopup()->objectName(), QStringLiteral("goToFilePopup"));
}

/*
 * The program's persistent services (ADR 0042) on a scratch data directory: a project opened in one
 * run, with a file in a tab, comes back at the next start-up; the real file watcher, reactor and
 * thread pool start with the services and stop with them.
 */
TEST(MainWindow, PersistentServicesRestoreTheLastProject) {
    const frost::test::ScratchDir scratch("app-session");
    const std::string project = scratch.join("proj");
    const std::string file = frost::io::joinPath(project, "main.cpp").value();
    frost::io::IFileSystem& fs = frost::io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(project).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(file, "int main() {}\n").has_value());
    const frost::app::AppServicesOptions options{.persistent = true, .dataDirectory = scratch.join("data")};
    {
        frost::app::AppServices services(options);
        EXPECT_TRUE(services.startupWarnings().empty()) << services.startupWarnings().front();
        frost::app::MainWindow window(services);
        ASSERT_TRUE(window.workbench()->openProject(project).has_value());
        ASSERT_TRUE(window.workbench()->openFile(file).has_value());
        QApplication::processEvents();
    }
    frost::app::AppServices services(options);
    frost::app::MainWindow window(services);
    EXPECT_TRUE(window.workbench()->restoreLastProject());
    ASSERT_EQ(window.tabs().count(), 1);
    EXPECT_TRUE(window.tabs().editor(0)->path().ends_with("main.cpp")) << window.tabs().editor(0)->path();
    // No journal was left behind by the first run's clean exit.
    auto scan = services.scanRecoverableJournals();
    ASSERT_TRUE(scan.has_value()) << scan.error().describe();
    EXPECT_TRUE(scan->journals.empty());
}

TEST(SelfTest, GuiChecksPassOffscreen) {
    frost::app::SelfTestReport report;
    report.checks = frost::app::runGuiChecks();
    EXPECT_TRUE(report.passed()) << report.format();
}

TEST(SelfTest, ReportFormatsEveryCheckAndSummary) {
    frost::app::SelfTestReport report;
    report.checks = {{"a", true, true, "fine"}, {"b", false, false, "informational"}};
    EXPECT_TRUE(report.passed());
    const std::string text = report.format();
    EXPECT_NE(text.find("selftest: ok   a: fine"), std::string::npos) << text;
    EXPECT_NE(text.find("selftest: warn b: informational"), std::string::npos) << text;
    EXPECT_NE(text.find("selftest: PASSED"), std::string::npos) << text;

    report.checks.push_back({"c", false, true, "broken"});
    EXPECT_FALSE(report.passed());
    EXPECT_NE(report.format().find("FAILED (1 required checks)"), std::string::npos);
}

} // namespace
