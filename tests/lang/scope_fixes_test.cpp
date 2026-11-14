// SPDX-License-Identifier: Apache-2.0
// Authored provider-union tests: independent fixes, conflicts, ambiguity and explicit resource refusal.
#include "frost/lang/refactoring.hpp"
#include "frost/lang/uri.hpp"

#include "feature_server.hpp"
#include <gtest/gtest.h>
namespace {
namespace fl = frost::lang;
using fl::Json;
std::string path() {
#ifdef _WIN32
    return "C:\\project\\scope.cpp";
#else
    return "/project/scope.cpp";
#endif
}
Json fix(std::string title, unsigned start, unsigned end, std::string replacement, bool preferred = false) {
    return Json::object()
        .set("title", title)
        .set("kind", "quickfix")
        .set("isPreferred", preferred)
        .set("edit", Json::object().set(
                         "changes", Json::object().set(
                                        fl::pathToUri(path()).value(),
                                        Json::array({Json::object()
                                                         .set("range", fl::toJson(fl::LspRange{{0, start}, {0, end}}))
                                                         .set("newText", replacement)}))));
}
fl::FileDiagnostics diagnostics() {
    fl::LiveDiagnostic first, second;
    first.file = path();
    first.message = "first";
    first.lspRange = {{0, 0}, {0, 1}};
    second.file = path();
    second.message = "second";
    second.lspRange = {{0, 2}, {0, 3}};
    fl::FileDiagnostics file;
    file.file = path();
    file.diagnostics = {first, second};
    return file;
}
TEST(ScopeFixes, UnionsOnlyActualUniqueOrPreferredDirectFixes) {
    frost::test::FeatureServer server;
    server.on("textDocument/codeAction", [](const Json& params) {
        const auto start = params.find("range")->find("start")->integerAt("character").value_or(0);
        return start == 0 ? Json::array({fix("preferred", 0, 1, "A", true), fix("alternative", 0, 1, "B")})
                          : Json::array({fix("second fix", 2, 3, "C")});
    });
    auto result = server.run(fl::requestFixesInScope(server.client(), {diagnostics()}));
    ASSERT_TRUE(result);
    ASSERT_EQ(result->edit.files.size(), 1u);
    EXPECT_EQ(result->edit.editCount(), 2u);
    EXPECT_EQ(result->chosenActions.size(), 2u);
    EXPECT_TRUE(result->unresolved.empty());
}
TEST(ScopeFixes, ConflictingProviderRangesRefuseTheEntireUnion) {
    frost::test::FeatureServer server;
    unsigned requests = 0;
    server.on("textDocument/codeAction", [&](const Json&) {
        return ++requests == 1 ? Json::array({fix("first", 0, 3, "A")}) : Json::array({fix("second", 2, 3, "B")});
    });
    auto result = server.run(fl::requestFixesInScope(server.client(), {diagnostics()}));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::InvalidArgument);
}
TEST(ScopeFixes, AmbiguousFixesRemainUnresolvedInsteadOfChoosingProviderOrder) {
    frost::test::FeatureServer server;
    server.on("textDocument/codeAction",
              [](const Json&) { return Json::array({fix("A", 0, 1, "A"), fix("B", 0, 1, "B")}); });
    auto result = server.run(fl::requestFixesInScope(server.client(), {diagnostics()}));
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->edit.empty());
    EXPECT_EQ(result->unresolved.size(), 2u);
}
TEST(ScopeFixes, UnsupportedResourceActionRemainsVisibleAndCannotBecomePartialTextEdits) {
    auto actions = fl::parseCodeActions(Json::array(
        {Json::object()
             .set("title", "Create declaration file")
             .set("kind", "quickfix")
             .set(
                 "edit",
                 Json::object().set(
                     "documentChanges",
                     Json::array({Json::object().set("kind", "create").set("uri", fl::pathToUri(path()).value())})))}));
    ASSERT_EQ(actions.size(), 1u);
    EXPECT_FALSE(actions[0].disabledReason.empty());
    EXPECT_TRUE(actions[0].edit.empty());
}
} // namespace
