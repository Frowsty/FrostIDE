// SPDX-License-Identifier: Apache-2.0
//
// Integration tests of LuaService's workspace protocol (LUA-3, LUA-4) against the pinned LuaLS
// 3.19.1; they fail, never skip, when it is missing. Expected diagnostics come from the oracle
// `lua-language-server --check` (the same binary's batch checker, run through IProcessLauncher on
// a copy of the file with the runtime in its .luarc.json), never from hard-coded lists. Covered:
// a runtime change sent as workspace/didChangeConfiguration changes the diagnostics of the running
// server (no restart) and draws its semantic-token and inlay-hint refresh requests; a .luarc.json
// written through lang::writeLuaRc (the settings page's path) takes over on the next change;
// multi-root folders with their own runtime; a new module resolving in require without a restart;
// and LuaLS's $/progress reaching onProgress.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lua_rc.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/templates.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "support/scratch_dir.hpp"
#include "../../src/lang/src/timeout.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using frost::lang::Json;
using Found = std::set<std::pair<int, std::string>>; // (1-based line, LuaLS code).

template<class Predicate>
bool waitFor(Predicate ready, std::chrono::seconds limit = 60s) {
    const auto until = std::chrono::steady_clock::now() + limit;
    for (;;) {
        if (ready())
            return true;
        if (std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::sleep_for(20ms);
    }
}

std::string pinnedLuaLs() {
    auto path = frost::lang::findLuaLanguageServer(frost::io::realFileSystem(), frost::lang::defaultLuaSearch());
    if (!path) {
        ADD_FAILURE() << "the pinned LuaLS " << frost::lang::pinnedLuaLanguageServerVersion()
                      << " is required (tools/bootstrap.ps1 -Only luals, or tools/bootstrap-wsl.sh --only luals): "
                      << path.error().describe();
        return {};
    }
    return *path;
}

void write(const std::string& path, std::string_view text) {
    auto& fs = frost::io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(frost::io::parentPath(path)).has_value()) << path;
    ASSERT_TRUE(fs.writeFileAtomic(path, text).has_value()) << path;
}

/*
 * The oracle: what `lua-language-server --check` reports for `source` as main.lua alone in a
 * folder whose .luarc.json sets `runtime`, at every severity. Runs the pinned binary through
 * IProcessLauncher with its log and meta under `scratch`.
 */
Found oracle(const std::string& server, frost::test::ScratchDir& scratch, std::string_view runtime,
             std::string_view source) {
    std::string name(runtime);
    std::ranges::replace(name, ' ', '-');
    const std::string folder = scratch.join("oracle-" + name);
    write(frost::io::joinPath(folder, "main.lua").value(), source);
    write(frost::io::joinPath(folder, ".luarc.json").value(),
          Json::object().set("runtime", Json::object().set("version", std::string(runtime))).dump());
    const std::string out = scratch.join("oracle-" + name + ".json");
    auto arguments = frost::lang::luaLanguageServerArguments(scratch.join("oracle-state"));
    EXPECT_TRUE(arguments.has_value());
    frost::io::ProcessSpec spec;
    spec.executable = server;
    spec.arguments = arguments.value_or(std::vector<std::string>{});
    spec.arguments.push_back("--check=" + folder);
    spec.arguments.emplace_back("--checklevel=Hint");
    spec.arguments.emplace_back("--check_format=json");
    spec.arguments.push_back("--check_out_path=" + out);
    spec.stdinMode = frost::io::Stdio::Null;
    spec.stdoutMode = frost::io::Stdio::Pipe;
    spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
    spec.killOnClose = true;
    auto process = frost::io::realProcessLauncher().launch(spec);
    EXPECT_TRUE(process.has_value()) << process.error().describe();
    if (!process)
        return {};
    auto result = (*process)->communicate({}, 180s);
    EXPECT_TRUE(result.has_value()) << result.error().describe();
    auto text = frost::io::realFileSystem().readFile(out);
    EXPECT_TRUE(text.has_value()) << "--check wrote no " << out;
    if (!text)
        return {};
    auto report = frost::lang::parseJson(*text);
    EXPECT_TRUE(report.has_value()) << *text;
    Found found;
    // {"<uri>": [diagnostic...]} with findings, [] without.
    if (report && report->isObject()) {
        for (const Json& diagnostics : report->items())
            for (const Json& diagnostic : diagnostics.items())
                found.emplace(
                    static_cast<int>(diagnostic.find("range")->find("start")->integerAt("line").value_or(-1)) + 1,
                    std::string(diagnostic.stringAt("code")));
    }
    return found;
}

Found published(frost::lang::LuaService& service, const std::string& file) {
    Found found;
    if (auto diagnostics = service.diagnostics().forFile(file))
        for (const auto& item : diagnostics->diagnostics)
            found.emplace(item.line, item.code);
    return found;
}

frost::lang::LuaServiceOptions serviceOptions(const std::string& server, frost::test::ScratchDir& scratch,
                                              const std::string& root) {
    frost::lang::LuaServiceOptions options;
    options.serverPath = server;
    options.workspaceRoot = root;
    options.trusted = true;
    auto arguments = frost::lang::luaLanguageServerArguments(scratch.join("state"));
    EXPECT_TRUE(arguments.has_value());
    if (arguments)
        options.arguments = std::move(*arguments);
    return options;
}

// What the callbacks saw, from LuaService's reader thread.
struct Observed {
    std::mutex mutex;
    std::vector<std::string> refreshes;
    std::vector<frost::lang::LuaProgress> progress;
    std::size_t refreshCount(std::string_view method) {
        const std::scoped_lock lock(mutex);
        return static_cast<std::size_t>(std::ranges::count(refreshes, method));
    }
};
void observe(frost::lang::LuaServiceOptions& options, const std::shared_ptr<Observed>& observed) {
    options.onRefresh = [observed](std::string_view method) {
        const std::scoped_lock lock(observed->mutex);
        observed->refreshes.emplace_back(method);
    };
    options.onProgress = [observed](const frost::lang::LuaProgress& progress) {
        const std::scoped_lock lock(observed->mutex);
        observed->progress.push_back(progress);
    };
}

// Grammar the 5.1 runtime rejects and 5.4 accepts, plus one undefined global either way.
constexpr std::string_view kSource = "local a = 1 << 2\n"
                                     "local b = 7 // 2\n"
                                     "local t <const> = 3\n"
                                     "print(a, b, t, frostProbeUndefined)\n";

// LUA-3's exit criterion (the runtime half) and LUA-4's: a runtime change re-sends
// didChangeConfiguration and LuaLS's diagnostics follow it in the running server; a .luarc.json
// written as the settings page writes it then takes over.
TEST(LuaWorkspaceE2E, RuntimeChangesReachTheRunningServerAndItsDiagnostics) {
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    frost::test::ScratchDir scratch("luals-runtime-change");
    const Found lua51 = oracle(server, scratch, "Lua 5.1", kSource);
    const Found lua54 = oracle(server, scratch, "Lua 5.4", kSource);
    ASSERT_FALSE(lua51.empty());
    ASSERT_FALSE(lua54.empty());
    ASSERT_NE(lua51, lua54) << "the oracle must tell the runtimes apart, or this test proves nothing";

    const std::string root = scratch.join("project");
    const std::string file = frost::io::joinPath(root, "main.lua").value();
    write(file, kSource);
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    auto options = serviceOptions(server, scratch, root);
    options.configuration = frost::lang::luaDefaults("Lua 5.1");
    auto observed = std::make_shared<Observed>();
    observe(options, observed);
    frost::lang::LuaService service(frost::io::realProcessLauncher(), {**reactor, pool}, options);
    auto document = frost::text::Document::fromUtf8(std::string(kSource));
    ASSERT_TRUE(document.has_value());
    ASSERT_TRUE(service.openDocument(file, document->text()).has_value());
    ASSERT_TRUE(service.start().has_value());
    EXPECT_TRUE(waitFor([&] { return published(service, file) == lua51; }))
        << "Lua 5.1: " << testing::PrintToString(published(service, file)) << " vs " << testing::PrintToString(lua51);
    const auto pid = service.serverPid();
    ASSERT_TRUE(pid.has_value());

    const std::size_t tokenRefreshes = observed->refreshCount("workspace/semanticTokens/refresh");
    const std::size_t hintRefreshes = observed->refreshCount("workspace/inlayHint/refresh");
    ASSERT_TRUE(service.changeConfiguration(frost::lang::luaDefaults("Lua 5.4"), {}).has_value());
    EXPECT_EQ(service.configurationChangesSent(), 1U);
    EXPECT_TRUE(waitFor([&] { return published(service, file) == lua54; }))
        << "Lua 5.4: " << testing::PrintToString(published(service, file)) << " vs " << testing::PrintToString(lua54);
    // LuaLS asks clients that announced refreshSupport to re-request tokens and hints after a
    // Lua.runtime change (its provider/semantic-tokens.lua and inlay-hint.lua).
    EXPECT_TRUE(waitFor([&] {
        return observed->refreshCount("workspace/semanticTokens/refresh") > tokenRefreshes &&
               observed->refreshCount("workspace/inlayHint/refresh") > hintRefreshes;
    }));

    // The settings page's write: .luarc.json says 5.1, and LuaLS prefers it over the client's 5.4.
    auto rc = frost::lang::readLuaRc(frost::io::realFileSystem(), root);
    ASSERT_TRUE(rc.has_value()) << rc.error().describe();
    frost::lang::LuaRcSettings settings = rc->settings;
    settings.runtimeVersion = "Lua 5.1";
    ASSERT_TRUE(frost::lang::writeLuaRc(frost::io::realFileSystem(), *rc, settings).has_value());
    ASSERT_TRUE(service.changeConfiguration(service.configuration(), {}).has_value());
    EXPECT_EQ(service.configurationChangesSent(), 2U);
    EXPECT_TRUE(waitFor([&] { return published(service, file) == lua51; }))
        << ".luarc.json 5.1: " << testing::PrintToString(published(service, file));

    EXPECT_EQ(service.restarts(), 0U);
    EXPECT_EQ(service.serverPid(), pid) << "the same LuaLS process throughout";
    if (auto error = service.lastError())
        ADD_FAILURE() << error->describe();
    pool.spawn(frost::sched::Lane::UserVisible, service.stop()).join();
    (*reactor)->shutdown();
    pool.shutdown();
}

// Multi-root: a nested folder listed before the root gets its own configuration (scopeUri).
TEST(LuaWorkspaceE2E, EachFolderKeepsItsOwnRuntime) {
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    frost::test::ScratchDir scratch("luals-multi-root");
    const Found lua51 = oracle(server, scratch, "Lua 5.1", kSource);
    const Found lua54 = oracle(server, scratch, "Lua 5.4", kSource);
    ASSERT_NE(lua51, lua54);
    const std::string root = scratch.join("checkout");
    const std::string game = frost::io::joinPath(root, "Samples/Game").value();
    const std::string rootFile = frost::io::joinPath(root, "main.lua").value();
    const std::string gameFile = frost::io::joinPath(game, "main.lua").value();
    write(rootFile, kSource);
    write(gameFile, kSource);
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    auto options = serviceOptions(server, scratch, root);
    options.configuration = frost::lang::luaDefaults("Lua 5.4");
    options.folders = {{game, frost::lang::luaDefaults("Lua 5.1")}};
    frost::lang::LuaService service(frost::io::realProcessLauncher(), {**reactor, pool}, options);
    for (const std::string& file : {rootFile, gameFile}) {
        auto document = frost::text::Document::fromUtf8(std::string(kSource));
        ASSERT_TRUE(document.has_value());
        ASSERT_TRUE(service.openDocument(file, document->text()).has_value());
    }
    ASSERT_TRUE(service.start().has_value());
    EXPECT_TRUE(waitFor([&] { return published(service, gameFile) == lua51 && published(service, rootFile) == lua54; }))
        << "game " << testing::PrintToString(published(service, gameFile)) << ", root "
        << testing::PrintToString(published(service, rootFile));

    // The folder's own runtime changes live; the root's stays.
    ASSERT_TRUE(service.changeConfiguration(service.configuration(), {{game, frost::lang::luaDefaults("Lua 5.4")}})
                    .has_value());
    EXPECT_TRUE(waitFor([&] { return published(service, gameFile) == lua54; }))
        << testing::PrintToString(published(service, gameFile));
    EXPECT_EQ(published(service, rootFile), lua54);
    // A different set of folders is refused: LuaLS appends added folders after the root, so the
    // nested-first order needs a restart (LuaService's callers restart).
    const auto other = service.changeConfiguration(service.configuration(), {});
    ASSERT_FALSE(other.has_value());
    EXPECT_EQ(other.error().code(), frost::ErrorCode::InvalidArgument);
    EXPECT_EQ(service.restarts(), 0U);
    pool.spawn(frost::sched::Lane::UserVisible, service.stop()).join();
    (*reactor)->shutdown();
    pool.shutdown();
}

// LUA-3's exit criterion (the file half): a module created after LuaLS started resolves in
// require without a restart. LuaLS 3.19.1 registers no watched-files watcher (it watches the
// workspace itself), so FrostIDE forwards nothing to it: what it sends follows the registrations.
TEST(LuaWorkspaceE2E, ANewModuleResolvesInRequireWithoutARestart) {
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    frost::test::ScratchDir scratch("luals-new-module");
    const std::string root = scratch.join("project");
    const std::string mainFile = frost::io::joinPath(root, "main.lua").value();
    // The undefined global makes LuaLS publish diagnostics: the sign it has read the file.
    constexpr std::string_view kMain = "local m = require(\"frostnewmod\")\nreturn m, frostProbeUndefined\n";
    write(mainFile, kMain);
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    auto options = serviceOptions(server, scratch, root);
    options.configuration = frost::lang::luaDefaults("Lua 5.4");
    frost::lang::LuaService service(frost::io::realProcessLauncher(), {**reactor, pool}, options);
    auto document = frost::text::Document::fromUtf8(std::string(kMain));
    ASSERT_TRUE(document.has_value());
    ASSERT_TRUE(service.openDocument(mainFile, document->text()).has_value());
    ASSERT_TRUE(service.start().has_value());
    ASSERT_TRUE(waitFor([&] { return service.diagnostics().forFile(mainFile).has_value(); }))
        << "LuaLS never published diagnostics for main.lua";
    const auto pid = service.serverPid();
    const std::string mainUri = frost::lang::pathToUri(mainFile).value();
    const auto definition = [&] {
        const Json params = Json::object()
                                .set("textDocument", Json::object().set("uri", mainUri))
                                .set("position", Json::object().set("line", 0).set("character", 22));
        auto result =
            pool.spawn(frost::sched::Lane::UserVisible, service.request("textDocument/definition", params)).join();
        return result ? result->dump() : std::string("error: ") + result.error().describe();
    };
    EXPECT_EQ(definition().find("frostnewmod.lua"), std::string::npos) << "the module does not exist yet";

    const std::string module = frost::io::joinPath(root, "frostnewmod.lua").value();
    write(module, "return { hello = 1 }\n");
    const std::array changes{frost::lang::LuaFileChange{module, frost::lang::FileChangeType::Created}};
    const auto sent = service.notifyFileChanges(changes);
    ASSERT_TRUE(sent.has_value()) << sent.error().describe();
    const auto watchers = service.fileWatchers();
    const auto matching = static_cast<std::size_t>(std::ranges::count_if(watchers, [&](const auto& watcher) {
        return frost::lang::fileWatcherMatches(watcher, module, frost::lang::FileChangeType::Created);
    }));
    EXPECT_EQ(*sent, matching > 0 ? 1U : 0U) << "sent only what a registration asked for";
    std::string last;
    EXPECT_TRUE(waitFor([&] {
        last = definition();
        return last.find("frostnewmod.lua") != std::string::npos;
    })) << last;
    EXPECT_EQ(service.restarts(), 0U);
    EXPECT_EQ(service.serverPid(), pid);
    pool.spawn(frost::sched::Lane::UserVisible, service.stop()).join();
    (*reactor)->shutdown();
    pool.shutdown();
}

// LuaLS's work-done progress (its workspace loading) reaches onProgress as begin ... end rounds
// under the titles LuaLS chose.
TEST(LuaWorkspaceE2E, ProgressArrivesAsLuaLsSendsIt) {
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    frost::test::ScratchDir scratch("luals-progress");
    const std::string root = scratch.join("project");
    // Enough files that loading outlasts LuaLS's 0.5 s progress delay (workspace/loading.lua).
    std::string body;
    for (int line = 0; line < 60; ++line)
        body += std::format("local value{0} = {{ name = \"v{0}\", size = {0} }}\n", line);
    for (int i = 0; i < 1500; ++i)
        write(frost::io::joinPath(root, std::format("m{:02}/file{}.lua", i % 30, i)).value(), body);
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    auto options = serviceOptions(server, scratch, root);
    options.configuration = frost::lang::luaDefaults("Lua 5.4");
    auto observed = std::make_shared<Observed>();
    observe(options, observed);
    frost::lang::LuaService service(frost::io::realProcessLauncher(), {**reactor, pool}, options);
    ASSERT_TRUE(service.start().has_value());
    const auto ended = [&] {
        const std::scoped_lock lock(observed->mutex);
        for (const auto& begin : observed->progress) {
            if (begin.kind != frost::lang::LuaProgress::Kind::Begin || begin.title.empty())
                continue;
            if (std::ranges::any_of(observed->progress, [&](const auto& item) {
                    return item.token == begin.token && item.kind == frost::lang::LuaProgress::Kind::End;
                }))
                return true;
        }
        return false;
    };
    EXPECT_TRUE(waitFor(ended, 120s)) << "no begin..end progress round from LuaLS";
    {
        const std::scoped_lock lock(observed->mutex);
        for (const auto& item : observed->progress)
            if (item.kind == frost::lang::LuaProgress::Kind::Begin)
                RecordProperty("luals_progress_title", item.title);
    }
    pool.spawn(frost::sched::Lane::UserVisible, service.stop()).join();
    (*reactor)->shutdown();
    pool.shutdown();
}

TEST(LuaTemplatesReal, SharedLuaExpansionsPassThePinnedServersActualBatchCheck) {
    const auto server=pinnedLuaLs(); ASSERT_FALSE(server.empty());
    frost::test::ScratchDir scratch("lua-shared-template-check");
    const auto folder=scratch.join("scripts");
    const std::string prefix="local value = 1\nlocal count = 3\nlocal items = {1,2,3}\nlocal condition = false\nlocal callable = function() return 42 end\n";
    for(const auto& definition:frost::lang::builtinTemplates()) {
        if(definition.language!="lua" || definition.kind!=frost::lang::TemplateKind::Live || definition.body.find("${AUTHOR}")!=std::string_view::npos) continue;
        auto expanded=frost::lang::expandTemplate(definition.body,{}); ASSERT_TRUE(expanded);
        write(folder+"/"+std::string(definition.id)+".lua",prefix+expanded->parsed.text+"\n");
    }
    const auto output=scratch.join("check.json");
    auto arguments=frost::lang::luaLanguageServerArguments(scratch.join("state")); ASSERT_TRUE(arguments);
    arguments->insert(arguments->end(),{"--check="+folder,"--checklevel=Error","--check_format=json","--check_out_path="+output});
    frost::io::ProcessSpec spec; spec.executable=server; spec.arguments=*arguments; spec.stdinMode=frost::io::Stdio::Null; spec.stderrMode=frost::io::Stdio::MergeIntoStdout; spec.killOnClose=true;
    auto result=frost::io::runToCompletion(frost::io::realProcessLauncher(),spec,{},180s); ASSERT_TRUE(result)<<result.error().describe();
    EXPECT_EQ(result->exitCode,0)<<result->stdoutBytes;
    auto report=frost::io::realFileSystem().readFile(output); ASSERT_TRUE(report)<<report.error().describe();
    auto parsed=frost::lang::parseJson(*report); ASSERT_TRUE(parsed);
    if(parsed->isObject()) for(const auto& diagnostics:parsed->items()) for(const auto& diagnostic:diagnostics.items())
        EXPECT_NE(diagnostic.integerAt("severity"),1)<<diagnostic.dump();
}

namespace {
frost::sched::Task<frost::Result<std::vector<frost::lang::Location>>> largeTreeDefinition(
    frost::sched::IScheduler& scheduler,frost::lang::LuaService& service,std::string file) {
    frost::lang::detail::Deadline deadline(scheduler,5s);
    co_return co_await frost::lang::requestLocations(service,frost::lang::NavigationTarget::Definition,
        std::move(file),{0,16},deadline.token());
}
}
TEST(LuaLargeTreeReal, ActualIndexedModuleDefinitionTracksDirtyEditsAndRecordsObservations) {
    const auto server=pinnedLuaLs();ASSERT_FALSE(server.empty());
    frost::test::ScratchDir scratch("lua-large-tree");const auto root=scratch.join("workspace"),file=scratch.join("workspace/main.lua");
    for(unsigned i=0;i<2000;++i)write(scratch.join("workspace/modules/m"+std::to_string(i)+".lua"),"return {value="+std::to_string(i)+"}\n");
    write(scratch.join("workspace/.luarc.json"),"{\"runtime\":{\"path\":[\"modules/?.lua\"],\"pathStrict\":true}}");
    const std::string source="local m=require('m1999')\nprint(m.value)\n";write(file,source);
    auto reactor=frost::io::Reactor::create();ASSERT_TRUE(reactor);
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount=2});
    auto options=serviceOptions(server,scratch,root);
    frost::lang::LuaService service(frost::io::realProcessLauncher(),{**reactor,pool},options);
    auto document=frost::text::Document::fromUtf8(source);ASSERT_TRUE(document);
    ASSERT_TRUE(service.openDocument(file,document->text()));const auto start=frost::systemClock().steadyNow();ASSERT_TRUE(service.start());
    ASSERT_TRUE(waitFor([&]{return service.state()==frost::lang::LuaState::Running;}));
    const auto resolves=[&](const std::string& expected) {
        auto locations=pool.spawn(frost::sched::Lane::UserVisible,largeTreeDefinition(pool,service,file)).join();
        return locations&&std::ranges::any_of(*locations,[&](const auto& location){return frost::lang::documentKey(location.path)==frost::lang::documentKey(expected);});
    };
    EXPECT_TRUE(waitFor([&]{return resolves(scratch.join("workspace/modules/m1999.lua"));}));
    RecordProperty("firstDefinitionNanoseconds",static_cast<std::int64_t>((frost::systemClock().steadyNow()-start).count()));
    const std::string dirty="local m=require('m0')\nprint(m.value)\n";
    auto changed=document->replace({frost::ByteOffset{0},frost::ByteOffset{source.size()}},dirty,{});ASSERT_TRUE(changed);
    const auto before=frost::systemClock().steadyNow();ASSERT_TRUE(service.changeDocument(file,*changed,document->text()));
    EXPECT_TRUE(waitFor([&]{return resolves(scratch.join("workspace/modules/m0.lua"));}));
    RecordProperty("dirtyDefinitionNanoseconds",static_cast<std::int64_t>((frost::systemClock().steadyNow()-before).count()));
    RecordProperty("moduleFiles",2000);
    EXPECT_EQ(frost::io::realFileSystem().readFile(file).value_or(""),source);
    pool.spawn(frost::sched::Lane::UserVisible,service.stop()).join();EXPECT_FALSE(service.serverPid());pool.shutdown();
}
} // namespace
