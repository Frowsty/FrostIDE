// SPDX-License-Identifier: Apache-2.0
// Authored LTR-17 registry governance, pin changes and supervised generic LSP wire regressions.
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/language_registry.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/text/document.hpp"

#include <gtest/gtest.h>
namespace frost::lang {
namespace {
using namespace std::chrono_literals;
std::string root() {
#ifdef _WIN32
    return "C:/registry";
#else
    return "/registry";
#endif
}
Json definition() {
    return Json::object()
        .set("id", "example")
        .set("name", "Local example")
        .set("languageId", "python")
        .set("version", "declared-1")
        .set("enabled", true)
        .set("extensions", Json::array({".py"}))
        .set("executable", root() + "/server")
        .set("arguments", Json::array({"--stdio"}))
        .set("license", "MIT")
        .set("licensePath", root() + "/LICENSE")
        .set("licenseSha256", io::sha256Hex("license evidence"))
        .set("artifacts",
             Json::array({Json::object().set("path", root() + "/server").set("sha256", io::sha256Hex("binary"))}))
        .set("configuration", Json::object().set("python", Json::object().set("analysis", true)))
        .set("initializationOptions", Json::object().set("exampleOption", true));
}
std::string manifest(Json d) {
    return Json::object().set("version", 1).set("servers", Json::array({std::move(d)})).dump();
}
std::string frame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
TEST(LanguageRegistry, LuauIsAnExplicitIndependentLanguageAndNeverOverridesLuaJitLuaFiles) {
    auto row = definition();
    row.set("id", "luau")
        .set("languageId", "luau")
        .set("version", "1.70.1")
        .set("extensions", Json::array({".luau"}))
        .set("arguments", Json::array({"lsp"}))
        .set("configuration",
             Json::object().set("luau-lsp", Json::object().set("platform", Json::object().set("type", "standard"))));
    auto registry = LanguageServerRegistry::parse(manifest(row));
    ASSERT_TRUE(registry);
    auto* selected = registry->forPath(root() + "/typed.luau");
    ASSERT_NE(selected, nullptr);
    auto options = registeredLanguageOptions(*selected, root() + "/workspace");
    EXPECT_EQ(options.languageId, "luau");
    EXPECT_EQ(options.arguments, (std::vector<std::string>{"lsp"}));
    EXPECT_TRUE(options.configurationIsSectionMap);
    EXPECT_EQ(registry->forPath(root() + "/jit.lua"), nullptr);
    row.set("extensions", Json::array({".lua"}));
    EXPECT_FALSE(LanguageServerRegistry::parse(manifest(row)));
}
TEST(LanguageRegistry, ParsesExplicitSelectorsAndSeparatesDeclaredVersionFromObservedServer) {
    auto parsed = LanguageServerRegistry::parse(manifest(definition()));
    ASSERT_TRUE(parsed);
    auto* selected = parsed->forPath(root() + "/workspace/main.PY");
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->version, "declared-1");
    EXPECT_EQ(selected->languageId, "python");
    EXPECT_EQ(parsed->forPath(root() + "/file.cpp"), nullptr);
    auto options = registeredLanguageOptions(*selected, root() + "/workspace");
    EXPECT_TRUE(options.configurationIsSectionMap);
    EXPECT_TRUE(options.initializationOptions.boolAt("exampleOption").value_or(false));
}
TEST(LanguageRegistry, RejectsUnsupportedLicenseUnpinnedExecutableDuplicateSelectorsAndBuiltinOverride) {
    auto d = definition();
    d.set("license", "GPL-3.0-only");
    EXPECT_FALSE(LanguageServerRegistry::parse(manifest(d)));
    d = definition();
    d.set("artifacts", Json::array());
    EXPECT_FALSE(LanguageServerRegistry::parse(manifest(d)));
    d = definition();
    d.set("extensions", Json::array({".cpp"}));
    EXPECT_FALSE(LanguageServerRegistry::parse(manifest(d)));
    d = definition();
    auto other = d;
    other.set("id", "other");
    EXPECT_FALSE(
        LanguageServerRegistry::parse(Json::object().set("version", 1).set("servers", Json::array({d, other})).dump()));
    EXPECT_FALSE(LanguageServerRegistry::parse(R"({"version":1.0,"servers":[]})"));
}
TEST(LanguageRegistry, LaunchRefusesChangedPinsRevokedThenRegrantedTrustAndUnreviewedArguments) {
    io::MemFs fs(systemClock());
    ASSERT_TRUE(fs.createDirectories(root() + "/workspace"));
    ASSERT_TRUE(fs.writeFileAtomic(root() + "/server", "binary"));
    ASSERT_TRUE(fs.writeFileAtomic(root() + "/LICENSE", "license evidence"));
    auto trust = project::TrustStore::open(fs, root() + "/trust.txt");
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(root() + "/workspace"));
    auto registry = LanguageServerRegistry::parse(manifest(definition()));
    ASSERT_TRUE(registry);
    auto d = registry->definitions().front();
    auto revision = (*trust)->snapshot(root() + "/workspace").revision;
    auto physical = fs.physicalPath(root() + "/workspace");
    ASSERT_TRUE(physical);
    io::ScriptedProcessLauncher actual;
    RegistryProcessLauncher guarded(actual, fs, **trust, d, root() + "/workspace", revision, *physical);
    io::ProcessSpec spec;
    spec.executable = d.executable;
    spec.arguments = d.arguments;
    spec.workingDirectory = root() + "/workspace";
    auto changed = spec;
    changed.arguments.emplace_back("--foreign");
    EXPECT_FALSE(guarded.launch(changed));
    EXPECT_TRUE(actual.launches().empty());
    ASSERT_TRUE(fs.writeFileAtomic(root() + "/server", "changed"));
    EXPECT_FALSE(guarded.launch(spec));
    EXPECT_TRUE(actual.launches().empty());
    ASSERT_TRUE(fs.writeFileAtomic(root() + "/server", "binary"));
    actual.expect(io::ScriptedProcess{});
    auto launched = guarded.launch(spec);
    ASSERT_TRUE(launched);
    EXPECT_EQ(actual.launches().size(), 1);
    ASSERT_TRUE((*trust)->untrust(root() + "/workspace"));
    ASSERT_TRUE((*trust)->trust(root() + "/workspace"));
    EXPECT_FALSE(guarded.launch(spec));
    EXPECT_EQ(actual.launches().size(), 1);
}
TEST(LanguageRegistry, RemovedContributionRevokesRetainedLaunchAuthorityWithoutTrustChange) {
    io::MemFs fs(systemClock());
    ASSERT_TRUE(fs.createDirectories(root() + "/workspace"));
    ASSERT_TRUE(fs.writeFileAtomic(root() + "/workspace/local.py", "value=1"));
    ASSERT_TRUE(fs.writeFileAtomic(root() + "/server", "binary"));
    ASSERT_TRUE(fs.writeFileAtomic(root() + "/LICENSE", "license evidence"));
    auto trust = project::TrustStore::open(fs, root() + "/trust.txt");
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(root() + "/workspace"));
    auto registry = LanguageServerRegistry::parse(manifest(definition()));
    ASSERT_TRUE(registry);
    auto physical = fs.physicalPath(root() + "/workspace");
    ASSERT_TRUE(physical);
    io::ScriptedProcessLauncher actual;
    RegistryProcessLauncher guarded(actual, fs, **trust, registry->definitions().front(), root() + "/workspace",
                                    (*trust)->snapshot(root() + "/workspace").revision, *physical);
    ASSERT_TRUE(guarded.admitDocument(root() + "/workspace/local.py"));
    guarded.revoke();
    EXPECT_FALSE(guarded.admitDocument(root() + "/workspace/local.py"));
    io::ProcessSpec spec;
    spec.executable = root() + "/server";
    spec.arguments = {"--stdio"};
    spec.workingDirectory = root() + "/workspace";
    EXPECT_FALSE(guarded.launch(spec));
    EXPECT_TRUE(actual.launches().empty());
}
TEST(LanguageRegistry, RequestBoundaryRejectsExternalDocumentsAndRevokedDelayedTasksBeforeServerAccess) {
    io::MemFs fs(systemClock());
    ASSERT_TRUE(fs.createDirectories(root() + "/workspace"));
    ASSERT_TRUE(fs.writeFileAtomic(root() + "/external.py", "external"));
    ASSERT_TRUE(fs.writeFileAtomic(root() + "/workspace/local.py", "local"));
    auto trust = project::TrustStore::open(fs, root() + "/trust.txt");
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(root() + "/workspace"));
    auto registry = LanguageServerRegistry::parse(manifest(definition()));
    ASSERT_TRUE(registry);
    auto reactor = io::Reactor::create();
    ASSERT_TRUE(reactor);
    sched::SimExecutor sim;
    io::ScriptedProcessLauncher actual;
    auto physical = fs.physicalPath(root() + "/workspace");
    ASSERT_TRUE(physical);
    auto admission = std::make_shared<RegistryProcessLauncher>(
        actual, fs, **trust, registry->definitions().front(), root() + "/workspace",
        (*trust)->snapshot(root() + "/workspace").revision, *physical);
    auto service =
        std::make_shared<LuaService>(*admission, io::AsyncContext{**reactor, sim},
                                     registeredLanguageOptions(registry->definitions().front(), root() + "/workspace"));
    RegistryLspRequester requester(service, admission);
    const auto externalEdit = Json::object().set(
        "changes",
        Json::object().set(
            pathToUri(root() + "/external.py").value(),
            Json::array(
                {Json::object()
                     .set("range", Json::object().set("start", toJson(LspPosition{})).set("end", toJson(LspPosition{})))
                     .set("newText", "foreign")})));
    EXPECT_FALSE(admission->admitEdit(externalEdit));
    const auto resource = Json::object().set(
        "documentChanges",
        Json::array(
            {Json::object().set("kind", "delete").set("uri", pathToUri(root() + "/workspace/local.py").value())}));
    EXPECT_FALSE(admission->admitEdit(resource));
    auto params = [](std::string_view path) {
        return Json::object().set("textDocument", Json::object().set("uri", pathToUri(path).value()));
    };
    auto denied = sched::syncWait(requester.request("textDocument/hover", params(root() + "/external.py")));
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code(), ErrorCode::PermissionDenied);
    auto delayed = requester.request("textDocument/hover", params(root() + "/workspace/local.py"));
    ASSERT_TRUE((*trust)->untrust(root() + "/workspace"));
    denied = sched::syncWait(std::move(delayed));
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code(), ErrorCode::Untrusted);
    EXPECT_TRUE(actual.launches().empty());
}
TEST(LanguageRegistry, GenericSupervisionSendsOwnLanguageAndInitializationOptionsWithoutLuaDefaults) {
    auto reactor = io::Reactor::create();
    ASSERT_TRUE(reactor);
    sched::SimExecutor sim;
    io::ScriptedProcessLauncher launcher;
    auto registry = LanguageServerRegistry::parse(manifest(definition()));
    ASSERT_TRUE(registry);
    auto options = registeredLanguageOptions(registry->definitions().front(), root() + "/workspace");
    launcher.expect(io::ScriptedProcess{
        .stdoutBytes = frame(
            R"({"jsonrpc":"2.0","id":1,"result":{"capabilities":{"textDocumentSync":2,"hoverProvider":true},"serverInfo":{"name":"actual-peer","version":"wire-2"}}})"),
        .neverExits = true,
        .chunkBytes = 1024,
        .chunkInterval = 1ms});
    LuaService server(launcher, {**reactor, sim}, options);
    auto doc = text::Document::fromUtf8("value = 1\n");
    ASSERT_TRUE(doc);
    ASSERT_TRUE(server.openDocument(root() + "/workspace/main.py", doc->text()));
    ASSERT_TRUE(server.start());
    sim.runFor(5ms);
    ASSERT_EQ(server.state(), LuaState::Running);
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1);
    EXPECT_NE(launches.front().stdinBytes.find("\"languageId\":\"python\""), std::string::npos);
    EXPECT_NE(launches.front().stdinBytes.find("\"exampleOption\":true"), std::string::npos);
    EXPECT_EQ(server.capabilities()->serverVersion, "wire-2");
    auto stopped = sim.spawn(sched::Lane::Background, server.stop());
    sim.runFor(10s);
    stopped.join();
}
} // namespace
} // namespace frost::lang
