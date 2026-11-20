// SPDX-License-Identifier: Apache-2.0
// At most one governed heavy analysis lease; cancellation also reaches the actual engine session request.
#include "engine_shader_language_requester.hpp"

#include <coroutine>
#include <mutex>
#include <utility>
namespace frost::app {
bool isEngineShaderProgram(std::string_view path) {
    if (path.size() < 5)
        return false;
    auto suffix = std::string(path.substr(path.size() - 5));
    for (char& c : suffix)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + 32);
    return suffix == ".glsl";
}
bool isEngineShaderSource(std::string_view path) {
    const auto dot = path.find_last_of('.');
    if (dot == path.npos)
        return false;
    std::string ext(path.substr(dot));
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return ext == ".glsl" || ext == ".glslh";
}
std::optional<lang::ServerCapabilities> EngineShaderLanguageRequester::serverCapabilities() const {
    lang::ServerCapabilities caps;
    caps.serverName = "FrostEngine GLSL";
    caps.positionEncoding = lang::PositionEncoding::Utf8;
    caps.sync = lang::TextDocumentSyncKind::None;
    caps.openClose = false;
    caps.completion = caps.definition = caps.declaration = caps.hover = caps.documentSymbol = true;
    caps.completionTriggerCharacters = {".", "/", "\""};
    return caps;
}
sched::Task<Result<lang::Json>> EngineShaderLanguageRequester::request(std::string method, lang::Json params,
                                                                       std::stop_token stop) {
    return answer(m_scheduler, m_governor, m_dispatch, std::move(method), std::move(params), stop);
}
namespace {
struct ShaderPending {
    sched::IScheduler& Scheduler;
    std::mutex Mutex;
    std::optional<Result<lang::Json>> Outcome;
    std::coroutine_handle<> Waiter;
    explicit ShaderPending(sched::IScheduler& scheduler) : Scheduler(scheduler) {}
    void complete(Result<lang::Json> value) {
        std::coroutine_handle<> wake;
        {
            std::lock_guard lock(Mutex);
            if (Outcome)
                return;
            Outcome.emplace(std::move(value));
            wake = std::exchange(Waiter, {});
        }
        if (wake)
            Scheduler.enqueue(wake, sched::Lane::Background);
    }
};
struct ShaderAwaiter {
    std::shared_ptr<ShaderPending> Pending;
    bool await_ready() {
        std::lock_guard lock(Pending->Mutex);
        return Pending->Outcome.has_value();
    }
    bool await_suspend(std::coroutine_handle<> waiter) {
        std::lock_guard lock(Pending->Mutex);
        if (Pending->Outcome)
            return false;
        Pending->Waiter = waiter;
        return true;
    }
    Result<lang::Json> await_resume() {
        std::lock_guard lock(Pending->Mutex);
        return std::move(*Pending->Outcome);
    }
};
sched::Task<void> shaderDeadline(sched::IScheduler& scheduler, std::shared_ptr<ShaderPending> pending,
                                 IClock::SteadyTime deadline, std::stop_source cancel, std::stop_token stop) {
    if (co_await scheduler.sleepUntil(deadline, stop)) {
        pending->complete(fail(ErrorCode::Timeout, "Native GLSL request timed out"));
        cancel.request_stop();
    }
    co_return;
}
} // namespace
sched::Task<Result<lang::Json>> EngineShaderLanguageRequester::answer(sched::IScheduler& scheduler,
                                                                      std::shared_ptr<sched::ResourceGovernor> governor,
                                                                      Dispatch dispatch, std::string method,
                                                                      lang::Json params, std::stop_token stop) {
    co_await scheduler.schedule(sched::Lane::Background);
    std::string query;
    if (method == "textDocument/completion")
        query = "completion";
    else if (method == "textDocument/definition" || method == "textDocument/declaration")
        query = "definition";
    else if (method == "textDocument/hover")
        query = "hover";
    else if (method == "textDocument/documentSymbol")
        query = "symbols";
    else if (method == "textDocument/diagnostic")
        query = "diagnostics";
    else
        co_return fail(ErrorCode::Unsupported, "This operation is unavailable in the native GLSL profile");
    std::stop_source cancellation;
    std::stop_callback caller(stop, [&] { cancellation.request_stop(); });
    std::stop_callback shutdown(scheduler.stopToken(), [&] { cancellation.request_stop(); });
    const auto token = cancellation.get_token();
    const auto deadline = scheduler.clock().steadyNow() + std::chrono::seconds(30);
    std::optional<sched::HeavyLegLease> lease;
    while (!token.stop_requested()) {
        auto admitted = governor->tryAcquireHeavyLeg(sched::HeavyLegKind::Inspection);
        if (admitted) {
            lease.emplace(std::move(*admitted));
            break;
        }
        if (admitted.error().code() != ErrorCode::Busy)
            co_return std::unexpected(admitted.error());
        if (scheduler.clock().steadyNow() >= deadline)
            co_return fail(ErrorCode::Timeout, "Native GLSL analysis resource wait timed out");
        co_await scheduler.sleepFor(std::chrono::milliseconds(20), token);
    }
    if (token.stop_requested())
        co_return fail(ErrorCode::Cancelled, "Native GLSL request cancelled");
    auto pending = std::make_shared<ShaderPending>(scheduler);
    std::stop_callback cancelled(
        token, [pending] { pending->complete(fail(ErrorCode::Cancelled, "Native GLSL request cancelled")); });
    std::stop_source timerStop;
    auto timer = scheduler.spawn(sched::Lane::Background,
                                 shaderDeadline(scheduler, pending, deadline, cancellation, timerStop.get_token()));
    dispatch(std::move(query), std::move(params), token,
             [pending](Result<lang::Json> result) { pending->complete(std::move(result)); });
    auto result = co_await ShaderAwaiter{pending};
    timerStop.request_stop();
    co_await timer;
    co_return result;
}
} // namespace frost::app
