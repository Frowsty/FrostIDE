// SPDX-License-Identifier: Apache-2.0
//
// Tests for the settings registry and values (registry.hpp, value.hpp, keys.hpp): the built-in keys
// and their Rider-like defaults, the typed key handles, registration checks, schema checks, and the
// rule that tool paths are Application-only `executes` keys.

#include "frost/settings/keys.hpp"
#include "frost/settings/registry.hpp"
#include "frost/settings/value.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <string_view>

namespace {

using namespace frost::settings;
using frost::ErrorCode;

template<SettingType T>
void expectTypedKey(Key<T> key) {
    const KeySpec* spec = builtinRegistry().find(key.name);
    ASSERT_NE(spec, nullptr) << key.name;
    EXPECT_TRUE(std::holds_alternative<T>(spec->defaultValue)) << key.name;
}

[[nodiscard]] KeySpec sampleSpec(std::string key = "test.flag") {
    return KeySpec{.key = std::move(key),
                   .title = "Flag",
                   .description = "A test flag.",
                   .category = "Tests",
                   .schema = boolSchema(),
                   .defaultValue = Value(false),
                   .scopes = kAllFileScopes,
                   .executes = false,
                   .advanced = false};
}

TEST(SettingsRegistry, BuiltinKeysAreValid) {
    const Registry& registry = builtinRegistry();
    EXPECT_GE(registry.size(), 40U);
    for (const KeySpec& spec : registry.keys()) {
        EXPECT_TRUE(isValidSettingKey(spec.key)) << spec.key;
        EXPECT_FALSE(spec.title.empty()) << spec.key;
        EXPECT_GE(spec.description.size(), 10U) << spec.key; // Good enough for the dialog's help text.
        EXPECT_FALSE(spec.category.empty()) << spec.key;
        EXPECT_TRUE(validateValue(spec.schema, spec.defaultValue).has_value()) << spec.key;
        EXPECT_FALSE(spec.scopes.has(Scope::Default)) << spec.key;
        // Re-adding every key to a fresh registry runs the registration checks on it.
    }
    Registry copy;
    for (const KeySpec& spec : registry.keys()) {
        auto added = copy.add(spec);
        EXPECT_TRUE(added.has_value()) << spec.key << ": " << (added ? "" : added.error().describe());
    }
}

TEST(SettingsRegistry, TypedKeysMatchTheRegistry) {
    expectTypedKey(key::editorFontFamily);
    expectTypedKey(key::editorFontSize);
    expectTypedKey(key::editorLineSpacing);
    expectTypedKey(key::editorFontLigatures);
    expectTypedKey(key::editorSoftWrap);
    expectTypedKey(key::editorLineNumbers);
    expectTypedKey(key::editorShowWhitespace);
    expectTypedKey(key::editorIndentGuides);
    expectTypedKey(key::editorRightMargin);
    expectTypedKey(key::editorShowRightMargin);
    expectTypedKey(key::editorCaretBlinking);
    expectTypedKey(key::editorTabSize);
    expectTypedKey(key::editorIndentSize);
    expectTypedKey(key::editorInsertSpaces);
    expectTypedKey(key::editorDetectIndentation);
    expectTypedKey(key::editorTrimTrailingWhitespace);
    expectTypedKey(key::editorInsertFinalNewline);
    expectTypedKey(key::editorEditorConfig);
    expectTypedKey(key::filesEncoding);
    expectTypedKey(key::filesUtf8Bom);
    expectTypedKey(key::filesLineSeparator);
    expectTypedKey(key::uiTheme);
    expectTypedKey(key::uiThemeSyncLight);
    expectTypedKey(key::uiThemeSyncDark);
    expectTypedKey(key::uiThemeSyncHighContrast);
    expectTypedKey(key::uiCompactMode);
    expectTypedKey(key::uiZoom);
    expectTypedKey(key::keymapId);
    expectTypedKey(key::keymapDoubleTapIntervalMs);
    expectTypedKey(key::keymapOverrides);
    expectTypedKey(key::saveAutosaveOnFrameDeactivation);
    expectTypedKey(key::saveAutosaveOnIdle);
    expectTypedKey(key::saveAutosaveIdleSeconds);
    expectTypedKey(key::saveHotExit);
    expectTypedKey(key::localHistoryEnabled);
    expectTypedKey(key::localHistoryMaxAgeDays);
    expectTypedKey(key::localHistoryMaxTotalMiB);
    expectTypedKey(key::localHistoryKeepNewestPerPath);
    expectTypedKey(key::generalReopenLastProject);
    expectTypedKey(key::generalConfirmExit);
    expectTypedKey(key::buildSaveAllBeforeBuild);
    expectTypedKey(key::buildMsbuildExtraArguments);
    expectTypedKey(key::buildMsbuildMaxCpuCount);
    expectTypedKey(key::cmakeProfiles);
    expectTypedKey(key::toolsMsbuildPath);
    expectTypedKey(key::toolsCmakePath);
    expectTypedKey(key::toolsNinjaPath);
    expectTypedKey(key::toolsGitPath);
    expectTypedKey(key::toolsClangdPath);
    expectTypedKey(key::toolsClangFormatPath);
    expectTypedKey(key::toolsClangTidyPath);
    expectTypedKey(key::toolsClangTidyPlugins);
    expectTypedKey(key::toolsDebuggerPath);
}

TEST(SettingsRegistry, DefaultsAreRiderLike) {
    const Registry& registry = builtinRegistry();
    const auto defaultOf = [&](std::string_view key) -> const Value& { return registry.find(key)->defaultValue; };
    EXPECT_EQ(defaultOf("editor.fontFamily"), Value(std::string("JetBrains Mono")));
    EXPECT_EQ(defaultOf("editor.fontSize"), Value(std::int64_t{13}));
    EXPECT_EQ(defaultOf("editor.tabSize"), Value(std::int64_t{4}));
    EXPECT_EQ(defaultOf("editor.insertSpaces"), Value(true));
    EXPECT_EQ(defaultOf("editor.softWrap"), Value(false));
    EXPECT_EQ(defaultOf("editor.lineNumbers"), Value(true));
    EXPECT_EQ(defaultOf("ui.theme"), Value(std::string("glacier-dark")));
    EXPECT_EQ(defaultOf("keymap.id"), Value(std::string("intellij")));
    EXPECT_EQ(defaultOf("save.autosave.onFrameDeactivation"), Value(true));
    EXPECT_EQ(defaultOf("save.autosave.idleSeconds"), Value(std::int64_t{15}));
    EXPECT_EQ(defaultOf("localHistory.maxAgeDays"), Value(std::int64_t{5}));
    EXPECT_EQ(defaultOf("files.encoding"), Value(std::string("UTF-8")));
    EXPECT_EQ(defaultOf("ui.zoom"), Value(std::int64_t{100}));
}

TEST(SettingsRegistry, ToolPathsAreApplicationOnlyExecutesKeys) {
    std::size_t tools = 0;
    for (const KeySpec& spec : builtinRegistry().keys()) {
        if (spec.key.starts_with("tools.")) {
            ++tools;
            EXPECT_TRUE(spec.executes) << spec.key;
            EXPECT_EQ(spec.scopes, kApplicationOnly) << spec.key;
        }
        // Appearance and keymap choices are per user, never committed.
        if (spec.key.starts_with("ui.") || spec.key.starts_with("keymap.")) {
            EXPECT_EQ(spec.scopes, kApplicationOnly) << spec.key;
        }
    }
    EXPECT_GE(tools, 8U);
}

TEST(SettingsRegistry, AddRejectsBadSpecs) {
    Registry registry;
    ASSERT_TRUE(registry.add(sampleSpec()));
    EXPECT_EQ(registry.add(sampleSpec()).error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(registry.add(sampleSpec("Bad.key")).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(registry.add(sampleSpec("single")).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(registry.add(sampleSpec("a..b")).error().code(), ErrorCode::InvalidArgument);

    KeySpec badDefault = sampleSpec("test.number");
    badDefault.schema = integerSchema(1, 10);
    badDefault.defaultValue = Value(std::int64_t{11});
    EXPECT_EQ(registry.add(badDefault).error().code(), ErrorCode::InvalidArgument);

    KeySpec noOptions = sampleSpec("test.choice");
    noOptions.schema = enumSchema({});
    noOptions.defaultValue = Value(std::string("x"));
    EXPECT_EQ(registry.add(noOptions).error().code(), ErrorCode::InvalidArgument);

    KeySpec defaultScope = sampleSpec("test.scoped");
    defaultScope.scopes = ScopeMask{Scope::Default, Scope::Application};
    EXPECT_EQ(registry.add(defaultScope).error().code(), ErrorCode::InvalidArgument);

    KeySpec noTitle = sampleSpec("test.untitled");
    noTitle.title.clear();
    EXPECT_EQ(registry.add(noTitle).error().code(), ErrorCode::InvalidArgument);

    EXPECT_EQ(registry.size(), 1U);
    EXPECT_NE(registry.find("test.flag"), nullptr);
    EXPECT_EQ(registry.find("test.other"), nullptr);
    EXPECT_TRUE(registry.knowsNamespace("test.typo"));
    EXPECT_FALSE(registry.knowsNamespace("layout"));
    EXPECT_FALSE(registry.knowsNamespace("other.key"));
}

TEST(SettingsValues, SchemaChecks) {
    EXPECT_TRUE(validateValue(integerSchema(1, 32), Value(std::int64_t{4})));
    EXPECT_FALSE(validateValue(integerSchema(1, 32), Value(std::int64_t{0})));
    EXPECT_FALSE(validateValue(integerSchema(1, 32), Value(true)));
    EXPECT_TRUE(validateValue(numberSchema(0.5, 3.0), Value(1.2)));
    EXPECT_FALSE(validateValue(numberSchema(0.5, 3.0), Value(std::numeric_limits<double>::quiet_NaN())));
    const Schema choice = enumSchema({EnumOption{.value = "lf", .label = "LF", .description = {}},
                                      EnumOption{.value = "crlf", .label = "CRLF", .description = {}}});
    EXPECT_TRUE(validateValue(choice, Value(std::string("crlf"))));
    const auto bad = validateValue(choice, Value(std::string("cr")));
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().message(), "must be one of \"lf\", \"crlf\"");
    EXPECT_FALSE(validateValue(stringSchema(3), Value(std::string("long"))));
    EXPECT_FALSE(validateValue(stringSchema(), Value(std::string("\xFF"))));
    EXPECT_FALSE(validateValue(stringListSchema(1), Value(StringList{"a", "b"})));

    const Schema records = recordListSchema(
        {FieldSpec{.name = "action", .type = FieldType::String, .required = true, .description = {}},
         FieldSpec{.name = "shortcuts", .type = FieldType::StringList, .required = true, .description = {}}});
    const Record good = makeRecord({RecordField{.name = "shortcuts", .value = StringList{"Ctrl+D"}},
                                    RecordField{.name = "action", .value = std::string("editor.duplicateLine")}});
    EXPECT_EQ(good.front().name, "action"); // makeRecord sorts.
    EXPECT_TRUE(validateValue(records, Value(RecordList{good})));
    const Record missing = makeRecord({RecordField{.name = "action", .value = std::string("x.y")}});
    EXPECT_FALSE(validateValue(records, Value(RecordList{missing})));
    const Record unknown = makeRecord({RecordField{.name = "action", .value = std::string("x.y")},
                                       RecordField{.name = "shortcuts", .value = StringList{}},
                                       RecordField{.name = "when", .value = std::string("always")}});
    EXPECT_FALSE(validateValue(records, Value(RecordList{unknown})));
    ASSERT_NE(findField(good, "action"), nullptr);
    EXPECT_EQ(findField(good, "nothing"), nullptr);
}

} // namespace
