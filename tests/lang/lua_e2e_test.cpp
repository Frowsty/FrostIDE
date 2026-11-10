// SPDX-License-Identifier: Apache-2.0
// Integration tests against the pinned LuaLS (sdk/sdk.lock.json, ADR 0079), which
// tools/bootstrap.ps1 and tools/bootstrap-wsl.sh install: they fail, never skip, when it is
// missing (LUA-1). Covered: the version the pinned binary reports, a syntax error and an undefined
// global diagnosed through LuaService, --logpath/--metapath keeping LuaLS's files out of its
// installation, and completion plus field diagnostics from a FrostEngine API library with the
// FrostEngine defaults (the engine-generated file when FROST_TEST_LUA_API names one, else a small
// hand-written stand-in of the same shape).
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/code_assist.hpp"
#include "frost/lang/completion.hpp"
#include "frost/lang/frost_engine_lua.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/refactoring.hpp"
#include "frost/lang/semantic_tokens.hpp"
#include "frost/lang/symbols.hpp"
#include "frost/lang/uri.hpp"
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
    const auto until = std::chrono::steady_clock::now() + 20s;
    // Returns the evaluation that ended the wait (as tests/app/workbench_rig.hpp does): evaluating
    // again after the loop could see a state changed in between (R0 follow-up 16).
    for (;;) {
        if (ready())
            return true;
        if (std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::sleep_for(20ms);
    }
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

// The pinned LuaLS, or "" after recording a failure that says how to install it.
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

// Service options for the pinned LuaLS over `scratch`, its log and meta under scratch/state.
frost::lang::LuaServiceOptions serviceOptions(const std::string& server, frost::test::ScratchDir& scratch) {
    frost::lang::LuaServiceOptions options;
    options.serverPath = server;
    options.workspaceRoot = scratch.path();
    options.trusted = true;
    auto arguments = frost::lang::luaLanguageServerArguments(scratch.join("state"));
    EXPECT_TRUE(arguments.has_value());
    if (arguments)
        options.arguments = std::move(*arguments);
    return options;
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

// The binary the search finds is the pinned release: `--version` through IProcessLauncher.
TEST(LuaLsE2E, PinnedServerReportsThePinnedVersion) {
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    frost::test::ScratchDir scratch("luals-version");
    // Even --version writes a service log, so it too goes under the state directory.
    auto arguments = frost::lang::luaLanguageServerArguments(scratch.join("state"));
    ASSERT_TRUE(arguments.has_value());
    frost::io::ProcessSpec spec;
    spec.executable = server;
    spec.arguments = std::move(*arguments);
    spec.arguments.emplace_back("--version");
    spec.stdinMode = frost::io::Stdio::Null;
    spec.stdoutMode = frost::io::Stdio::Pipe;
    spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
    spec.killOnClose = true;
    auto process = frost::io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    auto result = (*process)->communicate({}, 60s);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_NE(result->stdoutBytes.find(frost::lang::pinnedLuaLanguageServerVersion()), std::string::npos)
        << result->stdoutBytes;
}

TEST(LuaLsE2E, ReportsSyntaxErrorAndExits) {
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    frost::test::ScratchDir scratch("luals");
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    frost::lang::LuaService service(frost::io::realProcessLauncher(), {**reactor, pool},
                                    serviceOptions(server, scratch));
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
    // --logpath put LuaLS's own log under the per-user state directory.
    EXPECT_TRUE(frost::io::realFileSystem().exists(scratch.join("state/log")).value_or(false));
    (*reactor)->shutdown();
    pool.shutdown();
}

// LUA-1's acceptance: an undefined global in an open .lua file yields LuaLS's undefined-global
// diagnostic on that line, with the name in the message.
TEST(LuaLsE2E, UndefinedGlobalIsDiagnosed) {
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    frost::test::ScratchDir scratch("luals-undefined-global");
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    frost::lang::LuaService service(frost::io::realProcessLauncher(), {**reactor, pool},
                                    serviceOptions(server, scratch));
    auto document = frost::text::Document::fromUtf8("local known = 1\nprint(known + frostUndefinedThing)\n");
    ASSERT_TRUE(document.has_value());
    const std::string file = scratch.join("main.lua");
    ASSERT_TRUE(service.openDocument(file, document->text()).has_value());
    ASSERT_TRUE(service.start().has_value());
    const auto undefinedGlobal = [&] {
        auto found = service.diagnostics().forFile(file);
        return found && std::ranges::any_of(found->diagnostics, [](const auto& item) {
                   return item.code == "undefined-global" && item.line == 2 &&
                          item.message.find("frostUndefinedThing") != std::string::npos;
               });
    };
    EXPECT_TRUE(waitFor([&] { return undefinedGlobal() || service.state() == frost::lang::LuaState::Failed; }));
    EXPECT_TRUE(undefinedGlobal()) << "LuaLS did not report frostUndefinedThing as an undefined global";
    if (auto found = service.diagnostics().forFile(file)) {
        // `known` is a defined local and `print` a builtin: neither may be reported.
        EXPECT_FALSE(std::ranges::any_of(found->diagnostics, [](const auto& item) {
            return item.message.find("known") != std::string::npos || item.message.find("print") != std::string::npos;
        }));
    }
    if (auto error = service.lastError())
        ADD_FAILURE() << error->describe();
    pool.spawn(frost::sched::Lane::UserVisible, service.stop()).join();
    (*reactor)->shutdown();
    pool.shutdown();
}

// The engine-generated API when FROST_TEST_LUA_API names it, else a stand-in of the same shape.
std::string engineApi() {
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
    if (!apiPath.empty()) {
        auto api = frost::io::realFileSystem().readFile(apiPath);
        EXPECT_TRUE(api.has_value()) << apiPath;
        if (api)
            return std::move(*api);
    }
    return "---@meta\n"
           "---@class AI\n"
           "AI = {}\n"
           "---Wakes the agent.\n"
           "function AI.Wake() end\n";
}

TEST(LuaLsE2E, CompletesGeneratedFrostEngineApi) {
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    const std::string api = engineApi();
    auto& fs = frost::io::realFileSystem();
    frost::test::ScratchDir scratch("lua-engine-api");
    auto directory = scratch.join(".frost/lua-api");
    ASSERT_TRUE(fs.createDirectories(directory).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join(".frost/lua-api/frost.lua"), api).has_value());
    ASSERT_TRUE(
        fs.writeFileAtomic(
              scratch.join(".luarc.json"),
              R"({"runtime":{"version":"Lua 5.4"},"workspace":{"library":[".frost/lua-api"],"checkThirdParty":false}})")
            .has_value());
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    frost::lang::LuaServiceOptions options = serviceOptions(server, scratch);
    options.configuration = frost::lang::frostEngineLuaDefaults("Lua 5.4", [] {
        frost::lang::FrostEngineLuaProfile unreferenced;
        unreferenced.frostEngine = true; // A FrostEngine workspace whose reference was not found.
        return unreferenced;
    }());
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

// LUA-5 acceptance: hover, signature help, Find Usages (references), Structure (documentSymbol)
// and inlay hints through LuaService match direct LuaLS LSP queries (the oracle).
TEST(LuaLsE2E, CodeInsightMatchesDirectLuaLsResponses) {
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    frost::test::ScratchDir scratch("lua-insight-e2e");
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    frost::lang::LuaService service(frost::io::realProcessLauncher(), {**reactor, pool},
                                    serviceOptions(server, scratch));
    const std::string source = "---@param lhs number\n"
                               "---@param rhs number\n"
                               "---@return number\n"
                               "local function computeSum(lhs, rhs)\n"
                               "    return lhs + rhs\n"
                               "end\n"
                               "local first = computeSum(10, 20)\n"
                               "local second = computeSum(first, 30)\n"
                               "return second\n";
    auto document = frost::text::Document::fromUtf8(source);
    ASSERT_TRUE(document.has_value());
    const std::string file = scratch.join("insight.lua");
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(file, source).has_value());
    ASSERT_TRUE(service.openDocument(file, document->text()).has_value());
    ASSERT_TRUE(service.start().has_value());
    ASSERT_TRUE(waitFor([&] {
        return service.state() == frost::lang::LuaState::Running || service.state() == frost::lang::LuaState::Failed;
    }));
    ASSERT_EQ(service.state(), frost::lang::LuaState::Running);
    const std::string uri = frost::lang::pathToUri(file).value();

    // 1. Document symbols (Structure view): wait until LuaLS finishes parsing and returns symbols.
    std::vector<frost::lang::DocumentSymbol> symbols;
    ASSERT_TRUE(waitFor([&] {
        auto res =
            pool.spawn(frost::sched::Lane::UserVisible, frost::lang::requestDocumentSymbols(service, file)).join();
        if (res && !res->empty()) {
            symbols = std::move(*res);
            return true;
        }
        return false;
    }));
    auto rawSymbolsJson = pool.spawn(frost::sched::Lane::UserVisible,
                                     service.request("textDocument/documentSymbol",
                                                     frost::lang::Json::object().set(
                                                         "textDocument", frost::lang::Json::object().set("uri", uri))))
                              .join();
    ASSERT_TRUE(rawSymbolsJson.has_value()) << rawSymbolsJson.error().describe();
    const auto oracleSymbols = frost::lang::parseDocumentSymbols(*rawSymbolsJson);
    ASSERT_EQ(symbols.size(), oracleSymbols.size());
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        EXPECT_EQ(symbols[i].name, oracleSymbols[i].name);
        EXPECT_EQ(symbols[i].kind, oracleSymbols[i].kind);
        EXPECT_EQ(symbols[i].range, oracleSymbols[i].range);
    }
    EXPECT_TRUE(std::ranges::any_of(
        symbols, [](const frost::lang::DocumentSymbol& s) { return s.name.find("computeSum") != std::string::npos; }));

    // 2. Hover on `computeSum` (line 3, character 16, 0-based).
    const frost::lang::LspPosition fnPos{3, 16};
    auto hover = pool.spawn(frost::sched::Lane::UserVisible, frost::lang::requestHover(service, file, fnPos)).join();
    ASSERT_TRUE(hover.has_value()) << hover.error().describe();
    ASSERT_TRUE(hover->has_value());
    auto rawHoverJson =
        pool.spawn(frost::sched::Lane::UserVisible,
                   service.request(
                       "textDocument/hover",
                       frost::lang::Json::object()
                           .set("textDocument", frost::lang::Json::object().set("uri", uri))
                           .set("position",
                                frost::lang::Json::object().set("line", fnPos.line).set("character", fnPos.character))))
            .join();
    ASSERT_TRUE(rawHoverJson.has_value()) << rawHoverJson.error().describe();
    const auto oracleHover = frost::lang::parseHover(*rawHoverJson);
    ASSERT_TRUE(oracleHover.has_value());
    EXPECT_EQ((*hover)->contents, oracleHover->contents);
    EXPECT_EQ((*hover)->range, oracleHover->range);
    EXPECT_NE((*hover)->contents.text.find("computeSum"), std::string::npos);

    // 3. Signature help inside `computeSum(10, 20)` (line 6, character 26).
    const frost::lang::LspPosition sigPos{6, 26};
    auto sigHelp = pool.spawn(frost::sched::Lane::UserVisible,
                              frost::lang::requestSignatureHelp(service, file, sigPos, std::nullopt, false))
                       .join();
    ASSERT_TRUE(sigHelp.has_value()) << sigHelp.error().describe();
    ASSERT_TRUE(sigHelp->has_value());
    ASSERT_FALSE((*sigHelp)->signatures.empty());
    auto rawSigJson =
        pool.spawn(
                frost::sched::Lane::UserVisible,
                service.request(
                    "textDocument/signatureHelp",
                    frost::lang::Json::object()
                        .set("textDocument", frost::lang::Json::object().set("uri", uri))
                        .set("position",
                             frost::lang::Json::object().set("line", sigPos.line).set("character", sigPos.character))))
            .join();
    ASSERT_TRUE(rawSigJson.has_value()) << rawSigJson.error().describe();
    const auto oracleSig = frost::lang::parseSignatureHelp(*rawSigJson);
    ASSERT_TRUE(oracleSig.has_value());
    ASSERT_EQ((*sigHelp)->signatures.size(), oracleSig->signatures.size());
    EXPECT_EQ((*sigHelp)->signatures[0].label, oracleSig->signatures[0].label);
    EXPECT_EQ((*sigHelp)->activeParameter, oracleSig->activeParameter);
    EXPECT_NE((*sigHelp)->signatures[0].label.find("lhs"), std::string::npos);

    // 4. Find Usages (`textDocument/references`) on `computeSum` (declaration + two call sites = 3).
    auto refs =
        pool.spawn(frost::sched::Lane::UserVisible, frost::lang::requestReferences(service, file, fnPos, true)).join();
    ASSERT_TRUE(refs.has_value()) << refs.error().describe();
    auto rawRefsJson =
        pool.spawn(frost::sched::Lane::UserVisible,
                   service.request(
                       "textDocument/references",
                       frost::lang::Json::object()
                           .set("textDocument", frost::lang::Json::object().set("uri", uri))
                           .set("position",
                                frost::lang::Json::object().set("line", fnPos.line).set("character", fnPos.character))
                           .set("context", frost::lang::Json::object().set("includeDeclaration", true))))
            .join();
    ASSERT_TRUE(rawRefsJson.has_value()) << rawRefsJson.error().describe();
    const auto oracleRefs = frost::lang::parseLocations(*rawRefsJson);
    EXPECT_EQ(*refs, oracleRefs);
    EXPECT_EQ(refs->size(), 3U);

    // 5. Inlay hints (`textDocument/inlayHint`) across the full document range.
    const frost::lang::LspRange fullRange{{0, 0}, {9, 0}};
    std::vector<frost::lang::LspInlayHint> hints;
    ASSERT_TRUE(waitFor([&] {
        auto res = pool.spawn(frost::sched::Lane::UserVisible, frost::lang::requestInlayHints(service, file, fullRange))
                       .join();
        if (res && !res->empty()) {
            hints = std::move(*res);
            return true;
        }
        return false;
    }));
    auto rawHintsJson =
        pool.spawn(
                frost::sched::Lane::UserVisible,
                service.request(
                    "textDocument/inlayHint",
                    frost::lang::Json::object()
                        .set("textDocument", frost::lang::Json::object().set("uri", uri))
                        .set("range", frost::lang::Json::object()
                                          .set("start", frost::lang::Json::object().set("line", 0).set("character", 0))
                                          .set("end", frost::lang::Json::object().set("line", 9).set("character", 0)))))
            .join();
    ASSERT_TRUE(rawHintsJson.has_value()) << rawHintsJson.error().describe();
    const auto oracleHints = frost::lang::parseInlayHints(*rawHintsJson);
    EXPECT_EQ(hints, oracleHints);

    pool.spawn(frost::sched::Lane::UserVisible, service.stop()).join();
    (*reactor)->shutdown();
    pool.shutdown();
}

// LUA-6 acceptance: inspectLuaDirectory over a scripts folder produces results equal to running
// `lua-language-server --check` (the oracle) on that folder, and reports NotFound when LuaLS is missing.
TEST(LuaLsE2E, InspectDirectoryMatchesDirectCheckOracle) {
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    auto& fs = frost::io::realFileSystem();
    frost::test::ScratchDir scratch("lua-inspect-e2e");
    const std::string scriptsDir = scratch.join("scripts");
    ASSERT_TRUE(fs.createDirectories(scriptsDir).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("scripts/alpha.lua"), "local ok = 1\nprint(ok + unknownAlphaGlobal)\n")
                    .has_value());
    ASSERT_TRUE(
        fs.writeFileAtomic(scratch.join("scripts/beta.lua"), "local unusedBeta = 42\nprint(unknownBetaGlobal)\n")
            .has_value());
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("scripts/clean.lua"), "return 1 + 2\n").has_value());

    frost::lang::LuaInspectOptions missingOpts;
    missingOpts.directory = scriptsDir;
    missingOpts.stateDirectory = scratch.join("missing-state");
    missingOpts.search = frost::lang::LuaSearch{};
    missingOpts.trusted = true;
    const auto missingResult = frost::lang::inspectLuaDirectory(frost::io::realProcessLauncher(), fs, missingOpts);
    ASSERT_FALSE(missingResult.has_value());
    EXPECT_EQ(missingResult.error().code(), frost::ErrorCode::NotFound);

    frost::lang::LuaInspectOptions options;
    options.directory = scriptsDir;
    options.stateDirectory = scratch.join("inspect-state");
    options.search.configuredPath = server;
    options.minimumSeverity = frost::lang::DiagnosticSeverity::Hint;
    options.trusted = true;
    const auto inspected = frost::lang::inspectLuaDirectory(frost::io::realProcessLauncher(), fs, options);
    ASSERT_TRUE(inspected.has_value()) << inspected.error().describe();
    EXPECT_GE(inspected->size(), 2U);

    // Direct oracle: invoke `lua-language-server --check` on the same folder and parse its JSON output.
    const std::string oracleOut = scratch.join("oracle-check.json");
    auto oracleArgs = frost::lang::luaLanguageServerArguments(scratch.join("oracle-state"));
    ASSERT_TRUE(oracleArgs.has_value());
    frost::io::ProcessSpec spec;
    spec.executable = server;
    spec.workingDirectory = scriptsDir;
    spec.arguments = std::move(*oracleArgs);
    spec.arguments.push_back("--check=" + scriptsDir);
    spec.arguments.emplace_back("--checklevel=Hint");
    spec.arguments.emplace_back("--check_format=json");
    spec.arguments.push_back("--check_out_path=" + oracleOut);
    spec.stdinMode = frost::io::Stdio::Null;
    spec.stdoutMode = frost::io::Stdio::Pipe;
    spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
    spec.killOnClose = true;
    const auto oracleRun = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 180s);
    ASSERT_TRUE(oracleRun.has_value()) << oracleRun.error().describe();
    const auto oracleText = fs.readFile(oracleOut);
    ASSERT_TRUE(oracleText.has_value()) << oracleText.error().describe();
    const auto oracleFiles = frost::lang::parseLuaCheckReport(*oracleText, &fs);
    ASSERT_TRUE(oracleFiles.has_value()) << oracleFiles.error().describe();
    ASSERT_EQ(inspected->size(), oracleFiles->size());
    for (std::size_t i = 0; i < inspected->size(); ++i) {
        EXPECT_EQ(frost::lang::documentKey((*inspected)[i].file), frost::lang::documentKey((*oracleFiles)[i].file));
        EXPECT_EQ((*inspected)[i].diagnostics, (*oracleFiles)[i].diagnostics);
    }
}

// LUA-7 acceptance: semantic tokens, document formatting and range formatting through LuaService
// match LuaLS's own responses (the oracle).
TEST(LuaLsE2E, SemanticTokensAndFormattingMatchDirectLuaLsResponses) {
    const std::string server = pinnedLuaLs();
    ASSERT_FALSE(server.empty());
    frost::test::ScratchDir scratch("lua-tokens-format-e2e");
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    frost::lang::LuaService service(frost::io::realProcessLauncher(), {**reactor, pool},
                                    serviceOptions(server, scratch));
    const std::string unformatted = "local function multiply(x,y)\nreturn x*y\nend\nlocal z=multiply(3,4)\nreturn z\n";
    auto document = frost::text::Document::fromUtf8(unformatted);
    ASSERT_TRUE(document.has_value());
    const std::string file = scratch.join("format.lua");
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(file, unformatted).has_value());
    ASSERT_TRUE(service.openDocument(file, document->text()).has_value());
    ASSERT_TRUE(service.start().has_value());
    ASSERT_TRUE(waitFor([&] {
        return service.state() == frost::lang::LuaState::Running || service.state() == frost::lang::LuaState::Failed;
    }));
    ASSERT_EQ(service.state(), frost::lang::LuaState::Running);
    const std::string uri = frost::lang::pathToUri(file).value();
    const auto capabilities = service.capabilities();
    ASSERT_TRUE(capabilities.has_value());
    const auto tokenSupport = frost::lang::semanticTokensSupport(*capabilities);
    ASSERT_TRUE(tokenSupport.has_value());
    EXPECT_TRUE(tokenSupport->full);

    // 1. Semantic tokens full.
    frost::lang::SemanticTokensData tokensData;
    ASSERT_TRUE(waitFor([&] {
        auto res = pool.spawn(frost::sched::Lane::UserVisible,
                              frost::lang::requestSemanticTokens(service, file, std::nullopt, false))
                       .join();
        if (res && !res->data.empty()) {
            tokensData = std::move(*res);
            return true;
        }
        return false;
    }));
    auto rawTokensJson = pool.spawn(frost::sched::Lane::UserVisible,
                                    service.request("textDocument/semanticTokens/full",
                                                    frost::lang::Json::object().set(
                                                        "textDocument", frost::lang::Json::object().set("uri", uri))))
                             .join();
    ASSERT_TRUE(rawTokensJson.has_value()) << rawTokensJson.error().describe();
    auto oracleTokensData = frost::lang::parseSemanticTokensResult(*rawTokensJson);
    ASSERT_TRUE(oracleTokensData.has_value()) << oracleTokensData.error().describe();
    ASSERT_TRUE(oracleTokensData->data.has_value());
    EXPECT_EQ(tokensData.data, *oracleTokensData->data);
    const auto decodedSpans =
        frost::lang::decodeSemanticTokens(tokensData.data, tokenSupport->legend, document->text(), service.encoding());
    const auto oracleSpans = frost::lang::decodeSemanticTokens(*oracleTokensData->data, tokenSupport->legend,
                                                               document->text(), service.encoding());
    ASSERT_TRUE(decodedSpans.has_value()) << decodedSpans.error().describe();
    ASSERT_TRUE(oracleSpans.has_value()) << oracleSpans.error().describe();
    EXPECT_FALSE(decodedSpans->empty());
    EXPECT_EQ(*decodedSpans, *oracleSpans);

    // 2. Range formatting on the function body (lines 0..2).
    const frost::lang::FormattingOptions fmtOpts{.tabSize = 2, .insertSpaces = true};
    const frost::lang::LspRange fnRange{{0, 0}, {3, 0}};
    auto rangeEdits =
        pool.spawn(frost::sched::Lane::UserVisible, frost::lang::requestFormatting(service, file, fnRange, fmtOpts))
            .join();
    ASSERT_TRUE(rangeEdits.has_value()) << rangeEdits.error().describe();
    auto rawRangeJson =
        pool.spawn(
                frost::sched::Lane::UserVisible,
                service.request(
                    "textDocument/rangeFormatting",
                    frost::lang::Json::object()
                        .set("textDocument", frost::lang::Json::object().set("uri", uri))
                        .set("range", frost::lang::Json::object()
                                          .set("start", frost::lang::Json::object().set("line", 0).set("character", 0))
                                          .set("end", frost::lang::Json::object().set("line", 3).set("character", 0)))
                        .set("options", frost::lang::Json::object().set("tabSize", 2).set("insertSpaces", true))))
            .join();
    ASSERT_TRUE(rawRangeJson.has_value()) << rawRangeJson.error().describe();
    const auto oracleRangeEdits = frost::lang::parseTextEdits(*rawRangeJson);
    EXPECT_EQ(*rangeEdits, oracleRangeEdits);

    // 3. Full document formatting.
    auto docEdits = pool.spawn(frost::sched::Lane::UserVisible,
                               frost::lang::requestFormatting(service, file, std::nullopt, fmtOpts))
                        .join();
    ASSERT_TRUE(docEdits.has_value()) << docEdits.error().describe();
    ASSERT_FALSE(docEdits->empty());
    auto rawDocJson =
        pool.spawn(frost::sched::Lane::UserVisible,
                   service.request(
                       "textDocument/formatting",
                       frost::lang::Json::object()
                           .set("textDocument", frost::lang::Json::object().set("uri", uri))
                           .set("options", frost::lang::Json::object().set("tabSize", 2).set("insertSpaces", true))))
            .join();
    ASSERT_TRUE(rawDocJson.has_value()) << rawDocJson.error().describe();
    const auto oracleDocEdits = frost::lang::parseTextEdits(*rawDocJson);
    EXPECT_EQ(*docEdits, oracleDocEdits);

    const auto byteEdits = frost::lang::toByteEdits(document->text(), *docEdits, service.encoding());
    const auto oracleByteEdits = frost::lang::toByteEdits(document->text(), oracleDocEdits, service.encoding());
    ASSERT_TRUE(byteEdits.has_value());
    ASSERT_TRUE(oracleByteEdits.has_value());
    EXPECT_EQ(*byteEdits, *oracleByteEdits);
    const auto formattedText = frost::lang::applyTextEdits(unformatted, *docEdits, service.encoding());
    const auto oracleFormattedText = frost::lang::applyTextEdits(unformatted, oracleDocEdits, service.encoding());
    ASSERT_TRUE(formattedText.has_value());
    ASSERT_TRUE(oracleFormattedText.has_value());
    EXPECT_EQ(*formattedText, *oracleFormattedText);

    pool.spawn(frost::sched::Lane::UserVisible, service.stop()).join();
    (*reactor)->shutdown();
    pool.shutdown();
}
} // namespace
