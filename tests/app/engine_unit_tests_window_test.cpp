// SPDX-License-Identifier: Apache-2.0
// FER-10 composable provider UI over contract inputs. Real CLI fidelity has its own mandatory corpus.
#include "engine_unit_tests_window.hpp"
#include "support/engine_test_fixture.hpp"
#include <QApplication>
#include <QDeadlineTimer>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace frost::app {
namespace {
bool wait(const std::function<bool()>& done) {
    const QDeadlineTimer deadline(5000);
    while (!done()) {
        if (deadline.hasExpired())
            return false;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}
TEST(EngineUnitTestsProvider, ShowsSceneGranularityActualResultsAndNeverRunsCaseSelection) {
    frost::test::EngineTestFixture f;
    io::ScriptedProcessLauncher launcher;
    f.queue(launcher, "test.list", f.listingJson());
    EngineUnitTestsWindowContext context;
    context.fs = &io::realFileSystem();
    context.launcher = &launcher;
    context.retirements = std::make_shared<EngineTestsRetirementBatch>();
    context.source = [&]() -> Result<EngineUnitTestsSource> {
        return EngineUnitTestsSource{f.options, 1, [] { return true; }};
    };
    int debug = 0, opened = 0, openedLine = 0;
    std::string debugScene, openedPath;
    context.openLocation = [&](const std::string& path, int line) {
        ++opened;
        openedPath = path;
        openedLine = line;
    };
    context.debugScene = [&](const auto&, const auto& scene) {
        ++debug;
        debugScene = scene.scene;
    };
    std::unique_ptr<QWidget> widget(createEngineUnitTestsProviderWidget(nullptr, context));
    auto* tree = widget->findChild<QTreeWidget*>("engineTestsTree");
    ASSERT_NE(tree, nullptr);
    auto* all = widget->findChild<QPushButton*>("engineTestsRunAll");
    auto* selected = widget->findChild<QPushButton*>("engineTestsRunSelected");
    auto* failed = widget->findChild<QPushButton*>("engineTestsRunFailed");
    auto* debugging = widget->findChild<QPushButton*>("engineTestsDebug");
    ASSERT_TRUE(wait([&] { return all->isEnabled(); }));
    ASSERT_EQ(tree->topLevelItemCount(), 2);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), "Assets/Tests/A.fscene");
    EXPECT_EQ(tree->topLevelItem(0)->childCount(), 0); // Listing has no guessed Test.Case declarations.
    tree->setCurrentItem(tree->topLevelItem(1));
    ASSERT_TRUE(selected->isEnabled());
    f.queue(launcher, "test.run", frost::test::engineSceneReport("Assets/Tests/B.fscene", false), 3);
    selected->click();
    ASSERT_TRUE(wait([&] { return failed->isEnabled(); }));
    auto* scene = tree->topLevelItem(1);
    ASSERT_EQ(scene->childCount(), 2);
    EXPECT_EQ(scene->text(1), "Failed");
    EXPECT_EQ(scene->text(2), "12.345 ms");
    EXPECT_EQ(scene->child(0)->text(0), "actual case");
    EXPECT_EQ(scene->child(0)->text(2), QString::fromUtf8("—"));
    tree->setCurrentItem(scene->child(0));
    EXPECT_FALSE(selected->isEnabled());
    EXPECT_FALSE(debugging->isEnabled());
    EXPECT_NE(selected->toolTip().indexOf("not individual"), -1);
    selected->click();
    debugging->click();
    EXPECT_EQ(launcher.launches().size(), 2u);
    EXPECT_EQ(debug, 0);
    tree->itemDoubleClicked(scene->child(0), 0);
    ASSERT_TRUE(wait([&] { return opened == 1; }));
    EXPECT_EQ(openedLine, 4);
    EXPECT_EQ(openedPath, io::realFileSystem().canonicalPath(f.dir.join("Assets/Scripts/Test.lua")).value_or(""));
    tree->setCurrentItem(scene);
    ASSERT_TRUE(debugging->isEnabled());
    debugging->click();
    EXPECT_EQ(debug, 1);
    EXPECT_EQ(debugScene, "Assets/Tests/B.fscene");
    EXPECT_EQ(launcher.launches().size(), 2u);
    f.queue(launcher, "test.run", frost::test::engineSceneReport("Assets/Tests/B.fscene", true));
    failed->click();
    ASSERT_TRUE(wait([&] { return all->isEnabled(); }));
    EXPECT_FALSE(failed->isEnabled());
    EXPECT_EQ(tree->topLevelItem(1)->text(1), "Passed");
}
TEST(EngineUnitTestsProvider, TrustRevocationCancelsOwnedRunAndClearsStaleResults) {
    frost::test::EngineTestFixture f;
    io::ScriptedProcessLauncher launcher;
    std::atomic<bool> trusted = true;
    f.queue(launcher, "test.list", f.listingJson());
    EngineUnitTestsWindowContext context;
    context.fs = &io::realFileSystem();
    context.launcher = &launcher;
    context.retirements = std::make_shared<EngineTestsRetirementBatch>();
    context.source = [&]() -> Result<EngineUnitTestsSource> {
        return EngineUnitTestsSource{f.options, 1, [&] { return trusted.load(); }};
    };
    std::unique_ptr<QWidget> widget(createEngineUnitTestsProviderWidget(nullptr, context));
    auto* all = widget->findChild<QPushButton*>("engineTestsRunAll");
    auto* stop = widget->findChild<QPushButton*>("engineTestsStop");
    auto* tree = widget->findChild<QTreeWidget*>("engineTestsTree");
    ASSERT_TRUE(wait([&] { return all->isEnabled(); }));
    io::ScriptedProcess hung;
    hung.neverExits = true;
    launcher.expect(hung);
    all->click();
    ASSERT_TRUE(wait([&] { return launcher.launches().size() == 2; }));
    trusted = false;
    ASSERT_TRUE(wait([&] { return !stop->isEnabled() && tree->topLevelItemCount() == 0; }));
    EXPECT_FALSE(all->isEnabled());
    ASSERT_EQ(launcher.launches().size(), 2u);
    EXPECT_TRUE(launcher.launches().back().terminated);
}
TEST(EngineUnitTestsProvider, BlockedPhysicalValidationDoesNotBlockGuiDestructionAndScopedDrainOwnsWorker) {
    frost::test::EngineTestFixture f;
    io::ScriptedProcessLauncher launcher;
    auto batch = std::make_shared<EngineTestsRetirementBatch>();
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool released = false;
        std::atomic<bool> entered = false, returned = false;
    };
    auto gate = std::make_shared<Gate>();
    EngineUnitTestsWindowContext context;
    context.fs = &io::realFileSystem();
    context.launcher = &launcher;
    context.retirements = batch;
    context.source = [&]() -> Result<EngineUnitTestsSource> {
        EngineUnitTestsSource source{f.options, 1, [] { return true; }};
        source.validatePhysical = [gate]() -> Result<void> {
            gate->entered = true;
            std::unique_lock lock(gate->mutex);
            gate->changed.wait_for(lock, std::chrono::seconds(2), [&] { return gate->released; });
            gate->returned = true;
            return {};
        };
        return source;
    };
    std::unique_ptr<QWidget> widget(createEngineUnitTestsProviderWidget(nullptr, context));
    ASSERT_TRUE(wait([&] { return gate->entered.load(); }));
    widget.reset();
    EXPECT_FALSE(gate->returned.load()) << "GUI destruction joined blocked physical work";
    EXPECT_GT(batch->pending(), 0u);
    bool guiTick = false;
    QTimer::singleShot(0, [&] { guiTick = true; });
    ASSERT_TRUE(wait([&] { return guiTick; }));
    EXPECT_TRUE(launcher.launches().empty());
    {
        const std::lock_guard lock(gate->mutex);
        gate->released = true;
    }
    gate->changed.notify_all();
    batch->drain();
    EXPECT_EQ(batch->pending(), 0u);
    EXPECT_TRUE(gate->returned.load());
    QApplication::processEvents();
    EXPECT_TRUE(launcher.launches().empty()) << "revoked delivery/work cannot launch after widget destruction";
}
TEST(EngineUnitTestsProvider, PollablePhysicalValidationCancellationDiscardsLateSuccessAndDrains) {
    auto batch = std::make_shared<EngineTestsRetirementBatch>();
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<bool> entered = false;
        bool release = false;
    };
    auto gate = std::make_shared<Gate>();
    EngineUnitTestsSource source;
    source.admitted = [] { return true; };
    source.validatePhysical = [gate]() -> Result<void> {
        gate->entered = true;
        std::unique_lock lock(gate->mutex);
        gate->changed.wait_for(lock, std::chrono::seconds(2), [&] { return gate->release; });
        return {};
    };
    auto handle = batch->validate(source);
    ASSERT_TRUE(wait([&] { return gate->entered.load(); }));
    EXPECT_FALSE(handle->ready());
    handle->cancel();
    {
        const std::lock_guard lock(gate->mutex);
        gate->release = true;
    }
    gate->changed.notify_all();
    ASSERT_TRUE(wait([&] { return handle->ready(); }));
    auto result = handle->takeResult();
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().code(), ErrorCode::Cancelled);
    EXPECT_FALSE(handle->takeResult());
    batch->drain();
    EXPECT_EQ(batch->pending(), 0u);
}
TEST(EngineUnitTestsProvider, CancellingReadyPhysicalValidationCannotPublishItsEarlierSuccess) {
    EngineTestsRetirementBatch batch;
    EngineUnitTestsSource source;
    source.admitted = [] { return true; };
    source.validatePhysical = []() -> Result<void> { return {}; };
    auto validation = batch.validate(source);
    ASSERT_TRUE(wait([&] { return validation->ready(); }));
    validation->cancel();
    auto result = validation->takeResult();
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().code(), ErrorCode::Cancelled);
    batch.drain();
}
TEST(EngineUnitTestsProvider, RefusesUntrustedSourcesAndDoesNotExposeInventedListing) {
    frost::test::EngineTestFixture f;
    io::ScriptedProcessLauncher launcher;
    EngineUnitTestsWindowContext context;
    context.fs = &io::realFileSystem();
    context.launcher = &launcher;
    context.retirements = std::make_shared<EngineTestsRetirementBatch>();
    context.source = [&]() -> Result<EngineUnitTestsSource> {
        return EngineUnitTestsSource{f.options, 1, [] { return false; }};
    };
    std::unique_ptr<QWidget> widget(createEngineUnitTestsProviderWidget(nullptr, context));
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_EQ(widget->findChild<QTreeWidget*>("engineTestsTree")->topLevelItemCount(), 0);
    EXPECT_FALSE(widget->findChild<QPushButton*>("engineTestsRunAll")->isEnabled());
    EXPECT_TRUE(widget->findChild<QLabel*>("engineTestsSummary")->text().contains("Trust"));
}
} // namespace
} // namespace frost::app
