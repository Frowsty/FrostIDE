// SPDX-License-Identifier: Apache-2.0
// Authored CPP-8 storage generations, stale dependency exclusion, cancellation and wire refusals.
#include "frost/io/file_system.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/cpp_index.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

namespace {
using frost::lang::Json;
class CppIndex : public ::testing::Test {
protected:
    frost::test::ScratchDir scratch{"cpp-index-store"};
    std::string source = scratch.join("unit.cpp"), header = scratch.join("unit.hpp");
    std::string bytes = "int symbol;\n", include = "// config input\n";
    const std::string hash = std::string(64, 'c');
    void SetUp() override {
        ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(source, bytes));
        ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(header, include));
    }
    frost::lang::CppIndexUnit unit(char identity, std::string name = "symbol") {
        const auto usr = "c:@" + name;
        auto data = Json::object()
                        .set("kind", "frost-semantic-index")
                        .set("schema", 1)
                        .set("status", "ready")
                        .set("commandIdentity", std::string(64, identity))
                        .set("configuration", std::string("config-") + identity)
                        .set("source", source)
                        .set("commandIndex", 0)
                        .set("compilationDatabaseSha256", hash)
                        .set("symbols", Json::array({Json::object()
                                                         .set("usr", usr)
                                                         .set("name", name)
                                                         .set("qualifiedName", name)
                                                         .set("kind", "variable")}))
                        .set("occurrences", Json::array({Json::object()
                                                             .set("usr", usr)
                                                             .set("path", source)
                                                             .set("offset", 4)
                                                             .set("line", 1)
                                                             .set("column", 5)
                                                             .set("roles", 2)
                                                             .set("macro", false)}))
                        .set("relations", Json::array())
                        .set("missingDependencies", Json::array())
                        .set("directoryDependencies", Json::array())
                        .set("dependencies", Json::array({Json::object()
                                                              .set("path", source)
                                                              .set("sha256", frost::io::sha256Hex(bytes))
                                                              .set("bytes", bytes.size()),
                                                          Json::object()
                                                              .set("path", header)
                                                              .set("sha256", frost::io::sha256Hex(include))
                                                              .set("bytes", include.size())}));
        auto parsed = frost::lang::parseCppIndexUnit(data.dump());
        EXPECT_TRUE(parsed);
        return parsed ? std::move(*parsed) : frost::lang::CppIndexUnit{};
    }
};
TEST_F(CppIndex, DifferentConfigurationsOfSameFilePersistAndQueryIndependently) {
    auto store = frost::lang::CppIndexStore::open(scratch.join("index.db"));
    ASSERT_TRUE(store);
    ASSERT_TRUE(store->publish(unit('a'), 1));
    ASSERT_TRUE(store->publish(unit('b'), 1));
    auto rows = store->symbols("sym", hash);
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->locations.size(), 2u);
    EXPECT_NE(rows->locations[0].commandIdentity, rows->locations[1].commandIdentity);
    auto selected = store->symbols("sym", hash, "config-a");
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->locations.size(), 1u);
    auto wrongDatabase = store->symbols("sym", std::string(64, 'd'));
    ASSERT_TRUE(wrongDatabase);
    EXPECT_TRUE(wrongDatabase->locations.empty());
}
TEST_F(CppIndex, DisplayedReceiptCannotBeBlessedByNewerPublicationAfterSave) {
    auto store = frost::lang::CppIndexStore::open(scratch.join("index.db"));
    ASSERT_TRUE(store);
    ASSERT_TRUE(store->publish(unit('a'), 1));
    auto displayed = store->symbols("sym", hash);
    ASSERT_TRUE(displayed);
    ASSERT_EQ(displayed->receipts.size(), 1u);
    ASSERT_TRUE(frost::lang::validateCppIndexInputs(frost::io::realFileSystem(), displayed->receipts.front(), {}));
    include = "// saved changed input\n";
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(header, include));
    ASSERT_TRUE(store->publish(unit('a'), 2));
    EXPECT_FALSE(frost::lang::validateCppIndexInputs(frost::io::realFileSystem(), displayed->receipts.front(), {}));
    auto current = store->symbols("sym", hash);
    ASSERT_TRUE(current);
    ASSERT_EQ(current->receipts.size(), 1u);
    EXPECT_TRUE(frost::lang::validateCppIndexInputs(frost::io::realFileSystem(), current->receipts.front(), {}));
}
TEST_F(CppIndex, HeaderChangesInvalidateActualResultsWithoutDeletingSource) {
    auto store = frost::lang::CppIndexStore::open(scratch.join("index.db"));
    ASSERT_TRUE(store);
    ASSERT_TRUE(store->publish(unit('a'), 1));
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(header, "// changed definition input\n"));
    auto rows = store->symbols("sym", hash);
    ASSERT_TRUE(rows);
    EXPECT_TRUE(rows->locations.empty());
    EXPECT_EQ(rows->staleCommands, 1u);
    EXPECT_EQ(frost::io::realFileSystem().readFile(source).value_or("missing"), bytes);
}
TEST_F(CppIndex, CancelledGenerationDoesNotReplacePriorCompleteUnit) {
    auto store = frost::lang::CppIndexStore::open(scratch.join("index.db"));
    ASSERT_TRUE(store);
    ASSERT_TRUE(store->publish(unit('a'), 1));
    std::stop_source stop;
    stop.request_stop();
    EXPECT_FALSE(store->publish(unit('a', "newName"), 2, stop.get_token()));
    auto old = store->symbols("symbol", hash);
    ASSERT_TRUE(old);
    EXPECT_EQ(old->locations.size(), 1u);
    auto replacement = store->symbols("newName", hash);
    ASSERT_TRUE(replacement);
    EXPECT_TRUE(replacement->locations.empty());
    ASSERT_TRUE(store->invalidate(std::string(64, 'a')));
    auto cleared = store->symbols("symbol", hash);
    ASSERT_TRUE(cleared);
    EXPECT_TRUE(cleared->locations.empty());
}
TEST_F(CppIndex, DecoderRefusesUncapturedOccurrenceAndPublicMetadataMismatch) {
    auto input = unit('a');
    auto changed = input.data;
    changed.set("occurrences", Json::array({Json::object()
                                                .set("usr", "c:@symbol")
                                                .set("path", scratch.join("foreign.cpp"))
                                                .set("offset", 1)
                                                .set("line", 1)
                                                .set("column", 1)
                                                .set("roles", 2)
                                                .set("macro", false)}));
    EXPECT_FALSE(frost::lang::parseCppIndexUnit(changed.dump()));
    input.commandIdentity = std::string(64, 'b');
    EXPECT_FALSE(frost::lang::validateCppIndexInputs(frost::io::realFileSystem(), input, {}));
}
} // namespace
