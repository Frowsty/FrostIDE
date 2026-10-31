// SPDX-License-Identifier: Apache-2.0
//
// Offscreen tests for the M0 main window and the in-process GUI self-test
// checks. They run on Qt's offscreen platform (see tests/support/qt_test_main.cpp).

#include "frost/app/main_window.hpp"
#include "frost/app/selftest.hpp"

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
