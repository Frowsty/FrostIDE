// SPDX-License-Identifier: Apache-2.0
// Checks Unreal compile command rebasing, snapshot identity, and trust changes.
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/unreal_compile_commands.hpp"
#include "frost/project/trust_store.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>
namespace frost::lang {
TEST(UnrealCompileCommands, RebasesProducerPathsAndPreservesExactArguments) {
    const auto root = io::kNativePathStyle == io::PathStyle::Windows ? "C:/Synthetic" : "/Synthetic";
    auto result = normalizeUnrealCompileDatabase(
        R"([{"directory":"build","file":"../Source/Oracle.cpp","arguments":["compiler","-DFLAG=exact value","@generated.rsp","-c","../Source/Oracle.cpp"]}])",
        root);
    ASSERT_TRUE(result);
    auto json = parseJson(*result);
    ASSERT_TRUE(json);
    EXPECT_EQ(json->items()[0].stringAt("directory"), io::joinPath(root, "build").value());
    EXPECT_EQ(json->items()[0].stringAt("file"),
              io::joinPath(io::joinPath(root, "build").value(), "../Source/Oracle.cpp").value());
    EXPECT_EQ(*json->items()[0].find("arguments")->items()[2].asString(), std::string("@generated.rsp"));
    EXPECT_FALSE(normalizeUnrealCompileDatabase("[]", root));
    EXPECT_FALSE(normalizeUnrealCompileDatabase(
        R"([{"directory":"build","file":"a.cpp","file":"b.cpp","command":"compiler"}])", root));
}
TEST(UnrealCompileCommands, ContentAddressedSnapshotsPreserveOldProfileAndRejectTrustRevisionChanges) {
    frost::test::ScratchDir scratch("unreal-db");
    auto& fs = io::realFileSystem();
    const auto project = scratch.join("Oracle.uproject"), database = scratch.join("compile_commands.json");
    const auto bytes = Json::array({Json::object()
                                        .set("directory", scratch.path())
                                        .set("file", "Oracle.cpp")
                                        .set("arguments", Json::array({"compiler", "-DORIGINAL", "-c", "Oracle.cpp"}))})
                           .dump();
    ASSERT_TRUE(fs.writeFileAtomic(database, bytes));
    auto trust = project::TrustStore::open(fs, scratch.join("trust.txt"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(scratch.path()));
    const auto revision = (*trust)->snapshot(scratch.path()).revision;
    UnrealCompileCommandsProvider provider(fs, **trust, {project, database, "OracleEditor/Linux/Development"},
                                           scratch.join("cache"), revision);
    auto first = provider.locate();
    ASSERT_TRUE(first);
    ASSERT_TRUE(*first);
    const auto firstBytes = fs.readFile((**first).file);
    ASSERT_TRUE(firstBytes);
    ASSERT_TRUE(fs.writeFileAtomic(
        database, Json::array({Json::object()
                                   .set("directory", scratch.path())
                                   .set("file", "Oracle.cpp")
                                   .set("arguments", Json::array({"compiler", "-DCHANGED", "-c", "Oracle.cpp"}))})
                      .dump()));
    auto second = provider.locate();
    ASSERT_TRUE(second);
    ASSERT_TRUE(*second);
    EXPECT_NE((**first).directory, (**second).directory);
    EXPECT_EQ(fs.readFile((**first).file).value(), *firstBytes);
    ASSERT_TRUE((*trust)->untrust(scratch.path()));
    ASSERT_TRUE((*trust)->trust(scratch.path()));
    auto stale = provider.locate();
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code(), ErrorCode::Untrusted);
}
} // namespace frost::lang
