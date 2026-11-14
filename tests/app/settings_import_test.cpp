// SPDX-License-Identifier: Apache-2.0
// Clean-room fixture import, exact application-file expectations and durable scheme round-trip.
#include "../../src/app/src/settings_import.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/settings/keys.hpp"
#include <gtest/gtest.h>
namespace frost::app {
namespace {
constexpr std::string_view fixture = R"xml(<UserSettings><Category><FontsAndColors><Categories>
<Category GUID="editor-example" FontName="Consolas" FontSize="14"><Items>
<Item Name="Plain Text" Foreground="0x00332211" Background="0x00665544"/>
<Item Name="Comment" Foreground="0x00998877" Background="0x02000000"/>
</Items></Category><Category GUID="output-example" FontName="Other" FontSize="10"/>
</Categories></FontsAndColors><KeyboardShortcuts>
<Shortcut Command="Edit.Copy" Scope="Text Editor">Ctrl+C</Shortcut>
<Shortcut Command="Extension.Unknown" Scope="Global">Ctrl+Q</Shortcut>
</KeyboardShortcuts></Category></UserSettings>)xml";
TEST(SettingsImport, CategoriesRemainExplicitAndColorRefsDecodeAsBgr) {
    auto parsed = parseVisualStudioSettings(fixture);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->fonts.size(), 2U);
    EXPECT_EQ(parsed->fonts[0].font, "Consolas");
    ASSERT_EQ(parsed->fonts[0].colors.size(), 3U);
    const auto& foreground = parsed->fonts[0].colors[1];
    EXPECT_EQ(std::get<std::string>(*settings::findField(foreground, "color")), "#112233");
    ASSERT_EQ(parsed->shortcuts.size(), 1U);
    EXPECT_EQ(std::get<std::string>(*settings::findField(parsed->shortcuts[0], "action")), "editor.copy");
    EXPECT_FALSE(parseVisualStudioSettings("<!DOCTYPE UserSettings [<!ENTITY foo 'x'>]><UserSettings/>"));
}
TEST(SettingsImport, PreviewWritesNothingAndConditionalApplyIsAtomicAndReloadable) {
    ManualClock clock;
    io::MemFs fs(clock, io::PathStyle::Posix);
    ASSERT_TRUE(fs.createDirectories("/settings"));
    settings::SettingsPaths paths{.applicationFile = "/settings/settings.json"};
    const settings::StoreOptions options{.style = io::PathStyle::Posix};
    auto store = settings::SettingsStore::open(settings::builtinRegistry(), fs, paths, options).value();
    auto imported = parseVisualStudioSettings(fixture).value();
    auto preview = prepareVisualStudioImport(*store, imported, 0);
    ASSERT_TRUE(preview);
    EXPECT_EQ(store->snapshot()->get(settings::key::editorFontFamily), "JetBrains Mono");
    ASSERT_TRUE(store->set(settings::Scope::Application, settings::key::editorFontSize, std::int64_t{15}));
    EXPECT_FALSE(store->applyConditional(settings::Scope::Application, *preview));
    EXPECT_EQ(store->snapshot()->get(settings::key::editorFontFamily), "JetBrains Mono");
    preview = prepareVisualStudioImport(*store, imported, 0);
    ASSERT_TRUE(preview);
    ASSERT_TRUE(store->applyConditional(settings::Scope::Application, *preview));
    auto reopened = settings::SettingsStore::open(settings::builtinRegistry(), fs, paths, options).value();
    EXPECT_EQ(reopened->snapshot()->get(settings::key::editorFontFamily), "Consolas");
    EXPECT_EQ(reopened->snapshot()->get(settings::key::editorFontSize), 14);
    EXPECT_EQ(reopened->snapshot()->get(settings::key::editorSchemeColors), imported.fonts[0].colors);
}
}
} // namespace frost::app
