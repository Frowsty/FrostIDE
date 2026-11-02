// SPDX-License-Identifier: Apache-2.0
//
// Tests for settings migrations (migration.hpp) against the golden files in goldens/: FrostIDE's own
// 0 -> 1 step, a two-step test chain (a key rename and a value conversion, LF and CRLF files), files
// newer than the build, and a failing step.

#include "frost/io/file_system.hpp"
#include "frost/settings/migration.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using namespace frost::settings;
using frost::ErrorCode;
using frost::Result;

[[nodiscard]] std::string golden(std::string_view name) {
    const std::string path = std::string(FROST_SETTINGS_GOLDEN_DIR) + "/" + std::string(name);
    auto bytes = frost::io::realFileSystem().readFile(path);
    EXPECT_TRUE(bytes.has_value()) << path << ": " << (bytes ? "" : bytes.error().describe());
    return bytes ? std::move(*bytes) : std::string();
}

// Version 0 -> 1 renames editor.tabWidth; 1 -> 2 turns editor.wordWrap ("on"/"off") into the boolean
// editor.softWrap in place.
[[nodiscard]] MigrationSet testChain() {
    return MigrationSet(2, {Migration{.from = 0,
                                      .description = "rename editor.tabWidth",
                                      .apply = [](SettingsDocument& document) -> Result<void> {
                                          auto renamed = document.rename("editor.tabWidth", "editor.tabSize");
                                          if (!renamed) {
                                              return std::unexpected(renamed.error());
                                          }
                                          return {};
                                      }},
                            Migration{.from = 1,
                                      .description = "editor.wordWrap becomes editor.softWrap",
                                      .apply = [](SettingsDocument& document) -> Result<void> {
                                          auto old = document.value("editor.wordWrap",
                                                                    enumSchema({{"on", "On", ""}, {"off", "Off", ""}}));
                                          if (!old) {
                                              return {};
                                          }
                                          const bool on = std::get<std::string>(*old) == "on";
                                          if (auto renamed = document.rename("editor.wordWrap", "editor.softWrap");
                                              !renamed) {
                                              return std::unexpected(renamed.error());
                                          }
                                          return document.set("editor.softWrap", Value(on));
                                      }}});
}

void expectGolden(const MigrationSet& migrations, std::string_view input, std::string_view expected,
                  std::uint32_t fromVersion) {
    auto document = SettingsDocument::parse(golden(input));
    ASSERT_TRUE(document) << document.error().describe();
    const auto report = migrations.apply(*document);
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->fromVersion, fromVersion);
    EXPECT_EQ(report->toVersion, migrations.currentVersion());
    EXPECT_TRUE(report->changed);
    EXPECT_EQ(document->bytes(), golden(expected)) << input;

    // Migrating the result again changes nothing.
    const auto again = migrations.apply(*document);
    ASSERT_TRUE(again);
    EXPECT_FALSE(again->changed);
    EXPECT_EQ(document->bytes(), golden(expected));
}

TEST(SettingsMigration, BuiltinStampsSchemaVersion) {
    EXPECT_EQ(builtinMigrations().currentVersion(), kCurrentSchemaVersion);
    expectGolden(builtinMigrations(), "builtin_v0.input.json", "builtin_v0.expected.json", 0);
}

TEST(SettingsMigration, ChainRenamesAndConverts) {
    expectGolden(testChain(), "chain_v0.input.json", "chain_v0.expected.json", 0);
    expectGolden(testChain(), "chain_v1_crlf.input.json", "chain_v1_crlf.expected.json", 1);
}

TEST(SettingsMigration, NewerAndEmptyFilesAreLeftAlone) {
    const std::string newer = "{\"schemaVersion\": 7, \"future.key\": 1}";
    auto document = SettingsDocument::parse(newer);
    ASSERT_TRUE(document);
    const auto report = builtinMigrations().apply(*document);
    ASSERT_TRUE(report);
    EXPECT_TRUE(report->newer);
    EXPECT_FALSE(report->changed);
    EXPECT_EQ(document->bytes(), newer);

    SettingsDocument empty;
    const auto emptyReport = builtinMigrations().apply(empty);
    ASSERT_TRUE(emptyReport);
    EXPECT_FALSE(emptyReport->changed);
    EXPECT_TRUE(empty.bytes().empty());
}

TEST(SettingsMigration, FailingStepLeavesTheDocumentUnchanged) {
    const MigrationSet failing(
        1,
        {Migration{.from = 0, .description = "always fails", .apply = [](SettingsDocument& document) -> Result<void> {
                       (void)document.set("touched", Value(true));
                       return frost::fail(ErrorCode::Corrupt, "cannot migrate");
                   }}});
    const std::string text = "{\"a\": 1}";
    auto document = SettingsDocument::parse(text);
    ASSERT_TRUE(document);
    const auto report = failing.apply(*document);
    ASSERT_FALSE(report);
    EXPECT_EQ(report.error().code(), ErrorCode::Corrupt);
    ASSERT_FALSE(report.error().context().empty());
    EXPECT_NE(report.error().context().front().find("always fails"), std::string::npos);
    EXPECT_EQ(document->bytes(), text);
}

} // namespace
