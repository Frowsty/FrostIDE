// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/cmake_language.hpp"
#include "frost/text/text_buffer.hpp"
#include <gtest/gtest.h>
namespace {
using namespace frost;
using namespace frost::lang;
Json params(unsigned line, unsigned character) {
    return Json::object().set("position", toJson(LspPosition{line, character}));
}
TEST(CMakeLanguage, SourceDeclarationsAndConfiguredCacheAreDistinguished) {
    auto buffer = text::TextBuffer::fromUtf8("set(MY_OPTION source)\nmessage(${MY_OPTION})\n").value();
    CMakeLanguageInput input;
    input.path = "C:/project/CMakeLists.txt";
    input.source = buffer.snapshot();
    auto model = std::make_shared<project::CMakeProject>();
    model->cache.push_back({"CACHE_ONLY", "BOOL", "ON"});
    input.model = model;
    auto hover = queryCMakeLanguage(input, "textDocument/hover", params(1, 14));
    ASSERT_TRUE(hover);
    ASSERT_NE(hover->find("contents"), nullptr);
    EXPECT_NE(hover->find("contents")->stringAt("value").find("not evaluated"), std::string_view::npos);
    auto definition = queryCMakeLanguage(input, "textDocument/definition", params(1, 14));
    ASSERT_TRUE(definition);
    ASSERT_EQ(definition->size(), 1);
    EXPECT_EQ(definition->items()[0].find("range")->find("start")->integerAt("line"), 0);
    auto completion = queryCMakeLanguage(input, "textDocument/completion", params(2, 0));
    ASSERT_TRUE(completion);
    EXPECT_GE(completion->size(), 2);
}
TEST(CMakeLanguage, SkipsCommentsDynamicNamesAndConditionalVariables) {
    auto buffer = text::TextBuffer::fromUtf8("# set(FAKE yes)\nset(${NAME} value)\nif(ON)\nset(CONDITIONAL yes)\nendif()\n\n").value();
    CMakeLanguageInput input;
    input.path = "C:/project/CMakeLists.txt";
    input.source = buffer.snapshot();
    auto result = queryCMakeLanguage(input, "textDocument/completion", params(5, 0));
    ASSERT_TRUE(result);
    for (const auto& item : result->items()) {
        EXPECT_NE(item.stringAt("label"), "FAKE");
        EXPECT_NE(item.stringAt("label"), "CONDITIONAL");
    }
    std::stop_source stop;
    stop.request_stop();
    EXPECT_FALSE(queryCMakeLanguage(input, "textDocument/completion", params(5, 0), stop.get_token()));
}
TEST(CMakeLanguage, CompletionKeepsCompletedDeclarationsWhileTypingAnUnfinishedCommand) {
    auto buffer = text::TextBuffer::fromUtf8("set(MY_OPTION value)\nmessage(${MY_").value();
    CMakeLanguageInput input;
    input.path = "C:/project/CMakeLists.txt";
    input.source = buffer.snapshot();
    auto result = queryCMakeLanguage(input, "textDocument/completion", params(1, 13));
    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 1);
    EXPECT_EQ(result->items()[0].stringAt("label"), "MY_OPTION");
}
} // namespace
