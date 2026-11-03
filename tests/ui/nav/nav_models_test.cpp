// SPDX-License-Identifier: Apache-2.0
//
// Functional tests of frost::ui_nav (ADR 0042), a non-Qt executable: navigation history (back and
// forward over recorded places), recent files and locations, the session JSON round trip and its
// store, the document registry over a MemFs SaveManager, the project tree and Go to File candidates,
// and the aligned merge panes.

#include "frost/core/clock.hpp"
#include "frost/diff/line_sequence.hpp"
#include "frost/diff/merge.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/text/text_shaper.hpp"
#include "frost/ui/nav/document_registry.hpp"
#include "frost/ui/nav/merge_layout.hpp"
#include "frost/ui/nav/navigation_history.hpp"
#include "frost/ui/nav/project_files.hpp"
#include "frost/ui/nav/session.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::ui::nav::DocumentRegistry;
using frost::ui::nav::MergePane;
using frost::ui::nav::MergePanes;
using frost::ui::nav::NavigationHistory;
using frost::ui::nav::Place;
using frost::ui::nav::ProjectFiles;
using frost::ui::nav::RecentFiles;
using frost::ui::nav::RecentLocations;
using frost::ui::nav::SessionState;
using frost::ui::nav::SessionStore;

TEST(NavigationHistory, BackAndForwardRestorePlaces) {
    NavigationHistory history;
    history.recordJump(Place{"/p/a.cpp", 10, 1});
    history.recordJump(Place{"/p/b.cpp", 40, 5});
    const Place current{"/p/c.cpp", 3, 1};
    const auto back = history.back(current);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->path, "/p/b.cpp");
    EXPECT_EQ(back->line, 40);
    const auto again = history.back(*back);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->path, "/p/a.cpp");
    EXPECT_FALSE(history.canGoBack());
    const auto forward = history.forward(*again);
    ASSERT_TRUE(forward.has_value());
    EXPECT_EQ(forward->path, "/p/b.cpp");
    const auto last = history.forward(*forward);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->path, "/p/c.cpp");
    EXPECT_FALSE(history.canGoForward());
    // A new jump drops the forward stack.
    static_cast<void>(history.back(*last));
    history.recordJump(Place{"/p/b.cpp", 40, 5});
    EXPECT_FALSE(history.canGoForward());
}

TEST(NavigationHistory, OnlyBigCaretMovesAreRecorded) {
    NavigationHistory history;
    history.noteCaretMove(Place{"/p/a.cpp", 1, 1}, Place{"/p/a.cpp", 4, 1});
    EXPECT_FALSE(history.canGoBack());
    history.noteCaretMove(Place{"/p/a.cpp", 4, 1}, Place{"/p/a.cpp", 200, 1});
    ASSERT_TRUE(history.canGoBack());
    EXPECT_EQ(history.backStack().back().line, 4);
}

TEST(RecentFiles, MostRecentFirstWithoutDuplicates) {
    RecentFiles recent;
    recent.touch("/p/a.cpp");
    recent.touch("/p/b.cpp");
    recent.touch("/p/c.cpp");
    recent.touch("/p/a.cpp");
    EXPECT_EQ(std::vector<std::string>(recent.paths().begin(), recent.paths().end()),
              (std::vector<std::string>{"/p/a.cpp", "/p/c.cpp", "/p/b.cpp"}));
    recent.remove("/p/c.cpp");
    EXPECT_EQ(recent.paths().size(), 2U);
}

TEST(RecentLocations, NearPlacesReplaceEachOther) {
    RecentLocations recent;
    recent.touch(Place{"/p/a.cpp", 10, 1});
    recent.touch(Place{"/p/a.cpp", 11, 4});
    recent.touch(Place{"/p/b.cpp", 1, 1});
    ASSERT_EQ(recent.places().size(), 2U);
    EXPECT_EQ(recent.places()[0].path, "/p/b.cpp");
    EXPECT_EQ(recent.places()[1].line, 11);
}

class NavFsTest : public ::testing::Test {
protected:
    NavFsTest() : fs(clock, frost::io::PathStyle::Posix) {}

    void file(const std::string& path, std::string_view content = "x\n") {
        ASSERT_TRUE(fs.createDirectories(path.substr(0, path.rfind('/'))).has_value()) << path;
        ASSERT_TRUE(fs.writeFileAtomic(path, content).has_value()) << path;
    }

    frost::ManualClock clock{frost::IClock::SteadyTime{std::chrono::hours(1)},
                             frost::IClock::SystemTime{std::chrono::hours(24 * 365 * 50)}};
    frost::io::MemFs fs;
};

TEST(Session, SplitMetadataRoundTripsAndLegacyDefaultsToOneGroup) {
    frost::ui::nav::SessionState state;
    state.root = "/project";
    state.editorLayout = R"({"children":[{"group":0},{"group":1}]})";
    state.activeGroup = 1;
    frost::ui::nav::SessionTab tab;
    tab.path = "/project/a.cpp";
    tab.group = 1;
    tab.groupActive = true;
    state.tabs.push_back(tab);
    const auto decoded = frost::ui::nav::decodeSession(frost::ui::nav::encodeSession(state));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, state);
    const auto legacy = frost::ui::nav::decodeSession(R"({"version":1,"tabs":[{"path":"/a"}]})");
    ASSERT_TRUE(legacy);
    EXPECT_TRUE(legacy->editorLayout.empty());
    ASSERT_EQ(legacy->tabs.size(), 1U);
    EXPECT_EQ(legacy->tabs.front().group, 0);
}

TEST(Session, MalformedFoldEntryDiscardsTheEntireFoldState) {
    const auto loaded = frost::ui::nav::decodeSession(
        R"({"version":1,"root":"/proj","tabs":[{"path":"/proj/a.cpp","foldTextHash":42,"folds":[[1,4],[9,2],[12,15]]}]})");
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->tabs.size(), 1U);
    EXPECT_TRUE(loaded->tabs[0].folds.empty());
    EXPECT_EQ(loaded->tabs[0].foldTextHash, 0U);
}

TEST_F(NavFsTest, SessionRoundTripsThroughTheStore) {
    SessionStore store(fs, "/data/sessions");
    EXPECT_EQ(store.load("/proj").error().code(), ErrorCode::NotFound);
    SessionState state;
    state.root = "/proj";
    state.tabs = {{"/proj/a.cpp", false, 12, 3, 5}, {"/proj/b.cpp", true, 1, 1, 0}};
    state.tabs[0].foldTextHash = 0xfedcba9876543210ULL;
    state.tabs[0].folds = {{12, 80}, {24, 50}};
    state.active = 1;
    state.toolWindows = {{"fileSystem", true, 280}, {"problems", false, 200}};
    state.recentFiles = {"/proj/b.cpp", "/proj/a.cpp"};
    state.recentLocations = {Place{"/proj/a.cpp", 12, 3}};
    ASSERT_TRUE(store.save(state).has_value());
    const auto loaded = store.load("/proj");
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    EXPECT_EQ(loaded->root, "/proj");
    ASSERT_EQ(loaded->tabs.size(), 2U);
    EXPECT_EQ(loaded->tabs[0].path, "/proj/a.cpp");
    EXPECT_EQ(loaded->tabs[0].line, 12);
    EXPECT_EQ(loaded->tabs[0].column, 3);
    EXPECT_EQ(loaded->tabs[0].top, 5);
    EXPECT_EQ(loaded->tabs[0].foldTextHash, state.tabs[0].foldTextHash);
    EXPECT_EQ(loaded->tabs[0].folds, state.tabs[0].folds);
    EXPECT_TRUE(loaded->tabs[1].pinned);
    EXPECT_EQ(loaded->active, 1);
    ASSERT_EQ(loaded->toolWindows.size(), 2U);
    EXPECT_EQ(loaded->toolWindows[0].id, "fileSystem");
    EXPECT_TRUE(loaded->toolWindows[0].visible);
    EXPECT_EQ(loaded->toolWindows[0].size, 280);
    EXPECT_EQ(loaded->recentFiles, state.recentFiles);
    ASSERT_EQ(loaded->recentLocations.size(), 1U);
    EXPECT_EQ(loaded->recentLocations[0].line, 12);
    EXPECT_EQ(store.lastProject().value_or(""), "/proj");
    EXPECT_EQ(frost::ui::nav::decodeSession("{not json").error().code(), ErrorCode::Corrupt);

    // Closing the project forgets it as the last one (the next start shows the Welcome screen), but
    // it stays a recent project and keeps its session.
    ASSERT_TRUE(store.clearLastProject().has_value());
    EXPECT_FALSE(store.lastProject().has_value());
    EXPECT_EQ(store.recentProjects(), std::vector<std::string>{"/proj"});
    EXPECT_TRUE(store.load("/proj").has_value());
    ASSERT_TRUE(store.setLastProject("/proj").has_value());
    EXPECT_EQ(store.lastProject().value_or(""), "/proj");
}

TEST_F(NavFsTest, RegistryOpensOneDocumentPerPathAndClosesCleanOnes) {
    file("/proj/src/main.cpp", "int main() {}\n");
    frost::document::SaveManagerServices services;
    services.fs = &fs;
    services.clock = &clock;
    frost::document::SaveManagerOptions options;
    options.style = frost::io::PathStyle::Posix;
    frost::document::SaveManager manager(services, options);
    DocumentRegistry registry(manager, fs);
    const auto first = registry.open("/proj/src/./main.cpp");
    ASSERT_TRUE(first.has_value()) << first.error().describe();
    EXPECT_EQ(first->path, "/proj/src/main.cpp");
    const auto second = registry.open("/proj//src/main.cpp");
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->document, second->document);
    EXPECT_EQ(registry.find("/proj/src/main.cpp"), first->document);
    int changes = 0;
    const auto id = registry.subscribe([&](const std::string&) { ++changes; });
    registry.notifyChanged(first->path);
    registry.unsubscribe(id);
    registry.notifyChanged(first->path);
    EXPECT_EQ(changes, 1);
    registry.acquire(first->path);
    registry.acquire(first->path);
    registry.release(first->path);
    EXPECT_NE(registry.find(first->path), nullptr);
    registry.release(first->path);
    EXPECT_EQ(registry.find(first->path), nullptr); // The last view of a clean document closes it.
    EXPECT_EQ(registry.open("/proj/missing.cpp").error().code(), ErrorCode::NotFound);
}

TEST_F(NavFsTest, ProjectTreeHonoursIgnoresAndFeedsGoToFile) {
    file("/proj/.gitignore", "*.tmp\n");
    file("/proj/src/main.cpp");
    file("/proj/src/util.cpp");
    file("/proj/README.md");
    file("/proj/scratch.tmp");
    file("/proj/build/out.o");
    file("/proj/.git/HEAD");
    frost::sched::SimExecutor sim;
    auto scan = sim.spawn(frost::sched::Lane::Background, frost::ui::nav::scanProject(fs, sim, "/proj")).join();
    ASSERT_TRUE(scan.has_value()) << scan.error().describe();
    ProjectFiles files("/proj");
    files.setScan(*scan);
    files.setSolutionFiles({"/elsewhere/shared.h", "/proj/src/main.cpp"});
    const ProjectFiles::Node& root = files.node(0);
    ASSERT_EQ(root.children.size(), 3U); // src/, .gitignore, README.md (directories first).
    EXPECT_EQ(files.node(root.children[0]).name, "src");
    EXPECT_EQ(files.node(root.children[1]).name, ".gitignore");
    EXPECT_EQ(files.node(root.children[2]).name, "README.md");
    for (std::size_t row = 0; row < root.children.size(); ++row) {
        EXPECT_EQ(files.node(root.children[row]).row, row);
    }
    std::stop_source stopped;
    stopped.request_stop();
    ProjectFiles cancelled("/proj");
    EXPECT_FALSE(cancelled.setScanCancellable(*scan, stopped.get_token()));
    EXPECT_EQ(files.fileCount(), 4U);
    EXPECT_EQ(files.candidates(), (std::vector<std::string>{"/proj/.gitignore", "/proj/README.md", "/proj/src/main.cpp",
                                                            "/proj/src/util.cpp", "/elsewhere/shared.h"}));
    EXPECT_EQ(files.displayCandidates()[2], "src/main.cpp");
    EXPECT_TRUE(files.find("/proj/src/util.cpp").has_value());
    EXPECT_FALSE(files.find("/proj/scratch.tmp").has_value());
}

TEST(MergePanes, ChunksLineUpAcrossThePanes) {
    using frost::diff::LineSequence;
    // Yours inserted two lines after "a"; the disk changed "c" to "C".
    auto base = LineSequence::fromBytes("a\nb\nc\n");
    auto ours = LineSequence::fromBytes("a\nx\ny\nb\nc\n");
    auto theirs = LineSequence::fromBytes("a\nb\nC\n");
    ASSERT_TRUE(base && ours && theirs);
    auto model = frost::diff::MergeModel::create(std::move(*base), std::move(*ours), std::move(*theirs));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    const frost::text::MonospaceTextShaper shaper;
    const auto panes = MergePanes::create(*model, shaper);
    ASSERT_TRUE(panes.has_value()) << panes.error().describe();
    // Every pane shows max(lines) per chunk: yours has 5 lines, so every pane has 5 rows plus the
    // trailing empty line.
    EXPECT_EQ(panes->map(MergePane::Yours).rowCount(), 6U);
    EXPECT_EQ(panes->map(MergePane::Base).rowCount(), 6U);
    EXPECT_EQ(panes->map(MergePane::Disk).rowCount(), 6U);
    EXPECT_EQ(panes->fillerRows(MergePane::Yours), 0U);
    EXPECT_EQ(panes->fillerRows(MergePane::Base), 2U);
    EXPECT_EQ(panes->fillerRows(MergePane::Disk), 2U);
    for (std::size_t c = 0; c < panes->chunkCount(); ++c) {
        const auto row = panes->chunkRow(MergePane::Yours, c);
        EXPECT_EQ(panes->chunkRow(MergePane::Base, c), row) << c;
        EXPECT_EQ(panes->chunkRow(MergePane::Disk, c), row) << c;
        EXPECT_EQ(panes->chunkAt(MergePane::Base, row), c);
    }
}

} // namespace
