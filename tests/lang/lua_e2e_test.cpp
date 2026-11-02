// SPDX-License-Identifier: Apache-2.0
// Optional integration smoke test against an installed LuaLS on PATH.
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/completion.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "../../src/lang/src/timeout.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace std::chrono_literals;
template<class Predicate>
bool waitFor(Predicate ready) {
    auto until = std::chrono::steady_clock::now() + 20s;
    while (!ready() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(20ms);
    return ready();
}
// The deadline owns its cancellation state until the request completes; its timer keeps shared
// state after disarm, so a late timer cannot access this coroutine's stack.
frost::sched::Task<frost::Result<frost::lang::CompletionList>> boundedCompletion(frost::sched::IScheduler& scheduler,
                                                                                 frost::lang::ILspRequester& server,
                                                                                 std::string file,
                                                                                 frost::IClock::Duration timeout) {
    frost::lang::detail::Deadline deadline(scheduler, timeout);
    co_return co_await frost::lang::requestCompletion(server, std::move(file), {0, 15}, {}, deadline.token());
}

class SilentServer final : public frost::lang::ILspRequester {
public:
    explicit SilentServer(frost::sched::IScheduler& scheduler) : m_scheduler(scheduler) {}
    frost::sched::Task<frost::Result<frost::lang::Json>> request(std::string, frost::lang::Json,
                                                                 std::stop_token token) override {
        static_cast<void>(co_await m_scheduler.sleepFor(std::chrono::hours(1), token));
        co_return frost::fail(frost::ErrorCode::Cancelled, "request cancelled");
    }
    frost::lang::PositionEncoding encoding() const override { return frost::lang::PositionEncoding::Utf16; }
    std::optional<frost::lang::ServerCapabilities> serverCapabilities() const override { return std::nullopt; }

private:
    frost::sched::IScheduler& m_scheduler;
};

TEST(LuaLsE2E, UnansweredCompletionIsCancelledByDeadline) {
    frost::sched::SimExecutor sim;
    SilentServer server(sim);
#ifdef _WIN32
    const std::string file = "C:\\project\\main.lua";
#else
    const std::string file = "/project/main.lua";
#endif
    auto result = sim.spawn(frost::sched::Lane::UserVisible, boundedCompletion(sim, server, file, 100ms)).join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Cancelled);
    EXPECT_EQ(sim.steadyNow().time_since_epoch(), 100ms);
}

TEST(LuaLsE2E, ReportsSyntaxErrorAndExits) {
    auto path = frost::lang::findLuaLanguageServer(frost::io::realFileSystem(), frost::lang::defaultLuaSearch());
    if (!path)
        GTEST_SKIP() << "LuaLS is not on PATH";
    frost::test::ScratchDir scratch("luals");
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    frost::lang::LuaServiceOptions options;
    options.serverPath = *path;
    options.workspaceRoot = scratch.path();
    options.trusted = true;
    frost::lang::LuaService service(frost::io::realProcessLauncher(), {**reactor, pool}, options);
    auto document = frost::text::Document::fromUtf8("local =\n");
    ASSERT_TRUE(document.has_value());
    auto file = scratch.join("bad.lua");
    ASSERT_TRUE(service.openDocument(file, document->text()).has_value());
    ASSERT_TRUE(service.start().has_value());
    const bool diagnosed = waitFor([&] {
        auto found = service.diagnostics().forFile(file);
        return (found && !found->diagnostics.empty()) || service.state() == frost::lang::LuaState::Failed;
    });
    EXPECT_TRUE(diagnosed);
    auto found = service.diagnostics().forFile(file);
    EXPECT_TRUE(found && !found->diagnostics.empty());
    if (auto error = service.lastError())
        ADD_FAILURE() << error->describe();
    pool.spawn(frost::sched::Lane::UserVisible, service.stop()).join();
    EXPECT_EQ(service.state(), frost::lang::LuaState::Stopped);
    (*reactor)->shutdown();
    pool.shutdown();
}
TEST(LuaLsE2E, CompletesGeneratedFrostEngineApi) {
    auto server = frost::lang::findLuaLanguageServer(frost::io::realFileSystem(), frost::lang::defaultLuaSearch());
    std::string apiPath;
#ifdef _WIN32
    const DWORD size = GetEnvironmentVariableW(L"FROST_TEST_LUA_API", nullptr, 0);
    if (size) {
        std::wstring value(size, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"FROST_TEST_LUA_API", value.data(), size);
        if (length && length < size) {
            value.resize(length);
            if (auto utf8 = frost::io::fromWide(value))
                apiPath = std::move(*utf8);
        }
    }
#else
    if (const char* value = std::getenv("FROST_TEST_LUA_API"))
        apiPath = value;
#endif
    if (!server || apiPath.empty())
        GTEST_SKIP() << "LuaLS or FROST_TEST_LUA_API is not available";
    auto& fs = frost::io::realFileSystem();
    auto api = fs.readFile(apiPath);
    ASSERT_TRUE(api.has_value());
    frost::test::ScratchDir scratch("lua-engine-api");
    auto directory = scratch.join(".frost/lua-api");
    ASSERT_TRUE(fs.createDirectories(directory).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join(".frost/lua-api/frost.lua"), *api).has_value());
    ASSERT_TRUE(
        fs.writeFileAtomic(
              scratch.join(".luarc.json"),
              R"({"runtime":{"version":"Lua 5.4"},"workspace":{"library":[".frost/lua-api"],"checkThirdParty":false}})")
            .has_value());
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    frost::lang::LuaServiceOptions options;
    options.serverPath = *server;
    options.workspaceRoot = scratch.path();
    options.trusted = true;
    options.configuration = frost::lang::luaDefaults("Lua 5.4", true);
    frost::lang::LuaService service(frost::io::realProcessLauncher(), {**reactor, pool}, options);
    auto document = frost::text::Document::fromUtf8("local x = AI.Wa\nAI.DoesNotExist()\n");
    ASSERT_TRUE(document.has_value());
    const std::string file = scratch.join("main.lua");
    ASSERT_TRUE(service.openDocument(file, document->text()).has_value());
    ASSERT_TRUE(service.start().has_value());
    bool running = waitFor([&] {
        return service.state() == frost::lang::LuaState::Running || service.state() == frost::lang::LuaState::Failed;
    });
    EXPECT_TRUE(running && service.state() == frost::lang::LuaState::Running);
    bool completed = false;
    if (service.state() == frost::lang::LuaState::Running) {
        completed = waitFor([&] {
            auto result =
                pool.spawn(frost::sched::Lane::UserVisible, boundedCompletion(pool, service, file, 2s)).join();
            return result && std::ranges::any_of(result->items, [](const auto& item) {
                       return item.label.find("Wake") != std::string::npos;
                   });
        });
    }
    EXPECT_TRUE(completed) << "generated AI.Wake was absent from completion";
    const bool badField = waitFor([&] {
        auto diagnostic = service.diagnostics().forFile(file);
        return diagnostic && std::ranges::any_of(diagnostic->diagnostics, [](const auto& item) {
                   return item.line == 2 && item.message.find("DoesNotExist") != std::string::npos;
               });
    });
    EXPECT_TRUE(badField) << "invalid engine member was not diagnosed";
    if (auto diagnostic = service.diagnostics().forFile(file)) {
        EXPECT_FALSE(std::ranges::any_of(diagnostic->diagnostics, [](const auto& item) {
            return item.code == "undefined-global" && item.message.find("AI") != std::string::npos;
        }));
    }
    if (auto error = service.lastError())
        ADD_FAILURE() << error->describe();
    pool.spawn(frost::sched::Lane::UserVisible, service.stop()).join();
    (*reactor)->shutdown();
    pool.shutdown();
}
} // namespace
