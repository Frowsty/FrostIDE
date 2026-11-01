// SPDX-License-Identifier: Apache-2.0
//
// Tests for TrustStore (frost/project/trust_store.hpp): the round trip through a real file,
// parent/child coverage, untrust, refused paths and files, and the build gate.
#include "frost/io/file_system.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/project/trust_store.hpp"

#include "project_test_util.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

namespace frost::project {
namespace {

using test::MemTree;

constexpr bool kWindows = io::kNativePathStyle == io::PathStyle::Windows;

TEST(TrustStore, RoundTripsThroughTheFile) {
    frost::test::ScratchDir scratch("trust");
    // A directory that does not exist yet: the first trust() creates it.
    const std::string storePath = scratch.join(test::native("data/FrostIDE/trusted-roots.txt"));
    const std::string work = scratch.join("work");
    const std::string other = scratch.join("other");
    {
        auto store = TrustStore::open(io::realFileSystem(), storePath);
        ASSERT_TRUE(store.has_value()) << store.error().describe();
        EXPECT_TRUE((*store)->roots().empty());
        EXPECT_FALSE((*store)->isTrusted(work));
        ASSERT_TRUE((*store)->trust(work).has_value());
        ASSERT_TRUE((*store)->trust(other).has_value());
        ASSERT_TRUE((*store)->trust(work).has_value()); // Again: nothing new.
        EXPECT_EQ((*store)->roots(), (std::vector<std::string>{work, other}));
    }
    auto content = io::realFileSystem().readFile(storePath);
    ASSERT_TRUE(content.has_value());
    EXPECT_EQ(*content, "# FrostIDE trusted roots v1\n" + work + "\n" + other + "\n");

    auto reopened = TrustStore::open(io::realFileSystem(), storePath);
    ASSERT_TRUE(reopened.has_value()) << reopened.error().describe();
    EXPECT_TRUE((*reopened)->isTrusted(work));
    EXPECT_TRUE((*reopened)->isTrusted(scratch.join(test::native("work/src/main.cpp"))));
    ASSERT_TRUE((*reopened)->untrust(work).has_value());
    EXPECT_FALSE((*reopened)->isTrusted(work));
    EXPECT_TRUE((*reopened)->isTrusted(other));

    auto third = TrustStore::open(io::realFileSystem(), storePath);
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ((*third)->roots(), std::vector<std::string>{other});
}

TEST(TrustStore, CoversChildrenButNotSiblings) {
    MemTree tree;
    auto store = TrustStore::open(tree.fs, MemTree::path("data/trusted-roots.txt"));
    ASSERT_TRUE(store.has_value()) << store.error().describe();
    ASSERT_TRUE((*store)->trust(MemTree::path("work")).has_value());
    EXPECT_TRUE((*store)->isTrusted(MemTree::path("work")));
    EXPECT_TRUE((*store)->isTrusted(MemTree::path("work/a/b.sln")));
    EXPECT_FALSE((*store)->isTrusted(MemTree::path("workshop")));
    EXPECT_FALSE((*store)->isTrusted(MemTree::path("other")));
    EXPECT_FALSE((*store)->isTrusted("relative/path"));
    EXPECT_EQ((*store)->isTrusted(MemTree::path("WORK/x")), kWindows);

    // A child trusted on its own stays trusted when the parent goes.
    ASSERT_TRUE((*store)->trust(MemTree::path("work/keep")).has_value());
    ASSERT_TRUE((*store)->untrust(MemTree::path("work")).has_value());
    EXPECT_TRUE((*store)->isTrusted(MemTree::path("work/keep/x.cpp")));
    EXPECT_FALSE((*store)->isTrusted(MemTree::path("work/x.cpp")));
    ASSERT_TRUE((*store)->untrust(MemTree::path("never-trusted")).has_value());

    auto denied = (*store)->requireTrusted(MemTree::path("work/x.cpp"));
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::PermissionDenied);
    EXPECT_TRUE((*store)->requireTrusted(MemTree::path("work/keep")).has_value());
}

TEST(TrustStore, RefusesBadRootsAndFiles) {
    MemTree tree;
    auto store = TrustStore::open(tree.fs, MemTree::path("trusted-roots.txt"));
    ASSERT_TRUE(store.has_value());
    EXPECT_EQ((*store)->trust("relative").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ((*store)->trust(MemTree::path("a") + "\nC:\\").error().code(), ErrorCode::InvalidArgument);
    if constexpr (!kWindows) {
        EXPECT_EQ((*store)->trust("/w/a/../b").error().code(), ErrorCode::InvalidArgument);
        ASSERT_TRUE((*store)->trust("/w/a").has_value());
        EXPECT_FALSE((*store)->isTrusted("/w/a/../../etc"));
    }

    // A failed save leaves the list unchanged.
    tree.fs.injectFault(io::FsOp::Write, MemTree::path("trusted-roots.txt"), ErrorCode::IoError);
    const auto before = (*store)->roots();
    EXPECT_EQ((*store)->trust(MemTree::path("new")).error().code(), ErrorCode::IoError);
    EXPECT_EQ((*store)->roots(), before);

    tree.write("newer.txt", "# FrostIDE trusted roots v2\n" + MemTree::path("x") + "\n");
    auto newer = TrustStore::open(tree.fs, MemTree::path("newer.txt"));
    ASSERT_FALSE(newer.has_value());
    EXPECT_EQ(newer.error().code(), ErrorCode::Unsupported);

    tree.write("foreign.txt", "C:\\something\n");
    auto foreign = TrustStore::open(tree.fs, MemTree::path("foreign.txt"));
    ASSERT_FALSE(foreign.has_value());
    EXPECT_EQ(foreign.error().code(), ErrorCode::Corrupt);

    // Lines that are not absolute paths are skipped with a warning; CRLF and comments are fine.
    tree.write("mixed.txt",
               "# FrostIDE trusted roots v1\r\n# a comment\r\nnot/absolute\r\n" + MemTree::path("ok") + "\r\n");
    auto mixed = TrustStore::open(tree.fs, MemTree::path("mixed.txt"));
    ASSERT_TRUE(mixed.has_value()) << mixed.error().describe();
    EXPECT_EQ((*mixed)->roots(), std::vector<std::string>{MemTree::path("ok")});
    EXPECT_EQ((*mixed)->warnings().size(), 1u);
}

TEST(TrustStore, DefaultPathIsAbsolute) {
    auto path = TrustStore::defaultStorePath();
    if (!path) {
        GTEST_SKIP() << "no user data directory in this environment: " << path.error().describe();
    }
    EXPECT_TRUE(io::isAbsolutePath(*path)) << *path;
    EXPECT_TRUE(path->ends_with("trusted-roots.txt")) << *path;
}

TEST(TrustStore, GatesBuildsOfTheSolution) {
    MemTree tree;
    tree.write("sln/Demo.slnx", R"(<Solution>
  <Project Path="App/App.vcxproj" />
  <Project Path="../shared/Lib/Lib.vcxproj" />
</Solution>)");
    tree.write("sln/App/App.vcxproj", "<Project />");
    tree.write("shared/Lib/Lib.vcxproj", "<Project />");
    auto model = SolutionModel::load(tree.fs, MemTree::path("sln/Demo.slnx"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    EXPECT_EQ(buildTrustRoots(*model), (std::vector<std::string>{MemTree::path("sln"), MemTree::path("shared/Lib")}));

    auto store = TrustStore::open(tree.fs, MemTree::path("data/trusted-roots.txt"));
    ASSERT_TRUE(store.has_value());
    auto untrusted = checkBuildTrust(**store, *model);
    ASSERT_FALSE(untrusted.has_value());
    EXPECT_EQ(untrusted.error().code(), ErrorCode::PermissionDenied);
    ASSERT_TRUE((*store)->trust(MemTree::path("sln")).has_value());
    auto half = checkBuildTrust(**store, *model);
    ASSERT_FALSE(half.has_value());
    EXPECT_NE(half.error().message().find(MemTree::path("shared/Lib")), std::string::npos) << half.error().message();
    ASSERT_TRUE((*store)->trust(MemTree::path("shared")).has_value());
    EXPECT_TRUE(checkBuildTrust(**store, *model).has_value());
}

} // namespace
} // namespace frost::project
