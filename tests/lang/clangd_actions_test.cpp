// SPDX-License-Identifier: Apache-2.0
//
// Unit tests of clangd_actions.hpp (FND-3): the tweak id of a clangd.applyTweak action, finding a
// tweak by id or by its title, the Refactor This order (known tweaks first, then the other
// refactorings in server order; quick fixes and source actions dropped), recognizing clangd's
// include-cleaner diagnostics and picking its batch fix over a single-include one. The actions are
// shaped as the real clangd 23 sends them (see lang_clangd_tests for the end-to-end run).

#include "frost/lang/clangd_actions.hpp"
#include "frost/lang/json.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

namespace fl = frost::lang;
using fl::Json;

fl::CodeActionItem tweak(std::string id, std::string title, std::string kind = "refactor") {
    fl::CodeActionItem item;
    item.title = std::move(title);
    item.kind = std::move(kind);
    item.commandName = std::string(fl::kClangdApplyTweak);
    item.commandArguments = {Json::object().set("file", "file:///a.cpp").set("tweakID", std::move(id))};
    return item;
}

fl::CodeActionItem plain(std::string title, std::string kind, bool withEdit = false) {
    fl::CodeActionItem item;
    item.title = std::move(title);
    item.kind = std::move(kind);
    if (withEdit) {
        fl::FileEdit file;
        file.path = "a.cpp";
        file.edits.push_back(fl::TextEdit{.range = {{0, 0}, {1, 0}}, .newText = ""});
        item.edit.files.push_back(std::move(file));
    }
    return item;
}

TEST(ClangdActions, TweakIdComesFromTheApplyTweakArgument) {
    EXPECT_EQ(fl::tweakIdOf(tweak("ExtractVariable", "Extract subexpression to variable")), "ExtractVariable");
    fl::CodeActionItem other = tweak("ExtractVariable", "x");
    other.commandName = "other.command";
    EXPECT_EQ(fl::tweakIdOf(other), "");
    EXPECT_EQ(fl::tweakIdOf(plain("Insert ';'", "quickfix")), "");
}

TEST(ClangdActions, FindTweakMatchesTheIdOrTheKnownTitle) {
    const std::vector<fl::CodeActionItem> actions = {
        plain("Insert ';'", "quickfix"),
        tweak("ExtractFunction", "Extract to function"),
        plain("Define constructor", "refactor"), // An older clangd without tweakID in the arguments.
    };
    const fl::CodeActionItem* found = fl::findTweak(actions, "ExtractFunction");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->title, "Extract to function");
    found = fl::findTweak(actions, "MemberwiseConstructor");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->title, "Define constructor");
    EXPECT_EQ(fl::findTweak(actions, "ExtractVariable"), nullptr);
    // A quick fix whose title happens to match is not a tweak.
    const std::vector<fl::CodeActionItem> fixes = {plain("Define constructor", "quickfix")};
    EXPECT_EQ(fl::findTweak(fixes, "MemberwiseConstructor"), nullptr);
}

TEST(ClangdActions, RefactorOrderPutsKnownTweaksFirstAndDropsFixes) {
    std::vector<fl::CodeActionItem> actions = {
        tweak("SwapBinaryOperands", "Swap operands to *"),
        plain("Insert ';'", "quickfix"),
        tweak("SpecialMembers", "Declare implicit copy/move members"),
        plain("Organize", "source.organizeImports"),
        tweak("ExtractVariable", "Extract subexpression to variable"),
        tweak("OverridePureVirtuals", "Override pure virtual methods", "quickfix"),
        plain("Server rewrite", "refactor.rewrite"),
    };
    const auto ordered = fl::orderRefactorActions(actions);
    std::vector<std::string> titles;
    for (const fl::CodeActionItem& action : ordered) {
        titles.push_back(action.title);
    }
    EXPECT_EQ(titles,
              (std::vector<std::string>{"Extract subexpression to variable", "Declare implicit copy/move members",
                                        "Override pure virtual methods", "Swap operands to *", "Server rewrite"}));
}

TEST(ClangdActions, IncludeCleanerDiagnosticsAndTheirBatchFix) {
    fl::LiveDiagnostic unused;
    unused.code = "unused-includes";
    unused.source = "clangd";
    EXPECT_TRUE(fl::isIncludeCleanerDiagnostic(unused));
    unused.code = "missing-includes";
    EXPECT_TRUE(fl::isIncludeCleanerDiagnostic(unused));
    unused.source = "clang";
    EXPECT_FALSE(fl::isIncludeCleanerDiagnostic(unused));
    fl::LiveDiagnostic undeclared;
    undeclared.code = "undeclared_var_use";
    undeclared.source = "clang";
    EXPECT_FALSE(fl::isIncludeCleanerDiagnostic(undeclared));

    const std::vector<fl::CodeActionItem> actions = {
        plain("remove #include directive", "quickfix", true),
        plain("remove all unused includes", "quickfix", true),
    };
    auto fix = fl::pickIncludeCleanerFix(actions);
    ASSERT_TRUE(fix.has_value());
    EXPECT_EQ(fix->title, "remove all unused includes");
    const std::vector<fl::CodeActionItem> single = {plain("remove #include directive", "quickfix", true)};
    fix = fl::pickIncludeCleanerFix(single);
    ASSERT_TRUE(fix.has_value());
    EXPECT_EQ(fix->title, "remove #include directive");
    // Without an edit, or another fix: nothing to apply.
    const std::vector<fl::CodeActionItem> none = {plain("remove all unused includes", "quickfix"),
                                                  plain("Insert ';'", "quickfix", true)};
    EXPECT_FALSE(fl::pickIncludeCleanerFix(none).has_value());
}

} // namespace
