// SPDX-License-Identifier: Apache-2.0
// Single-dispatcher asynchronous native language requests: no worker blocking or late double resume.
#include "frost/sched/sim_executor.hpp"

#include "engine_shader_language_requester.hpp"
#include <gtest/gtest.h>
namespace {
using Requester = frost::app::EngineShaderLanguageRequester;
std::shared_ptr<frost::sched::ResourceGovernor> governor() {
    frost::sched::ResourceGovernorOptions options;
    options.hostOverride = frost::sched::HostResources{.logicalCores = 1};
    return std::make_shared<frost::sched::ResourceGovernor>(options);
}
frost::sched::Task<void> replyLater(Requester::Reply reply) {
    reply(frost::lang::Json("actual delayed response"));
    co_return;
}
TEST(EngineShaderRequester, OneDispatcherCanRunDelayedReplyAndReleaseLease) {
    Requester::Reply reply;
    frost::sched::SimExecutor scheduler;
    auto resources = governor();
    Requester requester(
        scheduler, resources,
        [&](const std::string& method, const frost::lang::Json&, const std::stop_token&, Requester::Reply callback) {
            EXPECT_EQ(method, "completion");
            reply = std::move(callback);
        });
    auto request = scheduler.spawn(frost::sched::Lane::Background,
                                   requester.request("textDocument/completion", frost::lang::Json::object()));
    for (int i = 0; i < 16 && !reply; ++i)
        ASSERT_TRUE(scheduler.step());
    ASSERT_TRUE(reply);
    EXPECT_FALSE(request.ready());
    scheduler.spawn(frost::sched::Lane::Background, replyLater(reply)).detach();
    scheduler.runUntilIdle();
    ASSERT_TRUE(request.ready());
    auto result = request.join();
    ASSERT_TRUE(result);
    ASSERT_NE(result->asString(), nullptr);
    EXPECT_EQ(*result->asString(), "actual delayed response");
    EXPECT_TRUE(resources->tryAcquireHeavyLeg(frost::sched::HeavyLegKind::Inspection));
}
TEST(EngineShaderRequester, CancellationStopsActualDispatchTokenAndLateReplyDoesNotResumeAgain) {
    Requester::Reply reply;
    std::stop_token dispatched;
    frost::sched::SimExecutor scheduler;
    auto resources = governor();
    std::stop_source stop;
    Requester requester(
        scheduler, resources,
        [&](const std::string&, const frost::lang::Json&, std::stop_token token, Requester::Reply callback) {
            dispatched = std::move(token);
            reply = std::move(callback);
        });
    auto request =
        scheduler.spawn(frost::sched::Lane::Background,
                        requester.request("textDocument/completion", frost::lang::Json::object(), stop.get_token()));
    for (int i = 0; i < 16 && !reply; ++i)
        ASSERT_TRUE(scheduler.step());
    ASSERT_TRUE(reply);
    stop.request_stop();
    scheduler.runUntilIdle();
    ASSERT_TRUE(request.ready());
    auto result = request.join();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Cancelled);
    EXPECT_TRUE(dispatched.stop_requested());
    reply(frost::lang::Json("late response"));
    scheduler.runUntilIdle();
    EXPECT_TRUE(resources->tryAcquireHeavyLeg(frost::sched::HeavyLegKind::Inspection));
}
TEST(EngineShaderRequester, DeadlineRemainsTimeoutAndLateReplyCannotReplaceIt) {
    Requester::Reply reply;
    std::stop_token dispatched;
    frost::sched::SimExecutor scheduler;
    Requester requester(
        scheduler, governor(),
        [&](const std::string&, const frost::lang::Json&, std::stop_token token, Requester::Reply callback) {
            dispatched = std::move(token);
            reply = std::move(callback);
        });
    auto request = scheduler.spawn(frost::sched::Lane::Background,
                                   requester.request("textDocument/completion", frost::lang::Json::object()));
    scheduler.runUntilIdle();
    ASSERT_TRUE(request.ready());
    auto result = request.join();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Timeout);
    EXPECT_TRUE(dispatched.stop_requested());
    ASSERT_TRUE(reply);
    reply(frost::lang::Json("late response"));
    scheduler.runUntilIdle();
}
TEST(EngineShaderRequester, UppercaseProgramsAreNotMisclassifiedAsIncludes) {
    EXPECT_TRUE(frost::app::isEngineShaderProgram("Assets/Shaders/Upper.GLSL"));
    EXPECT_TRUE(frost::app::isEngineShaderSource("Assets/Shaders/Include/Types.GLSLH"));
    EXPECT_FALSE(frost::app::isEngineShaderProgram("Types.GLSLH"));
}
} // namespace
