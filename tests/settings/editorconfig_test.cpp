// SPDX-License-Identifier: Apache-2.0
//
// Tests for EditorConfig support (editorconfig.hpp): the glob rules, the file reader, the upward search
// with root = true and unset, the mapping onto registry keys (charset and end_of_line onto the
// encoding keys), and FileSettings layered over a store snapshot.

#include "frost/io/mem_fs.hpp"
#include "frost/settings/editorconfig.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/store.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace frost::settings;
using frost::io::MemFs;
using frost::io::PathStyle;

TEST(EditorConfigGlob, Rules) {
    EXPECT_TRUE(editorConfigGlobMatches("*", "a.cpp"));
    EXPECT_TRUE(editorConfigGlobMatches("*", "src/deep/a.cpp"));
    EXPECT_TRUE(editorConfigGlobMatches("*.cpp", "src/x.cpp"));
    EXPECT_FALSE(editorConfigGlobMatches("*.cpp", "x.hpp"));
    EXPECT_TRUE(editorConfigGlobMatches("*.{cpp,hpp}", "a.hpp"));
    EXPECT_FALSE(editorConfigGlobMatches("*.{cpp,hpp}", "a.c"));
    EXPECT_TRUE(editorConfigGlobMatches("*.{c{,pp},h}", "a.cpp"));
    EXPECT_TRUE(editorConfigGlobMatches("src/*.cpp", "src/a.cpp"));
    EXPECT_FALSE(editorConfigGlobMatches("src/*.cpp", "src/sub/a.cpp"));
    EXPECT_FALSE(editorConfigGlobMatches("src/*.cpp", "other/src/a.cpp"));
    EXPECT_TRUE(editorConfigGlobMatches("/src/*.cpp", "src/a.cpp"));
    EXPECT_TRUE(editorConfigGlobMatches("src/**/*.cpp", "src/a.cpp"));
    EXPECT_TRUE(editorConfigGlobMatches("src/**/*.cpp", "src/x/y/a.cpp"));
    EXPECT_TRUE(editorConfigGlobMatches("[Mm]akefile", "Makefile"));
    EXPECT_TRUE(editorConfigGlobMatches("[Mm]akefile", "sub/makefile"));
    EXPECT_TRUE(editorConfigGlobMatches("file[!0-9].txt", "fileA.txt"));
    EXPECT_FALSE(editorConfigGlobMatches("file[!0-9].txt", "file1.txt"));
    EXPECT_TRUE(editorConfigGlobMatches("test{1..3}.c", "test2.c"));
    EXPECT_FALSE(editorConfigGlobMatches("test{1..3}.c", "test4.c"));
    EXPECT_TRUE(editorConfigGlobMatches("{-5..5}.c", "-3.c"));
    EXPECT_TRUE(editorConfigGlobMatches("a?c", "abc"));
    EXPECT_FALSE(editorConfigGlobMatches("a?c", "a/c"));
    EXPECT_TRUE(editorConfigGlobMatches("\\*.txt", "*.txt"));
    EXPECT_FALSE(editorConfigGlobMatches("\\*.txt", "a.txt"));
    EXPECT_TRUE(editorConfigGlobMatches("{single}", "{single}"));
    EXPECT_FALSE(editorConfigGlobMatches("*.cpp", "a.CPP")); // Case-sensitive.
}

TEST(EditorConfigFile, Parse) {
    const EditorConfigFile file = parseEditorConfig("\xEF\xBB\xBF# top comment\n"
                                                    "root = TRUE\n"
                                                    "\n"
                                                    "[*]\r\n"
                                                    "Indent_Style = Space\r\n"
                                                    "; another comment\n"
                                                    "custom_key = KeepCase\n"
                                                    "[*.md]\n"
                                                    "trim_trailing_whitespace=false\n"
                                                    "no equals sign\n");
    EXPECT_TRUE(file.root);
    ASSERT_EQ(file.sections.size(), 2U);
    EXPECT_EQ(file.sections[0].glob, "*");
    EXPECT_EQ(file.sections[0].properties, (std::vector<std::pair<std::string, std::string>>{
                                               {"indent_style", "space"}, {"custom_key", "KeepCase"}}));
    EXPECT_EQ(file.sections[1].properties,
              (std::vector<std::pair<std::string, std::string>>{{"trim_trailing_whitespace", "false"}}));
}

struct EditorConfigTest : ::testing::Test {
    frost::ManualClock clock;
    MemFs fs{clock, PathStyle::Posix};

    void write(std::string_view path, std::string_view text) {
        ASSERT_TRUE(fs.createDirectories(frost::io::parentPath(path, PathStyle::Posix)));
        ASSERT_TRUE(fs.writeFileAtomic(path, text));
    }
    void SetUp() override {
        write("/.editorconfig", "[*]\nindent_size = 7\n"); // Above the root: never read.
        write("/w/.editorconfig", "root = true\n"
                                  "[*]\n"
                                  "indent_style = space\nindent_size = 4\nend_of_line = lf\ncharset = utf-8\n"
                                  "[*.{cpp,hpp}]\nindent_size = 2\n"
                                  "[Makefile]\nindent_style = tab\n");
        write("/w/sub/.editorconfig", "[*.cpp]\ntab_width = 8\ncharset = utf-8-bom\nend_of_line = CRLF\n"
                                      "[*.txt]\nindent_size = unset\n");
    }
};

TEST_F(EditorConfigTest, ResolvesUpwardsUntilRoot) {
    const auto cpp = resolveEditorConfig(fs, "/w/sub/a.cpp", PathStyle::Posix);
    ASSERT_TRUE(cpp) << cpp.error().describe();
    EXPECT_EQ(cpp->files, (std::vector<std::string>{"/w/.editorconfig", "/w/sub/.editorconfig"}));
    EXPECT_EQ(cpp->properties, (std::vector<std::pair<std::string, std::string>>{{"charset", "utf-8-bom"},
                                                                                 {"end_of_line", "crlf"},
                                                                                 {"indent_size", "2"},
                                                                                 {"indent_style", "space"},
                                                                                 {"tab_width", "8"}}));
    const auto text = resolveEditorConfig(fs, "/w/sub/notes.txt", PathStyle::Posix);
    ASSERT_TRUE(text);
    EXPECT_EQ(text->find("indent_size"), nullptr); // unset removes the outer file's value.
    EXPECT_NE(text->find("indent_style"), nullptr);

    const auto outside = resolveEditorConfig(fs, "/elsewhere/a.cpp", PathStyle::Posix);
    ASSERT_TRUE(outside);
    EXPECT_EQ(*outside->find("indent_size"), "7");
    EXPECT_FALSE(resolveEditorConfig(fs, "relative/a.cpp", PathStyle::Posix).has_value());
}

TEST_F(EditorConfigTest, MapsOntoRegistryKeys) {
    const Registry& registry = builtinRegistry();
    const auto cpp = mapEditorConfig(registry, resolveEditorConfig(fs, "/w/sub/a.cpp", PathStyle::Posix).value());
    EXPECT_EQ(cpp, (std::vector<std::pair<std::string, Value>>{
                       {"editor.indentSize", Value(std::int64_t{2})},
                       {"editor.insertSpaces", Value(true)},
                       {"editor.tabSize", Value(std::int64_t{8})},
                       {"files.encoding", Value(std::string("UTF-8"))},
                       {"files.lineSeparator", Value(std::string("crlf"))},
                       {"files.utf8Bom", Value(true)},
                   }));
    const auto makefile = mapEditorConfig(registry, resolveEditorConfig(fs, "/w/Makefile", PathStyle::Posix).value());
    EXPECT_EQ(makefile, (std::vector<std::pair<std::string, Value>>{
                            {"editor.indentSize", Value(std::int64_t{4})},
                            {"editor.insertSpaces", Value(false)},
                            {"editor.tabSize", Value(std::int64_t{4})}, // tab_width defaults to indent_size.
                            {"files.encoding", Value(std::string("UTF-8"))},
                            {"files.lineSeparator", Value(std::string("lf"))},
                            {"files.utf8Bom", Value(false)},
                        }));

    EditorConfigProperties odd;
    odd.properties = {{"charset", "latin1"},
                      {"indent_size", "100"},
                      {"indent_style", "weird"},
                      {"insert_final_newline", "true"},
                      {"max_line_length", "off"},
                      {"tab_width", "tab"},
                      {"trim_trailing_whitespace", "true"}};
    std::vector<Diagnostic> diagnostics;
    const auto mapped = mapEditorConfig(registry, odd, &diagnostics);
    EXPECT_EQ(mapped, (std::vector<std::pair<std::string, Value>>{
                          {"editor.insertFinalNewline", Value(true)},
                          {"editor.trimTrailingWhitespace", Value(std::string("all"))},
                          {"files.encoding", Value(std::string("ISO-8859-1"))},
                      }));
    EXPECT_EQ(diagnostics.size(), 3U); // indent_size 100, indent_style weird, tab_width tab.
}

TEST_F(EditorConfigTest, FileSettingsLayerOverASnapshot) {
    const std::string appFile = "/home/u/.config/frostide/settings.json";
    auto store =
        SettingsStore::open(builtinRegistry(), fs, SettingsPaths{.applicationFile = appFile, .project = {}},
                            StoreOptions{.projectTrusted = false, .migrations = nullptr, .style = PathStyle::Posix});
    ASSERT_TRUE(store);
    const auto file = settingsForFile((*store)->snapshot(), fs, "/w/sub/a.cpp", PathStyle::Posix);
    ASSERT_TRUE(file) << file.error().describe();
    EXPECT_EQ(file->get(key::editorTabSize), 8);
    EXPECT_EQ(file->source(key::editorTabSize.name), Scope::FileLevel);
    EXPECT_EQ(file->get(key::filesLineSeparator), "crlf");
    EXPECT_EQ(file->get(key::editorFontSize), 13);
    EXPECT_EQ(file->source(key::editorFontSize.name), Scope::Default);

    ASSERT_TRUE((*store)->set(Scope::Application, key::editorEditorConfig, false));
    const auto disabled = settingsForFile((*store)->snapshot(), fs, "/w/sub/a.cpp", PathStyle::Posix);
    ASSERT_TRUE(disabled);
    EXPECT_EQ(disabled->get(key::editorTabSize), 4);
    EXPECT_EQ(disabled->source(key::editorTabSize.name), Scope::Default);
}

} // namespace
