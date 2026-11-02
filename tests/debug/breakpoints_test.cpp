// SPDX-License-Identifier: Apache-2.0
//
// Tests for BreakpointManager and DocumentBreakpointTracker (frost/debug/breakpoints.hpp): toggling,
// path identity, the adapter's verdicts, listeners, the lines a session is configured with,
// breakpoints moving and merging with a document's edits, and the .frost/workspace.json round trip.

#include "frost/core/clock.hpp"
#include "frost/debug/breakpoints.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/text/document.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace frost::debug {
namespace {

using test::workPath;

// The 1-based lines of `path`'s breakpoints.
std::vector<std::int64_t> linesOf(const BreakpointManager& manager, std::string_view path) {
    std::vector<std::int64_t> out;
    for (const LineBreakpoint& breakpoint : manager.inFile(path)) {
        out.push_back(breakpoint.line);
    }
    return out;
}

TEST(BreakpointManager, TogglesAddsAndRemoves) {
    BreakpointManager manager;
    const std::string main = workPath("App/main.cpp");
    auto added = manager.toggle(main, 12);
    ASSERT_TRUE(added.has_value()) << added.error().describe();
    ASSERT_TRUE(added->has_value());
    const BreakpointId first = **added;
    EXPECT_EQ(manager.size(), 1U);
    ASSERT_TRUE(manager.at(main, 12).has_value());
    EXPECT_EQ(manager.at(main, 12)->status, BreakpointStatus::Unknown);
    EXPECT_EQ(manager.add(main, 12).value(), first) << "one breakpoint per line";
    const BreakpointId second = manager.add(main, 3).value();
    EXPECT_NE(second, first);
    EXPECT_EQ(linesOf(manager, main), (std::vector<std::int64_t>{3, 12})) << "by line";

    auto removed = manager.toggle(main, 12);
    ASSERT_TRUE(removed.has_value());
    EXPECT_FALSE(removed->has_value()) << "a second toggle removes it";
    EXPECT_FALSE(manager.find(first).has_value());
    EXPECT_EQ(manager.add(main, 12).value(), second + 1) << "ids are never reused";

    EXPECT_EQ(manager.toggle("main.cpp", 1).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(manager.toggle(main, 0).error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(manager.remove(second));
    EXPECT_FALSE(manager.remove(second));
    manager.clear();
    EXPECT_EQ(manager.size(), 0U);
}

TEST(BreakpointManager, WindowsPathsCompareCaseInsensitively) {
    BreakpointManager windows(io::PathStyle::Windows);
    ASSERT_TRUE(windows.toggle("C:\\Work\\App\\Main.cpp", 4).has_value());
    EXPECT_TRUE(windows.samePath("C:\\Work\\App\\Main.cpp", "c:\\work\\app\\main.cpp"));
    auto again = windows.toggle("c:\\work\\app\\main.cpp", 4);
    ASSERT_TRUE(again.has_value());
    EXPECT_FALSE(again->has_value()) << "the same file in another case";
    BreakpointManager posix(io::PathStyle::Posix);
    EXPECT_FALSE(posix.samePath("/w/Main.cpp", "/w/main.cpp"));
}

TEST(BreakpointManager, StatusesListenersAndSessionLines) {
    BreakpointManager manager;
    const std::string main = workPath("App/main.cpp");
    const std::string util = workPath("App/util.cpp");
    std::vector<std::pair<std::string, BreakpointManager::Change>> changes;
    const auto token = manager.subscribe(
        [&](std::string_view path, BreakpointManager::Change change) { changes.emplace_back(path, change); });
    const BreakpointId a = manager.add(main, 10).value();
    const BreakpointId b = manager.add(main, 11).value();
    const BreakpointId c = manager.add(util, 2).value();
    ASSERT_EQ(changes.size(), 3U);
    EXPECT_EQ(changes[0], (std::pair{main, BreakpointManager::Change::Lines}));

    const auto lines = manager.sessionLines();
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines.at(main), (std::vector<BreakpointLine>{{a, 10}, {b, 11}}));
    EXPECT_EQ(manager.sessionLines(util), (std::vector<BreakpointLine>{{c, 2}}));
    EXPECT_EQ(manager.files(), (std::vector<std::string>{main, util}));

    manager.resetStatus(BreakpointStatus::Pending);
    EXPECT_EQ(manager.find(a)->status, BreakpointStatus::Pending);
    manager.setStatus(a, true, {});
    manager.setStatus(b, false, "No code is generated for this line");
    manager.setStatus(9999, true, {}); // An id that does not exist is ignored.
    EXPECT_EQ(manager.find(a)->status, BreakpointStatus::Verified);
    EXPECT_EQ(manager.find(b)->status, BreakpointStatus::Unverified);
    EXPECT_EQ(manager.find(b)->message, "No code is generated for this line");
    EXPECT_EQ(changes.back(), (std::pair{main, BreakpointManager::Change::Status}));
    manager.resetStatus();
    EXPECT_EQ(manager.find(b)->status, BreakpointStatus::Unknown);
    EXPECT_TRUE(manager.find(b)->message.empty());

    manager.unsubscribe(token);
    const std::size_t seen = changes.size();
    static_cast<void>(manager.add(util, 9));
    EXPECT_EQ(changes.size(), seen);
}

TEST(BreakpointManager, MoveMergesOntoTheLowerId) {
    BreakpointManager manager;
    const std::string main = workPath("App/main.cpp");
    const BreakpointId a = manager.add(main, 5).value();
    const BreakpointId b = manager.add(main, 8).value();
    manager.move(main, {{b, 5}, {a, 5}});
    EXPECT_EQ(manager.size(), 1U);
    EXPECT_TRUE(manager.find(a).has_value());
    EXPECT_FALSE(manager.find(b).has_value());
    manager.move(main, {{a, 20}, {12345, 1}});
    EXPECT_EQ(linesOf(manager, main), (std::vector<std::int64_t>{20}));
}

TEST(DocumentBreakpointTracker, BreakpointsMoveWithEditsAndMerge) {
    BreakpointManager manager;
    const std::string path = workPath("App/main.cpp");
    text::Document document = text::Document::fromUtf8("a\nb\nc\nd\n").value();
    const BreakpointId onB = manager.add(path, 2).value();
    const BreakpointId onD = manager.add(path, 4).value();
    DocumentBreakpointTracker tracker(manager, document, path);

    // Lines inserted above move both down; typing at a line's start keeps it on its line.
    ASSERT_TRUE(document.replace(text::ByteRange{ByteOffset{0}, ByteOffset{0}}, "new\n", text::EditContext{}));
    tracker.sync();
    EXPECT_EQ(linesOf(manager, path), (std::vector<std::int64_t>{3, 5}));
    // "new\na\nb\nc\nd\n": b starts at 6.
    ASSERT_TRUE(document.replace(text::ByteRange{ByteOffset{6}, ByteOffset{6}}, "x", text::EditContext{}));
    tracker.sync();
    EXPECT_EQ(linesOf(manager, path), (std::vector<std::int64_t>{3, 5}));

    // Deleting "xb\nc\n" (bytes 6..11) puts both on the line that takes the place: they merge.
    ASSERT_TRUE(document.replace(text::ByteRange{ByteOffset{6}, ByteOffset{11}}, "", text::EditContext{}));
    tracker.sync();
    EXPECT_EQ(manager.size(), 1U);
    EXPECT_TRUE(manager.find(onB).has_value()) << "the lower id stays";
    EXPECT_FALSE(manager.find(onD).has_value());
    EXPECT_EQ(manager.find(onB)->line, 3);

    // A toggle on the edited document goes through the manager's listener and is tracked too.
    ASSERT_TRUE(manager.toggle(path, 1).has_value());
    ASSERT_TRUE(document.replace(text::ByteRange{ByteOffset{0}, ByteOffset{0}}, "top\n", text::EditContext{}));
    tracker.sync();
    EXPECT_EQ(linesOf(manager, path), (std::vector<std::int64_t>{2, 4}));
    EXPECT_EQ(tracker.path(), path);
}

class BreakpointPersistence : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(fs.createDirectories(root).has_value()); }

    [[nodiscard]] std::string workspaceFile() const {
        return io::joinPath(io::joinPath(root, ".frost").value(), "workspace.json").value();
    }
    [[nodiscard]] std::string read() { return fs.readFile(workspaceFile()).value_or(std::string()); }

    ManualClock clock;
    io::MemFs fs{clock};
    std::string root = workPath("Proj");
};

TEST_F(BreakpointPersistence, SavesRelativePathsAndLoadsThemBack) {
    BreakpointManager manager;
    const std::string main = io::joinPath(root, test::kWindows ? "src\\main.cpp" : "src/main.cpp").value();
    const std::string outside = workPath("Other/lib.cpp");
    static_cast<void>(manager.add(main, 12).value());
    static_cast<void>(manager.add(main, 3).value());
    static_cast<void>(manager.add(outside, 7).value());
    ASSERT_TRUE(manager.save(fs, root).has_value());
    const std::string bytes = read();
    EXPECT_NE(bytes.find("\"schemaVersion\""), std::string::npos) << bytes;
    EXPECT_NE(bytes.find("\"breakpoints\": [\n    {\"file\": "), std::string::npos) << bytes;
    EXPECT_NE(bytes.find("    {\"file\": \"src/main.cpp\", \"line\": 3},\n"
                         "    {\"file\": \"src/main.cpp\", \"line\": 12}"),
              std::string::npos)
        << "one breakpoint per line, by file and line: " << bytes;
    EXPECT_NE(bytes.find("\"line\": 7"), std::string::npos) << "a file outside the root is stored absolute";

    BreakpointManager loaded;
    ASSERT_TRUE(loaded.load(fs, root).has_value());
    EXPECT_EQ(loaded.size(), 3U);
    EXPECT_EQ(linesOf(loaded, main), (std::vector<std::int64_t>{3, 12}));
    EXPECT_EQ(linesOf(loaded, outside), (std::vector<std::int64_t>{7}));
    EXPECT_EQ(loaded.find(loaded.at(main, 3)->id)->status, BreakpointStatus::Unknown);
}

TEST_F(BreakpointPersistence, KeepsOtherMembersSkipsBadEntriesAndRemovesAnEmptyList) {
    ASSERT_TRUE(fs.createDirectories(io::joinPath(root, ".frost").value()).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(workspaceFile(), "{\n  \"schemaVersion\": 1,\n  \"run.startup\": \"App\",\n"
                                                    "  \"breakpoints\": [{\"file\": \"a.cpp\", \"line\": 2}, "
                                                    "{\"file\": \"\", \"line\": 1}, {\"line\": 4}, "
                                                    "{\"file\": \"b.cpp\", \"line\": 0}]\n}\n")
                    .has_value());
    BreakpointManager manager;
    ASSERT_TRUE(manager.load(fs, root).has_value());
    ASSERT_EQ(manager.size(), 1U) << "malformed entries are skipped";
    EXPECT_EQ(linesOf(manager, io::joinPath(root, "a.cpp").value()), (std::vector<std::int64_t>{2}));

    manager.clear();
    ASSERT_TRUE(manager.save(fs, root).has_value());
    const std::string bytes = read();
    EXPECT_EQ(bytes.find("breakpoints"), std::string::npos) << bytes;
    EXPECT_NE(bytes.find("\"run.startup\": \"App\""), std::string::npos) << "other members are kept";

    ASSERT_TRUE(fs.writeFileAtomic(workspaceFile(), "{ not json").has_value());
    EXPECT_FALSE(manager.load(fs, root).has_value());

    // Nothing to save and no file: nothing is written.
    BreakpointManager empty;
    const std::string fresh = workPath("Fresh");
    ASSERT_TRUE(fs.createDirectories(fresh).has_value());
    ASSERT_TRUE(empty.save(fs, fresh).has_value());
    EXPECT_FALSE(fs.exists(io::joinPath(fresh, ".frost").value()).value());
    ASSERT_TRUE(empty.load(fs, fresh).has_value()) << "a missing file is an empty list";
}

} // namespace
} // namespace frost::debug
