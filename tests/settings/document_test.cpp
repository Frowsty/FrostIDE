// SPDX-License-Identifier: Apache-2.0
//
// Tests for SettingsDocument (document.hpp): parsing with comments, a BOM and trailing commas, and the
// byte-exact edits that keep unknown members, comments and formatting untouched.

#include "frost/settings/document.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <string_view>

namespace {

using namespace frost::settings;
using frost::ErrorCode;

[[nodiscard]] SettingsDocument parsed(std::string text) {
    auto document = SettingsDocument::parse(std::move(text));
    EXPECT_TRUE(document.has_value()) << (document ? "" : document.error().describe());
    return document ? std::move(*document) : SettingsDocument();
}

TEST(SettingsDocument, RoundTripKeepsUnknownKeysByteForByte) {
    const std::string original = "{\n"
                                 "    // FrostIDE settings\n"
                                 "    \"schemaVersion\": 1,\n"
                                 "    \"editor.tabSize\":   2,  /* narrow */\n"
                                 "    \"plugin.unknown\": {\"nested\": [1, 2, {\"x\": \"}\"}]},\n"
                                 "    \"editor.softWrap\": false\n"
                                 "}\n";
    SettingsDocument document = parsed(original);
    ASSERT_EQ(document.members().size(), 4U);
    EXPECT_EQ(document.members()[2].key, "plugin.unknown");
    EXPECT_EQ(document.rawValue(document.members()[2]), "{\"nested\": [1, 2, {\"x\": \"}\"}]}");
    EXPECT_EQ(document.members()[1].line, 4U);
    EXPECT_EQ(document.members()[1].column, 5U);

    // Untouched: parsing alone never changes a byte.
    EXPECT_EQ(document.bytes(), original);

    ASSERT_TRUE(document.set("editor.tabSize", Value(std::int64_t{8})));
    std::string expected = original;
    expected.replace(expected.find("   2,"), 5, "   8,");
    EXPECT_EQ(document.bytes(), expected);

    ASSERT_TRUE(document.set("editor.fontSize", Value(std::int64_t{14})));
    expected.replace(expected.find("false\n}"), 7, "false,\n    \"editor.fontSize\": 14\n}");
    EXPECT_EQ(document.bytes(), expected);

    ASSERT_TRUE(document.remove("editor.tabSize").value());
    expected.erase(expected.find("\"editor.tabSize\""),
                   std::string("\"editor.tabSize\":   8,  /* narrow */\n    ").size());
    EXPECT_EQ(document.bytes(), expected);

    ASSERT_TRUE(document.remove("editor.fontSize").value());
    const std::string_view appended = "false,\n    \"editor.fontSize\": 14\n}";
    expected.replace(expected.find(appended), appended.size(), "false\n}");
    EXPECT_EQ(document.bytes(), expected);
    EXPECT_FALSE(document.remove("editor.fontSize").value());
    EXPECT_NE(document.bytes().find("\"plugin.unknown\": {\"nested\": [1, 2, {\"x\": \"}\"}]},"), std::string::npos);
}

TEST(SettingsDocument, AppendsInTheFilesStyle) {
    SettingsDocument crlf = parsed("{\r\n\t\"a\": 1\r\n}\r\n");
    ASSERT_TRUE(crlf.set("b", Value(true)));
    EXPECT_EQ(crlf.bytes(), "{\r\n\t\"a\": 1,\r\n\t\"b\": true\r\n}\r\n");

    SettingsDocument compact = parsed("{\"a\":1}");
    ASSERT_TRUE(compact.set("b", Value(true)));
    EXPECT_EQ(compact.bytes(), "{\"a\":1, \"b\": true}");

    SettingsDocument trailing = parsed("{\n  \"a\": 1,\n}");
    ASSERT_TRUE(trailing.set("b", Value(std::int64_t{2})));
    EXPECT_EQ(trailing.bytes(), "{\n  \"a\": 1,\n  \"b\": 2,\n}");

    SettingsDocument emptyObject = parsed("{}");
    ASSERT_TRUE(emptyObject.set("a", Value(std::int64_t{1})));
    EXPECT_EQ(emptyObject.bytes(), "{\n  \"a\": 1\n}");

    SettingsDocument empty = parsed("");
    EXPECT_TRUE(empty.isEmpty());
    ASSERT_TRUE(empty.set("a", Value(std::int64_t{1})));
    EXPECT_EQ(empty.bytes(), "{\n  \"a\": 1\n}\n");

    SettingsDocument commentOnly = parsed("// hi\n");
    EXPECT_TRUE(commentOnly.isEmpty());
    ASSERT_TRUE(commentOnly.set("a", Value(std::int64_t{1})));
    EXPECT_EQ(commentOnly.bytes(), "// hi\n{\n  \"a\": 1\n}\n");

    SettingsDocument bom = parsed("\xEF\xBB\xBF{\"a\": 1}");
    ASSERT_EQ(bom.members().size(), 1U);
    ASSERT_TRUE(bom.set("a", Value(std::int64_t{2})));
    EXPECT_EQ(bom.bytes(), "\xEF\xBB\xBF{\"a\": 2}");

    SettingsDocument versioned = parsed("{\n  \"a\": 1\n}");
    ASSERT_TRUE(versioned.setSchemaVersion(1));
    EXPECT_EQ(versioned.bytes(), "{\n  \"schemaVersion\": 1,\n  \"a\": 1\n}");
    ASSERT_TRUE(versioned.setSchemaVersion(2));
    EXPECT_EQ(versioned.bytes(), "{\n  \"schemaVersion\": 2,\n  \"a\": 1\n}");
    EXPECT_EQ(versioned.schemaVersion(), 2U);
}

TEST(SettingsDocument, RenameAndDuplicates) {
    SettingsDocument renamed = parsed("{\"old\": 5, \"keep\": 1}");
    ASSERT_TRUE(renamed.rename("old", "new").value());
    EXPECT_EQ(renamed.bytes(), "{\"new\": 5, \"keep\": 1}");
    EXPECT_FALSE(renamed.rename("absent", "x").value());

    SettingsDocument collision = parsed("{\"old\": 5, \"new\": 1}");
    ASSERT_TRUE(collision.rename("old", "new").value());
    EXPECT_EQ(collision.bytes(), "{\"new\": 1}");

    SettingsDocument twice = parsed("{\"a\": 1, \"a\": 2}");
    EXPECT_EQ(twice.value("a", integerSchema(0, 9)).value(), Value(std::int64_t{2}));
    ASSERT_TRUE(twice.set("a", Value(std::int64_t{3})));
    EXPECT_EQ(twice.bytes(), "{\"a\": 1, \"a\": 3}");
    ASSERT_TRUE(twice.remove("a").value());
    EXPECT_EQ(twice.bytes(), "{}");
}

TEST(SettingsDocument, ValuesAreDecodedBySchema) {
    const SettingsDocument document = parsed(R"({
        "i": 4, "s": "4", "f": 4.5, "big": 99, "n": 1.25, "list": ["a", "b"],
        "records": [{"action": "a.b", "shortcuts": ["Ctrl+D"]}],
        "schemaVersion": -1
    })");
    EXPECT_EQ(document.value("i", integerSchema(1, 32)).value(), Value(std::int64_t{4}));
    EXPECT_EQ(document.value("s", integerSchema(1, 32)).error().message(), "must be an integer");
    EXPECT_EQ(document.value("f", integerSchema(1, 32)).error().message(), "must be an integer");
    EXPECT_EQ(document.value("big", integerSchema(1, 32)).error().message(), "must be between 1 and 32");
    EXPECT_EQ(document.value("n", numberSchema(0, 2)).value(), Value(1.25));
    EXPECT_EQ(document.value("i", numberSchema(0, 9)).value(), Value(4.0));
    EXPECT_EQ(document.value("list", stringListSchema()).value(), Value(StringList{"a", "b"}));
    EXPECT_EQ(document.value("missing", boolSchema()).error().code(), ErrorCode::NotFound);
    const Schema records = recordListSchema(
        {FieldSpec{.name = "action", .type = FieldType::String, .required = true, .description = {}},
         FieldSpec{.name = "shortcuts", .type = FieldType::StringList, .required = true, .description = {}}});
    const auto decoded = document.value("records", records);
    ASSERT_TRUE(decoded) << decoded.error().describe();
    const auto& list = std::get<RecordList>(*decoded);
    ASSERT_EQ(list.size(), 1U);
    EXPECT_EQ(*findField(list[0], "action"), FieldValue(std::string("a.b")));
    EXPECT_FALSE(document.schemaVersion().has_value()); // -1 is not a version.

    SettingsDocument written;
    ASSERT_TRUE(written.set("s", Value(std::string("quote\"and\\slash"))));
    ASSERT_TRUE(written.set("r", Value(RecordList{list})));
    ASSERT_TRUE(written.set("d", Value(1.5)));
    EXPECT_EQ(written.value("s", stringSchema()).value(), Value(std::string("quote\"and\\slash")));
    EXPECT_EQ(written.value("r", records).value(), Value(RecordList{list}));
    EXPECT_EQ(written.rawValue(*written.find("d")), "1.5");
    EXPECT_EQ(written.set("x", Value(std::numeric_limits<double>::infinity())).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(written.setRaw("x", "1 2").error().code(), ErrorCode::InvalidArgument);
}

TEST(SettingsDocument, InvalidTextIsCorruptWithAPosition) {
    const auto missingColon = SettingsDocument::parse("{\n  \"a\": 1,\n  \"b\" 2\n}");
    ASSERT_FALSE(missingColon);
    EXPECT_EQ(missingColon.error().code(), ErrorCode::Corrupt);
    EXPECT_TRUE(missingColon.error().message().starts_with("line 3, column ")) << missingColon.error().message();

    const auto array = SettingsDocument::parse("[1]");
    ASSERT_FALSE(array);
    EXPECT_EQ(array.error().code(), ErrorCode::Corrupt);

    const auto notUtf8 = SettingsDocument::parse("{\"a\": \"\xFF\"}");
    ASSERT_FALSE(notUtf8);
    EXPECT_EQ(notUtf8.error().code(), ErrorCode::Corrupt);

    const auto truncated = SettingsDocument::parse("{\"a\": [1, 2");
    ASSERT_FALSE(truncated);
    EXPECT_EQ(truncated.error().code(), ErrorCode::Corrupt);
}

} // namespace
