// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the porcelain v2 parser (git_status.hpp), the per-file status map and its path
// helpers (vcs_status.hpp), the checkout-refusal parser and the failure messages
// (git_repository.hpp, git_runner.hpp), on hand-written git output.

#include "frost/io/path.hpp"
#include "frost/vcs/git_repository.hpp"
#include "frost/vcs/git_runner.hpp"
#include "frost/vcs/git_status.hpp"
#include "frost/vcs/vcs_status.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::ErrorCode;
using namespace frost::vcs;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;
const std::string& workPath() {
    static const std::string value(kWindows ? "C:\\repo" : "/repo");
    return value;
}

std::string nul(std::initializer_list<std::string_view> records) {
    std::string out;
    for (const std::string_view record : records) {
        out += record;
        out += '\0';
    }
    return out;
}

const std::string& testOid() {
    static const std::string value("1111111111111111111111111111111111111111");
    return value;
}

std::string ordinary(std::string_view xy, std::string_view path) {
    return std::format("1 {} N... 100644 100644 100644 {} {} {}", xy, testOid(), testOid(), path);
}

TEST(GitStatusParse, EveryStateAndTheBranchHeaders) {
    const std::string output = nul({
        "# branch.oid 2222222222222222222222222222222222222222",
        "# branch.head main",
        "# branch.upstream origin/main",
        "# branch.ab +2 -1",
        "# stash 3",
        ordinary(".M", "src/modified file.cpp"),
        ordinary("M.", "staged.cpp"),
        ordinary("A.", "added.cpp"),
        ordinary(".D", "gone.cpp"),
        ordinary("D.", "removed.cpp"),
        ordinary("AD", "added-then-deleted.cpp"),
        std::format("2 R. N... 100644 100644 100644 {} {} R100 new name.cpp", testOid(), testOid()),
        "old name.cpp",
        std::format("2 C. N... 100644 100644 100644 {} {} C75 copy.cpp", testOid(), testOid()),
        "source.cpp",
        std::format("u UU N... 100644 100644 100644 100644 {} {} {} both.cpp", testOid(), testOid(), testOid()),
        std::format("1 .M S.M. 160000 160000 160000 {} {} third_party/sub", testOid(), testOid()),
        "? untracked dir/new.txt",
        "! build/out.o",
    });
    auto parsed = parsePorcelainV2(output);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    const GitStatus& status = *parsed;
    EXPECT_EQ(status.branch.oid, "2222222222222222222222222222222222222222");
    EXPECT_EQ(status.branch.head, "main");
    EXPECT_EQ(status.branch.upstream, std::optional<std::string>("origin/main"));
    EXPECT_EQ(status.branch.ahead, 2);
    EXPECT_EQ(status.branch.behind, 1);
    EXPECT_FALSE(status.branch.detached());
    EXPECT_FALSE(status.branch.unborn());
    ASSERT_EQ(status.entries.size(), 12U);

    const auto expect = [&](std::string_view path, VcsFileStatus file, bool staged, bool unstaged) {
        const GitStatusEntry* entry = status.find(path);
        ASSERT_NE(entry, nullptr) << path;
        EXPECT_EQ(entry->fileStatus(), file) << path << ": " << toString(entry->fileStatus());
        EXPECT_EQ(entry->staged(), staged) << path;
        EXPECT_EQ(entry->unstaged(), unstaged) << path;
    };
    expect("src/modified file.cpp", VcsFileStatus::Modified, false, true);
    expect("staged.cpp", VcsFileStatus::Modified, true, false);
    expect("added.cpp", VcsFileStatus::Added, true, false);
    expect("gone.cpp", VcsFileStatus::Deleted, false, true);
    expect("removed.cpp", VcsFileStatus::Deleted, true, false);
    expect("added-then-deleted.cpp", VcsFileStatus::Deleted, true, true);
    expect("new name.cpp", VcsFileStatus::Renamed, true, false);
    expect("copy.cpp", VcsFileStatus::Added, true, false);
    expect("both.cpp", VcsFileStatus::Conflicted, true, true);
    expect("untracked dir/new.txt", VcsFileStatus::Untracked, false, true);
    expect("build/out.o", VcsFileStatus::Ignored, false, false);

    const GitStatusEntry* rename = status.find("old name.cpp");
    ASSERT_NE(rename, nullptr); // find() also answers for a rename's source.
    EXPECT_EQ(rename->kind, GitEntryKind::RenamedOrCopied);
    EXPECT_EQ(rename->path, "new name.cpp");
    EXPECT_EQ(rename->originalPath, "old name.cpp");
    const GitStatusEntry* submodule = status.find("third_party/sub");
    ASSERT_NE(submodule, nullptr);
    EXPECT_TRUE(submodule->submodule);
    EXPECT_EQ(status.find("missing.cpp"), nullptr);
}

TEST(GitStatusParse, UnbornAndDetachedHeads) {
    auto unborn = parsePorcelainV2(nul({"# branch.oid (initial)", "# branch.head main", "? a.txt"}));
    ASSERT_TRUE(unborn.has_value()) << unborn.error().describe();
    EXPECT_TRUE(unborn->branch.unborn());
    EXPECT_EQ(unborn->branch.head, "main");
    EXPECT_FALSE(unborn->branch.upstream.has_value());

    auto detached = parsePorcelainV2(nul({"# branch.oid " + testOid(), "# branch.head (detached)"}));
    ASSERT_TRUE(detached.has_value()) << detached.error().describe();
    EXPECT_TRUE(detached->branch.detached());
    EXPECT_TRUE(detached->entries.empty());

    auto empty = parsePorcelainV2("");
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->entries.empty());
}

TEST(GitStatusParse, RejectsBrokenRecords) {
    EXPECT_EQ(parsePorcelainV2(nul({"1 .M N... 100644"})).error().code(), ErrorCode::Corrupt);
    EXPECT_EQ(parsePorcelainV2(nul({"x what"})).error().code(), ErrorCode::Corrupt);
    // A rename whose source record is missing.
    const std::string rename = std::format("2 R. N... 100644 100644 100644 {} {} R100 b.cpp", testOid(), testOid());
    EXPECT_EQ(parsePorcelainV2(rename + '\0').error().code(), ErrorCode::Corrupt);
}

TEST(GitStatusParse, StatusArgumentsAskForPorcelainV2) {
    const std::vector<std::string> arguments = statusArguments();
    ASSERT_GE(arguments.size(), 4U);
    EXPECT_EQ(arguments[0], "status");
    EXPECT_NE(std::ranges::find(arguments, "--porcelain=v2"), arguments.end());
    EXPECT_NE(std::ranges::find(arguments, "-z"), arguments.end());
    EXPECT_NE(std::ranges::find(arguments, "--branch"), arguments.end());
    EXPECT_NE(std::ranges::find(arguments, "--ignored=no"), arguments.end());
}

TEST(VcsStatusMap, AnswersInsideTheWorkTreeOnly) {
    auto parsed = parsePorcelainV2(
        nul({ordinary(".M", "src/a.cpp"), "? notes.txt",
             std::format("2 R. N... 100644 100644 100644 {} {} R100 b2.cpp", testOid(), testOid()), "b.cpp"}));
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    const VcsStatusMap map(workPath(), *parsed, 7);
    EXPECT_EQ(map.generation(), 7U);
    EXPECT_EQ(map.size(), 3U);
    EXPECT_EQ(map.statusOf(absoluteGitPath(workPath(), "src/a.cpp")), VcsFileStatus::Modified);
    EXPECT_EQ(map.statusOf(absoluteGitPath(workPath(), "notes.txt")), VcsFileStatus::Untracked);
    EXPECT_EQ(map.statusOf(absoluteGitPath(workPath(), "b2.cpp")), VcsFileStatus::Renamed);
    EXPECT_EQ(map.statusOf(absoluteGitPath(workPath(), "src/clean.cpp")), VcsFileStatus::Unmodified);
    EXPECT_EQ(map.statusOf(kWindows ? "C:\\other\\a.cpp" : "/other/a.cpp"), std::nullopt);
    EXPECT_EQ(map.statusOf(kWindows ? "C:\\repository\\a.cpp" : "/repository/a.cpp"), std::nullopt);
    EXPECT_EQ(map.statusOf("relative.cpp"), std::nullopt);
    if constexpr (kWindows) {
        EXPECT_EQ(map.statusOf("c:/REPO/Src/A.cpp"), VcsFileStatus::Modified); // NTFS ignores case.
    }
    const auto entries = map.entries();
    ASSERT_EQ(entries.size(), 3U);
    EXPECT_EQ(entries.front().first, absoluteGitPath(workPath(), "b2.cpp"));
    EXPECT_EQ(VcsStatusMap().statusOf(absoluteGitPath(workPath(), "src/a.cpp")), std::nullopt);
}

TEST(VcsStatusMap, PathHelpersConvertBetweenGitAndNativePaths) {
    EXPECT_EQ(absoluteGitPath(workPath(), "src/a.cpp"), kWindows ? "C:\\repo\\src\\a.cpp" : "/repo/src/a.cpp");
    EXPECT_EQ(relativeGitPath(workPath(), absoluteGitPath(workPath(), "src/a.cpp")),
              std::optional<std::string>("src/a.cpp"));
    EXPECT_EQ(relativeGitPath(workPath(), workPath()), std::nullopt);
    EXPECT_EQ(relativeGitPath(workPath(), kWindows ? "C:\\repo2\\a.cpp" : "/repo2/a.cpp"), std::nullopt);
}

TEST(GitMessages, CheckoutBlockersNameTheFiles) {
    const std::string stderrText =
        "error: Your local changes to the following files would be overwritten by checkout:\n"
        "\tsrc/a.cpp\n"
        "\tREADME.md\n"
        "Please commit your changes or stash them before you switch branches.\n"
        "Aborting\n";
    EXPECT_EQ(checkoutBlockers(stderrText), (std::vector<std::string>{"src/a.cpp", "README.md"}));
    EXPECT_TRUE(checkoutBlockers("fatal: invalid reference: nope\n").empty());
}

TEST(GitMessages, FailuresExplainWhatHappened) {
    GitOutput output;
    output.exitCode = 128;
    output.stderrBytes = "fatal: could not read Username for 'https://example.invalid': terminal prompts disabled\n";
    frost::Error error = gitFailure("Push", output);
    EXPECT_EQ(error.code(), ErrorCode::IoError);
    EXPECT_NE(error.message().find("credential"), std::string::npos) << error.message();
    EXPECT_NE(error.message().find("terminal prompts disabled"), std::string::npos) << error.message();

    output.exitCode = 1;
    output.stderrBytes = " ! [rejected]        main -> main (fetch first)\nerror: failed to push some refs\n";
    error = gitFailure("Push", output);
    EXPECT_NE(error.message().find("pull"), std::string::npos) << error.message();

    output.stderrBytes = "fatal: Unable to create '/r/.git/index.lock': File exists.\n";
    EXPECT_EQ(gitFailure("Commit", output).code(), ErrorCode::Busy);
    EXPECT_EQ(trimGitText("  a b \r\n"), "a b");
}

} // namespace
