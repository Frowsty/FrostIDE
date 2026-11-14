// SPDX-License-Identifier: Apache-2.0
// Unit tests for console retention, source-link identity, filtering and command callback boundaries.
#include "frost/ui/toolwindows/engine_console.hpp"

#include "toolwindows/toolwindow_test_util.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <gtest/gtest.h>

namespace frost::ui {
namespace {

EngineLogEntry entry(QString message, int line = 47) {
    EngineLogEntry result;
    result.level = EngineLogLevel::Error;
    result.logger = QStringLiteral("Lua");
    result.message = std::move(message);
    result.timestamp = QStringLiteral("23:01:04.125");
#ifdef _WIN32
    result.locations = {{"C:\\project\\Assets\\Player.lua", line, 3}, {"C:\\engine\\Script.cpp", 88, 9}};
#else
    result.locations = {{"/project/Assets/Player.lua", line, 3}, {"/engine/Script.cpp", 88, 9}};
#endif
    return result;
}

TEST(EngineConsole, BoundsEntriesAndPreservesExactSourceFramesThroughEviction) {
    EngineConsole console(nullptr, 3, 4096);
    for (int n = 0; n < 5; ++n)
        EXPECT_TRUE(console.append(entry(QString::number(n), 47 + n)));
    test::runPostedEvents();
    EXPECT_EQ(console.retainedEntries(), 3U);
    EXPECT_EQ(console.droppedEntries(), 2U);
    EXPECT_LE(console.retainedBytes(), 4096U);
    ASSERT_EQ(console.view()->topLevelItemCount(), 3);
    auto* retained = console.view()->topLevelItem(0);
    ASSERT_EQ(retained->childCount(), 2);
    EXPECT_EQ(retained->text(3), QStringLiteral("2"));
    test::SignalLog jumps(&console, &EngineConsole::openLocation);
    EXPECT_TRUE(console.activate(retained));
    ASSERT_EQ(jumps.count(), 1);
    EXPECT_EQ(jumps.at(0).at(0).toString().toStdString(), entry({}).locations[0].path);
    EXPECT_EQ(jumps.at(0).at(1).toInt(), 49);
    EXPECT_EQ(jumps.at(0).at(2).toInt(), 3);
    EXPECT_TRUE(console.activate(retained->child(1)));
    ASSERT_EQ(jumps.count(), 2);
    EXPECT_EQ(jumps.at(1).at(0).toString().toStdString(), entry({}).locations[1].path);
    EXPECT_EQ(jumps.at(1).at(1).toInt(), 88);
    console.clear();
    EXPECT_EQ(console.retainedEntries(), 0U);
    EXPECT_EQ(console.retainedBytes(), 0U);
    EXPECT_EQ(console.droppedEntries(), 0U);
}

TEST(EngineConsole, BoundsBytesAndDropsOversizedEventsWhole) {
    EngineConsole console(nullptr, 5000, 1024);
    for (int n = 0; n < 10; ++n)
        EXPECT_TRUE(console.append(entry(QString(100, QLatin1Char('a')))));
    EXPECT_LE(console.retainedBytes(), 1024U);
    EXPECT_GT(console.droppedEntries(), 0U);
    const auto retained = console.retainedEntries();
    const auto dropped = console.droppedEntries();
    EXPECT_FALSE(console.append(entry(QString(2000, QLatin1Char('b')))));
    EXPECT_EQ(console.retainedEntries(), retained);
    EXPECT_EQ(console.droppedEntries(), dropped + 1);
    EXPECT_LE(console.retainedBytes(), 1024U);
}

TEST(EngineConsole, RestoresIndividualTimestampsAndLocationsAfterUncollapsing) {
    EngineConsole console;
    auto first = entry(QStringLiteral("bad call"));
    auto second = first;
    second.timestamp = QStringLiteral("23:01:05.125");
    EXPECT_TRUE(console.append(std::move(first)));
    EXPECT_TRUE(console.append(std::move(second)));
    test::runPostedEvents();
    console.setCollapseDuplicates(true);
    EXPECT_FALSE(console.view()->topLevelItem(0)->isHidden());
    EXPECT_TRUE(console.view()->topLevelItem(1)->isHidden());
    EXPECT_TRUE(console.view()->topLevelItem(0)->text(3).contains(QStringLiteral("x2")));
    console.setCollapseDuplicates(false);
    EXPECT_FALSE(console.view()->topLevelItem(1)->isHidden());
    EXPECT_EQ(console.view()->topLevelItem(1)->text(0), QStringLiteral("23:01:05.125"));
    EXPECT_EQ(console.view()->topLevelItem(0)->text(3), QStringLiteral("bad call"));
    console.loggerField()->setText(QStringLiteral("shader"));
    EXPECT_TRUE(console.view()->topLevelItem(0)->isHidden());
    console.loggerField()->clear();
    console.searchField()->setText(QStringLiteral("BAD CALL"));
    EXPECT_FALSE(console.view()->topLevelItem(0)->isHidden());
    console.levelFilter()->setCurrentIndex(static_cast<int>(EngineLogLevel::Critical));
    EXPECT_TRUE(console.view()->topLevelItem(0)->isHidden());
}

TEST(EngineConsole, RegexAndDuplicateFiltersPreserveDistinctReportedLocations) {
    EngineConsole console;
    EXPECT_TRUE(console.append(entry(QStringLiteral("failure 21"), 21)));
    EXPECT_TRUE(console.append(entry(QStringLiteral("failure 21"), 22)));
    test::runPostedEvents();
    console.setCollapseDuplicates(true);
    EXPECT_FALSE(console.view()->topLevelItem(1)->isHidden());
    auto* regex = console.findChild<QCheckBox*>(QStringLiteral("engineLogRegex"));
    ASSERT_NE(regex, nullptr);
    regex->setChecked(true);
    console.searchField()->setText(QStringLiteral("failure [0-9]+"));
    EXPECT_FALSE(console.view()->topLevelItem(0)->isHidden());
    console.searchField()->setText(QStringLiteral("["));
    EXPECT_TRUE(console.view()->topLevelItem(0)->isHidden());
    EXPECT_FALSE(console.searchField()->toolTip().isEmpty());
    regex->setChecked(false);
    console.searchField()->clear();
    EXPECT_FALSE(console.view()->topLevelItem(0)->isHidden());
}

TEST(EngineConsole, DelegatesEveryWellFormedCommandToTheCurrentPolicyCallback) {
    EngineConsole console;
    int calls = 0;
    QString method, params;
    console.setCommandHandler([&](const QString& m, const QString& p) {
        ++calls;
        method = m;
        params = p;
    });
    EXPECT_FALSE(console.submitCommand(QStringLiteral("server.info")));
    EXPECT_EQ(calls, 0);
    console.setConnected(true);
    EXPECT_FALSE(console.submitCommand(QStringLiteral("server.info"), QStringLiteral("[]")));
    EXPECT_FALSE(console.submitCommand(QStringLiteral("server.info"), QStringLiteral("{oops")));
    EXPECT_FALSE(console.submitCommand(QString(), QStringLiteral("{}")));
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(console.submitCommand(QStringLiteral(" server.info "), QStringLiteral("{ \"verbose\": true }")));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(method, QStringLiteral("server.info"));
    EXPECT_EQ(params, QStringLiteral("{ \"verbose\": true }"));
    // The UI must not pretend this dispatch returned a result or changed the engine state.
    EXPECT_EQ(console.retainedEntries(), 0U);
    console.setCommandHandler({});
    EXPECT_FALSE(console.submitCommand(QStringLiteral("server.info")));
    console.setCommandHandler([&](const QString&, const QString&) { ++calls; });
    console.setConnected(false);
    EXPECT_FALSE(console.submitCommand(QStringLiteral("server.info")));
    EXPECT_EQ(calls, 1);
}

TEST(EngineConsole, ClearOnPlayUsesOnlyDeliveredRisingEdgesAndAcceptsQueuedDestruction) {
    auto console = std::make_unique<EngineConsole>();
    console->setClearOnPlay(true);
    EXPECT_TRUE(console->append(entry(QStringLiteral("old error"))));
    console->setPlaying(true);
    EXPECT_EQ(console->retainedEntries(), 0U);
    EXPECT_TRUE(console->append(entry(QStringLiteral("during play"))));
    console->setPlaying(true);
    EXPECT_EQ(console->retainedEntries(), 1U);
    console->setPlaying(false);
    console->setPlaying(true);
    EXPECT_EQ(console->retainedEntries(), 0U);
    EXPECT_TRUE(console->append(entry(QStringLiteral("queued log"))));
    console.reset();
    test::runPostedEvents(); // The QObject-scoped queued display refresh cannot touch a dead console.
}

} // namespace
} // namespace frost::ui
