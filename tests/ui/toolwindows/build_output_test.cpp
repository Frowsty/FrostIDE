// SPDX-License-Identifier: Apache-2.0
//
// Tests for the Build output pane (frost/ui/toolwindows/build_output.hpp): BuildOutputLog's line
// styles, diagnostic links and summary lines, and the pane streaming a build from the controller's
// marshalled callbacks, clickable diagnostic lines, tail following and Clear. Offscreen.
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/build_output.hpp"

#include "toolwindows/toolwindow_test_util.hpp"
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QToolButton>
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

namespace frost::ui {
namespace {

using build::BuildOutputKind;
using test::FakeBuildBackend;
using test::runPostedEvents;

#ifdef _WIN32
constexpr std::string_view kMain = R"(C:\src\App\main.cpp)";
#else
constexpr std::string_view kMain = "/src/App/main.cpp";
#endif

// The line MSBuild prints for the diagnostic below.
std::string errorLine() { return std::string(kMain) + "(7,27): error C2065: 'x': undeclared identifier [App.vcxproj]"; }

build::BuildDiagnostic errorDiagnostic() {
    auto parsed = build::parseDiagnosticLine(errorLine());
    EXPECT_TRUE(parsed.has_value());
    build::BuildDiagnostic d;
    d.diagnostic = parsed.value_or(build::Diagnostic{});
    d.path = std::string(kMain);
    d.jumpable = true;
    return d;
}

build::BuildOutcome outcome(bool success, std::chrono::milliseconds duration) {
    build::BuildOutcome result;
    result.result.success = success;
    result.result.duration = duration;
    return result;
}

TEST(BuildOutputLog, StylesLinesAndLinksDiagnosticsInEitherOrder) {
    BuildOutputLog log;
    EXPECT_EQ(log.append(BuildOutputKind::Status, "Build started: SdkApp, Debug | x64"), 0U);
    EXPECT_EQ(log.append(BuildOutputKind::Stdout, "  core.cpp\r"), 1U);
    const std::size_t error = log.append(BuildOutputKind::Stdout, errorLine());
    const std::size_t warning =
        log.append(BuildOutputKind::Stdout, std::string(kMain) + "(3): warning C4100: 'argc': unreferenced parameter");
    EXPECT_EQ(log.line(0).style, OutputLineStyle::Status);
    EXPECT_EQ(log.line(1).style, OutputLineStyle::Plain);
    EXPECT_EQ(log.line(1).text, "  core.cpp"); // Without the CR.
    EXPECT_EQ(log.line(error).style, OutputLineStyle::Error);
    EXPECT_EQ(log.line(warning).style, OutputLineStyle::Warning);
    EXPECT_FALSE(log.locationAt(error)); // Not linked until the service reports the diagnostic.

    // The diagnostic after its line links the line...
    EXPECT_EQ(log.addDiagnostic(errorDiagnostic()), std::vector<std::size_t>{error});
    EXPECT_EQ(log.locationAt(error), (SourceLocation{std::string(kMain), 7, 27}));
    // ...and a repeated line (another project including the header) links straight away.
    const std::size_t again = log.append(BuildOutputKind::Stdout, errorLine());
    EXPECT_EQ(log.locationAt(again), (SourceLocation{std::string(kMain), 7, 27}));
    // A diagnostic without a place to jump to links its line but is no location.
    constexpr std::string_view kLinker = "LINK : fatal error LNK1104: cannot open file 'a.lib'";
    const std::size_t linker = log.append(BuildOutputKind::Stdout, kLinker);
    EXPECT_EQ(log.line(linker).style, OutputLineStyle::Error);
    build::BuildDiagnostic link;
    link.diagnostic = build::parseDiagnosticLine(kLinker).value_or(build::Diagnostic{});
    EXPECT_EQ(log.addDiagnostic(link), std::vector<std::size_t>{linker});
    EXPECT_FALSE(log.locationAt(linker));
    EXPECT_FALSE(log.locationAt(99));

    const std::size_t summary = log.appendSummary(outcome(false, std::chrono::milliseconds(2'340)));
    EXPECT_EQ(log.line(summary).text, "Build failed in 2.3 s");
    EXPECT_EQ(log.line(summary).style, OutputLineStyle::Failed);
    log.clear();
    EXPECT_EQ(log.size(), 0U);
    EXPECT_TRUE(log.diagnostics().empty());
}

TEST(BuildOutputLog, SummaryTexts) {
    build::BuildOutcome ok = outcome(true, std::chrono::milliseconds(1'500));
    EXPECT_EQ(buildSummaryText(ok), "Build succeeded in 1.5 s");
    build::BuildOutcome cancelled = outcome(false, std::chrono::milliseconds(800));
    cancelled.target.action = build::BuildAction::Rebuild;
    cancelled.result.cancelled = true;
    EXPECT_EQ(buildSummaryText(cancelled), "Rebuild cancelled after 0.8 s");
    build::BuildOutcome refused;
    refused.target.action = build::BuildAction::Clean;
    refused.error = Error(ErrorCode::NotFound, "MSBuild.exe was not found");
    EXPECT_EQ(buildSummaryText(refused), "Clean not started: MSBuild.exe was not found");
}

TEST(BuildOutputPane, StreamsABuildFromServiceCallbacksAndJumpsToDiagnostics) {
    FakeBuildBackend backend;
    BuildController controller;
    controller.setBackend(&backend);
    BuildOutputPane pane;
    pane.setController(&controller);
    test::SignalLog jumps(&pane, &BuildOutputPane::openLocation);

    // The service calls back on its own thread; the controller marshals to this one.
    const build::BuildServiceCallbacks callbacks = controller.serviceCallbacks();
    std::thread service([&callbacks] {
        callbacks.onStateChanged(build::BuildState::Running);
        callbacks.onOutput(BuildOutputKind::Stdout, "  main.cpp");
        callbacks.onOutput(BuildOutputKind::Stdout, errorLine());
        callbacks.onDiagnostic(errorDiagnostic());
        callbacks.onFinished(outcome(false, std::chrono::milliseconds(4'200)));
        callbacks.onStateChanged(build::BuildState::Idle);
    });
    service.join();
    EXPECT_EQ(pane.log().size(), 0U); // Nothing ran on the service thread.
    runPostedEvents();

    ASSERT_EQ(pane.log().size(), 3U);
    EXPECT_EQ(pane.editor()->document()->blockCount(), 3);
    EXPECT_EQ(pane.editor()->document()->findBlockByNumber(1).text().toStdString(), errorLine());
    EXPECT_EQ(pane.editor()->document()->findBlockByNumber(2).text(), QStringLiteral("Build failed in 4.2 s"));
    EXPECT_FALSE(pane.activateLine(0));
    EXPECT_TRUE(pane.activateLine(1));
    ASSERT_EQ(jumps.count(), 1);
    EXPECT_EQ(jumps.at(0).at(0).toString().toStdString(), std::string(kMain));
    EXPECT_EQ(jumps.at(0).at(1).toInt(), 7);
    EXPECT_EQ(jumps.at(0).at(2).toInt(), 27);

    // A pane created after the build replays it.
    BuildOutputPane late;
    late.setController(&controller);
    EXPECT_EQ(late.log().size(), 3U);
    EXPECT_TRUE(late.log().locationAt(1).has_value());

    // The next build starts with an empty pane.
    controller.serviceCallbacks().onStateChanged(build::BuildState::Starting);
    runPostedEvents();
    EXPECT_EQ(pane.log().size(), 0U);
    EXPECT_TRUE(pane.editor()->toPlainText().isEmpty());
}

TEST(BuildOutputPane, AClickOnADiagnosticLineJumps) {
    BuildOutputPane pane;
    pane.resize(800, 300);
    pane.show(); // Offscreen: lays the text out without a window on screen.
    pane.appendLine(BuildOutputKind::Stdout, QStringLiteral("  main.cpp"));
    pane.appendLine(BuildOutputKind::Stdout, QString::fromStdString(errorLine()));
    pane.addDiagnostic(errorDiagnostic());
    QCoreApplication::processEvents();
    test::SignalLog jumps(&pane, &BuildOutputPane::openLocation);

    QWidget* viewport = pane.editor()->viewport();
    const auto click = [viewport](QPoint position) {
        QMouseEvent press(QEvent::MouseButtonPress, position, viewport->mapToGlobal(position), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, position, viewport->mapToGlobal(position), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &release);
    };
    const QTextBlock plain = pane.editor()->document()->findBlockByNumber(0);
    const QTextBlock error = pane.editor()->document()->findBlockByNumber(1);
    const QPoint onPlain = pane.editor()->cursorRect(QTextCursor(plain)).center() + QPoint(4, 0);
    const QPoint onError = pane.editor()->cursorRect(QTextCursor(error)).center() + QPoint(4, 0);
    EXPECT_EQ(pane.lineAt(onError), 1U);
    click(onPlain);
    EXPECT_EQ(jumps.count(), 0);
    click(onError);
    ASSERT_EQ(jumps.count(), 1);
    EXPECT_EQ(jumps.at(0).at(1).toInt(), 7);
    pane.hide();
}

TEST(BuildOutputPane, FollowsTheTailUnlessScrolledUpAndClears) {
    BuildOutputPane pane;
    pane.resize(600, 200);
    pane.show();
    QScrollBar* bar = pane.editor()->verticalScrollBar();
    for (int i = 0; i < 200; ++i) {
        pane.appendLine(BuildOutputKind::Stdout, QStringLiteral("line %1").arg(i));
    }
    QCoreApplication::processEvents();
    ASSERT_GT(bar->maximum(), 0);
    EXPECT_TRUE(pane.followsTail());
    EXPECT_EQ(bar->value(), bar->maximum());

    bar->setValue(0); // The user scrolls up to read.
    EXPECT_FALSE(pane.followsTail());
    pane.appendLine(BuildOutputKind::Stdout, QStringLiteral("more"));
    EXPECT_EQ(bar->value(), 0);

    bar->setValue(bar->maximum()); // Back at the bottom: follow again.
    EXPECT_TRUE(pane.followsTail());
    pane.appendLine(BuildOutputKind::Stdout, QStringLiteral("and more"));
    EXPECT_EQ(bar->value(), bar->maximum());

    pane.clearButton()->click();
    EXPECT_EQ(pane.log().size(), 0U);
    EXPECT_TRUE(pane.editor()->toPlainText().isEmpty());
    pane.hide();
}

} // namespace
} // namespace frost::ui
