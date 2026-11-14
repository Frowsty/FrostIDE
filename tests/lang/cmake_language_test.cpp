// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/cmake_language.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/sched/sim_executor.hpp"
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
TEST(CMakeLanguage, ConfiguredInputReadsAreDeferredToBackgroundAndOpenSnapshotsWin) {
    sched::SimExecutor scheduler;
    unsigned reads = 0;
    CMakeLanguageService service(scheduler, [&](std::string_view path, std::size_t limit) -> Result<std::string> {
        ++reads;
        EXPECT_EQ(scheduler.currentLane(), sched::Lane::Background);
        EXPECT_EQ(path, "disk.cmake");
        EXPECT_LE(limit, CMakeLanguageService::kMaxInputBytes);
        return std::string("set(FROM_DISK value)");
    });
    CMakeLanguageInput input;
    input.path = "CMakeLists.txt";
    input.source = text::TextBuffer::fromUtf8("message(${FROM_").value().snapshot();
    input.openInputs.emplace_back("open.cmake", text::TextBuffer::fromUtf8("set(FROM_OPEN unsaved)").value().snapshot());
    auto model = std::make_shared<project::CMakeProject>();
    model->inputs = {{.path = "open.cmake"}, {.path = "disk.cmake"}};
    input.model = model;
    service.setInput(std::move(input));
    auto request = service.request("textDocument/completion", params(0, 15));
    EXPECT_EQ(reads, 0U);
    service.setInput({}); // The pending request retains its original immutable snapshots/model.
    auto job = scheduler.spawn(sched::Lane::Interactive, std::move(request));
    EXPECT_EQ(reads, 0U);
    auto result = job.join();
    ASSERT_TRUE(result);
    EXPECT_EQ(reads, 1U);
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ(result->items()[0].stringAt("label"), "FROM_OPEN");
    EXPECT_EQ(result->items()[1].stringAt("label"), "FROM_DISK");
}
TEST(CMakeLanguage, ConfiguredInputReadsHonorAggregateBudgetAndCancellation) {
    sched::SimExecutor scheduler;
    std::size_t readBytes = 0;
    CMakeLanguageService service(scheduler, [&](std::string_view, std::size_t limit) -> Result<std::string> {
        EXPECT_LE(limit, CMakeLanguageService::kMaxInputBytes);
        readBytes += limit;
        return std::string(limit, ' ');
    });
    CMakeLanguageInput input;
    input.path = "CMakeLists.txt";
    auto model = std::make_shared<project::CMakeProject>();
    for (unsigned i = 0; i < 100; ++i) model->inputs.push_back({.path = "input" + std::to_string(i) + ".cmake"});
    input.model = model;
    service.setInput(input);
    auto job = scheduler.spawn(sched::Lane::Interactive, service.request("textDocument/completion", params(0, 0)));
    ASSERT_TRUE(job.join());
    EXPECT_EQ(readBytes, CMakeLanguageService::kMaxTotalInputBytes);
    std::stop_source stop;
    unsigned reads = 0;
    CMakeLanguageService cancelled(scheduler, [&](std::string_view, std::size_t) -> Result<std::string> {
        ++reads;
        stop.request_stop();
        return std::string("set(STALE value)");
    });
    cancelled.setInput(std::move(input));
    auto stopped = scheduler.spawn(sched::Lane::Interactive,
        cancelled.request("textDocument/completion", params(0, 0), stop.get_token()));
    auto result = stopped.join();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(reads, 1U);
}
} // namespace
