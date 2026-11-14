// SPDX-License-Identifier: Apache-2.0
// Explicitly opt-in real-provider source spike. No performance/adoption conclusion is encoded.
#include "frost/lang/lua_service.hpp"
#include "frost/lang/uri.hpp"
#include "frost/io/reactor.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"
#include "../../src/lang/src/timeout.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>
#include <thread>
using namespace frost;
using namespace std::chrono_literals;
namespace {
sched::Task<Result<lang::Json>> bounded(sched::IScheduler& scheduler,lang::LuaService& service,std::string method,lang::Json params) {
    lang::detail::Deadline deadline(scheduler,10s);
    co_return co_await service.request(std::move(method),std::move(params),deadline.token());
}
TEST(LuaProviderComparison, SameDirtySnapshotAndActualCapabilitiesHoverDefinitionRenameReceipts) {
    test::ScratchDir scratch("lua-provider-comparison"); auto& fs=io::realFileSystem();
    const auto root=scratch.join("workspace"),file=scratch.join("workspace/main.lua");
    ASSERT_TRUE(fs.createDirectories(root)); ASSERT_TRUE(fs.writeFileAtomic(file,"local value=1\nprint(value)\n"));
    // Both providers read the same native config; no LuaLS->Emmy setting translation.
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("workspace/.luarc.json"),"{\"runtime\":{\"version\":\"Lua 5.4\"}}"));
    auto pinned=lang::findLuaLanguageServer(fs,lang::defaultLuaSearch());ASSERT_TRUE(pinned);
    const std::string emmy=FROST_EMMYLUA_TEST_EXECUTABLE;
    ASSERT_TRUE(io::isAbsolutePath(emmy));ASSERT_TRUE(fs.exists(emmy).value_or(false));
    lang::Json report=lang::Json::array();
    for(bool experimental:{false,true}) {
        auto reactor=io::Reactor::create();ASSERT_TRUE(reactor);sched::ThreadPool pool({.workerCount=2});
        lang::LuaServiceOptions options;options.workspaceRoot=root;options.trusted=true;
        options.serverPath=experimental?emmy:*pinned; options.displayName=experimental?"EmmyLua Rust":"LuaLS";
        auto args=experimental?lang::emmyLuaArguments(scratch.join("emmy-state")):lang::luaLanguageServerArguments(scratch.join("luals-state"));
        ASSERT_TRUE(args);options.arguments=*args; options.configuration=lang::Json::object();
        lang::LuaService service(io::realProcessLauncher(),{**reactor,pool},options);
        auto dirty=text::Document::fromUtf8("local value=2\nprint(value)\n");ASSERT_TRUE(dirty);
        ASSERT_TRUE(service.openDocument(file,dirty->text())); const auto started=systemClock().steadyNow();ASSERT_TRUE(service.start());
        const auto deadline=started+60s;
        while(service.state()!=lang::LuaState::Running&&systemClock().steadyNow()<deadline) std::this_thread::sleep_for(20ms);
        auto caps=service.capabilities();
        if(!caps) { pool.spawn(sched::Lane::UserVisible,service.stop()).join();FAIL()<<"Provider did not initialize"; }
        lang::Json observed=lang::Json::object().set("provider",options.displayName).set("server",options.serverPath)
            .set("capabilities",caps->raw).set("readyNanoseconds",static_cast<std::int64_t>((systemClock().steadyNow()-started).count()));
        auto uri=lang::pathToUri(file);ASSERT_TRUE(uri);
        for(const auto& [method,supported]:{std::pair{"textDocument/hover",caps->hover},std::pair{"textDocument/definition",caps->definition},std::pair{"textDocument/rename",caps->rename}}) {
            if(!supported) { observed.set(method,lang::Json::object().set("supported",false));continue; }
            auto params=lang::Json::object().set("textDocument",lang::Json::object().set("uri",*uri))
                .set("position",lang::Json::object().set("line",std::int64_t{0}).set("character",std::int64_t{6}));
            if(std::string_view(method)=="textDocument/rename")params.set("newName","renamed");
            const auto before=systemClock().steadyNow();auto result=pool.spawn(sched::Lane::UserVisible,bounded(pool,service,method,std::move(params))).join();
            EXPECT_TRUE(result)<<method;
            observed.set(method,lang::Json::object().set("supported",true).set("elapsedNanoseconds",static_cast<std::int64_t>((systemClock().steadyNow()-before).count()))
                .set("response",result?*result:lang::Json::object().set("error",result.error().describe())));
        }
        lang::Json diagnostics=lang::Json::array();
        if(auto published=service.diagnostics().forFile(file))for(const auto& item:published->diagnostics)
            diagnostics.push(lang::Json::object().set("code",item.code).set("line",std::int64_t{item.line}));
        observed.set("observedDiagnostics",std::move(diagnostics)); report.push(std::move(observed));
        pool.spawn(sched::Lane::UserVisible,service.stop()).join();
        EXPECT_FALSE(service.serverPid());
    }
    RecordProperty("providerComparison",report.dump());
    EXPECT_EQ(fs.readFile(file).value_or(""),"local value=1\nprint(value)\n");
}
}
