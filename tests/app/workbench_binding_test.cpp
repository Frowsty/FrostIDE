// SPDX-License-Identifier: Apache-2.0
// FEC-13 no-launch trust/provider boundaries and generic generated-resource write refusal.
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/lua_host.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/thread_pool.hpp"

#include "../../src/app/src/engine_binding_integration.hpp"
#include "../../src/app/src/engine_binding_work.hpp"
#include "../../src/app/src/language_bridge.hpp"

#include <condition_variable>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "workbench_rig.hpp"
#include "support/directory_link.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/text/text_buffer.hpp"

#include <algorithm>
#include <array>

namespace frost::app::test {
namespace {

TEST(WorkbenchBindings, UntrustedQueriesRefuseWithZeroToolLaunchesAndNoInventedPreview) {
    WorkDir dir("binding-untrusted");
    dir.write("project/main.lua", "print('unchanged')\n");
    io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    const auto before = launcher.launches().size();
    auto refresh = rig.workbench->refreshEngineBindings();
    auto usages = rig.workbench->findEngineBindingUsages();
    auto rename = rig.workbench->previewEngineBindingRename("Renamed");
    auto apply = rig.workbench->applyEngineBindingRename();
    ASSERT_FALSE(apply);
    EXPECT_EQ(apply.error().code(), ErrorCode::Busy);
    ASSERT_FALSE(refresh);
    ASSERT_FALSE(usages);
    ASSERT_FALSE(rename);
    EXPECT_EQ(refresh.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(usages.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(rename.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(launcher.launches().size(), before);
    const auto state = rig.workbench->engineBindingState();
    EXPECT_EQ(state.boolAt("available"), false);
    EXPECT_EQ(state.boolAt("previewReady"), false);
    EXPECT_TRUE(state.find("usages") && state.find("usages")->items().empty());
    EXPECT_EQ(io::realFileSystem().readFile(dir.join("project/main.lua")).value(), "print('unchanged')\n");
}

TEST(WorkbenchBindings, MissingProvidersDoNotManufactureCombinedResultsOrStartAnAlternateTool) {
    WorkDir dir("binding-no-providers");
    dir.write("project/main.cpp", "void f() {}\n");
    io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    const auto before = launcher.launches().size();
    auto result = rig.workbench->refreshEngineBindings();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(launcher.launches().size(), before);
    EXPECT_EQ(rig.workbench->engineBindingState().boolAt("busy"), false);
    EXPECT_EQ(rig.workbench->engineBindingState().boolAt("available"), false);
    rig.workbench->shutdown();
    EXPECT_FALSE(rig.workbench->previewEngineBindingRename("AfterShutdown"));
    EXPECT_EQ(launcher.launches().size(), before);
}

TEST(WorkbenchBindings, GenericWorkspaceEditCannotWriteOwnedEngineGeneratedMetadata) {
    WorkDir dir("binding-cache-write-refusal");
    dir.write("project/main.lua", "local name = 1\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    const auto directory = lang::luaHostMetaDirectory(dir.join("data/luals"), "FrostEngine", dir.join("project"));
    ASSERT_TRUE(directory);
    ASSERT_TRUE(io::realFileSystem().createDirectories(*directory));
    const auto path = io::joinPath(*directory, "frost_schema.lua").value();
    const std::string original = "function Cast() end\n";
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(path, original));
    lang::WorkspaceEdit edit;
    edit.files.push_back({path, lang::pathToUri(path).value(), {}, {{{{0, 9}, {0, 13}}, "Trace"}}});
    LanguageBridge bridge(*rig.workbench);
    auto applied = bridge.apply(edit, "Generic rename");
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(io::realFileSystem().readFile(path).value(), original);
    EXPECT_EQ(io::realFileSystem().readFile(dir.join("project/main.lua")).value(), "local name = 1\n");
}
TEST(WorkbenchBindings, PhysicalAliasOutsideAdmittedRootsIsReadOnly) {
    WorkDir dir("binding-alias-role");
    dir.write("project/main.lua", "return 1\n");
    dir.write("outside/export.lua", "return 2\n");
    const auto link = dir.join("project/escaped");
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH> system{};
    const auto length = GetSystemDirectoryW(system.data(), MAX_PATH);
    ASSERT_GT(length, 0u);
    io::ProcessSpec spec;
    spec.executable = io::fromWide(std::wstring_view(system.data(), length)).value() + "\\cmd.exe";
    spec.arguments = {"/c", "mklink", "/J", link, dir.join("outside")};
    auto made = io::runToCompletion(io::realProcessLauncher(), spec, {}, std::chrono::seconds(30));
    ASSERT_TRUE(made);
    ASSERT_EQ(made->exitCode, 0) << made->stdoutBytes << made->stderrBytes;
#else
    ASSERT_EQ(symlink(dir.join("outside").c_str(), link.c_str()), 0);
#endif
    auto resolved = binding_work::resolveDocumentPath(io::realFileSystem(), dir.join("project"), "", "",
                                                      io::joinPath(link, "export.lua").value());
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->role, engine::BindingDocumentRole::ExternalReadOnly);
    EXPECT_EQ(lang::documentKey(resolved->physical),
              lang::documentKey(io::realFileSystem().canonicalPath(dir.join("outside/export.lua")).value()));
    auto admitted = binding_work::resolveDocumentPath(io::realFileSystem(), dir.join("project"), "", "",
                                                      dir.join("project/main.lua"));
    ASSERT_TRUE(admitted);
    EXPECT_EQ(admitted->role, engine::BindingDocumentRole::UserSource);
    ASSERT_TRUE(io::realFileSystem().removeFile(link));
}

TEST(WorkbenchBindings, CaptureReadBoundsAreEnforcedBeforeOpeningOrAllocatingOversizeInput) {
    WorkDir dir("binding-read-bounds");
    dir.write("large.lua", std::string(16U * 1024U * 1024U + 1, 'x'));
    auto oversized = binding_work::readDocumentSnapshot(io::realFileSystem(), dir.join("large.lua"), 0);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code(), ErrorCode::OutOfRange);
    // Nonexistent path proves exhausted aggregate budget refuses before filesystem lookup.
    auto exhausted =
        binding_work::readDocumentSnapshot(io::realFileSystem(), dir.join("missing.lua"), 256U * 1024U * 1024U);
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error().code(), ErrorCode::OutOfRange);
    dir.write("small.lua", "12345");
    auto remaining =
        binding_work::readDocumentSnapshot(io::realFileSystem(), dir.join("small.lua"), 256U * 1024U * 1024U - 4);
    ASSERT_FALSE(remaining);
    EXPECT_EQ(remaining.error().code(), ErrorCode::OutOfRange);
}

struct BindingWorkerGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false, release = false;
    ~BindingWorkerGate() { unblock(); }
    void unblock() {
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        condition.notify_all();
    }
};
sched::Task<void> blockedBindingPost(std::shared_ptr<BindingWorkerGate> gate,
                                     std::shared_ptr<EngineBindingPostRelay> relay,
                                     std::shared_ptr<std::atomic<unsigned>> delivered) {
    {
        std::unique_lock lock(gate->mutex);
        gate->started = true;
        gate->condition.notify_all();
        gate->condition.wait(lock, [&] { return gate->release; });
    }
    relay->post([delivered](EngineBindingIntegration&) { ++*delivered; });
    co_return;
}
TEST(WorkbenchBindings, RevokedRelayAndRetirementKeepBlockedWorkOffInteractiveTeardown) {
    WorkDir dir("binding-retirement");
    WorkbenchRig rig(dir);
    sched::ThreadPool pool({.workerCount = 2});
    auto retirement = std::make_shared<EngineBindingRetirement>();
    auto relay = std::make_shared<EngineBindingPostRelay>();
    auto feature = std::make_unique<EngineBindingIntegration>(*rig.workbench, retirement, relay);
    auto delivered = std::make_shared<std::atomic<unsigned>>(0);
    auto gate = std::make_shared<BindingWorkerGate>();
    // Ensure early assertion unwinding never leaves the test worker blocked.
    struct Release {
        std::shared_ptr<BindingWorkerGate> gate;
        ~Release() { gate->unblock(); }
    } release{gate};
    retirement->jobs.push_back(pool.spawn(sched::Lane::Background, blockedBindingPost(gate, relay, delivered)));
    {
        std::unique_lock lock(gate->mutex);
        ASSERT_TRUE(gate->condition.wait_for(lock, std::chrono::seconds(5), [&] { return gate->started; }));
    }
    EXPECT_FALSE(retirement->ready());
    feature->shutdown(); // Revokes the feature's actual posting relay before withdrawing work.
    feature.reset();
    EXPECT_FALSE(retirement->ready()); // Destruction did not join the blocked worker.
    gate->unblock();
    retirement->join(); // Explicit final owner drain, after feature destruction.
    QApplication::processEvents();
    EXPECT_TRUE(retirement->ready());
    EXPECT_EQ(delivered->load(), 0u);
}

TEST(WorkbenchBindings, PhysicalAdmissionKeepsAliasWorkspaceWritableButExpectedMetadataFailureRefuses) {
    WorkDir dir("binding-physical-alias-admission");
    dir.write("project/main.lua", "return 1\n");
    dir.write("project/generated.lua", "function Cast() end\n");
    const auto alias = dir.join("workspace");
    ASSERT_NO_FATAL_FAILURE(frost::test::makeDirectoryLink(alias, dir.join("project")));
    auto& fs = io::realFileSystem();
    const auto document = io::joinPath(alias, "main.lua").value();
    const auto metadata = io::joinPath(alias, "generated.lua").value();
    auto admitted = binding_work::resolveDocumentPath(fs, alias, "", metadata, document);
    ASSERT_TRUE(admitted);
    EXPECT_EQ(admitted->role, engine::BindingDocumentRole::UserSource);
    EXPECT_EQ(lang::documentKey(admitted->physical), lang::documentKey(fs.physicalPath(dir.join("project/main.lua")).value()));
    ASSERT_TRUE(fs.removeFile(dir.join("project/generated.lua")));
    EXPECT_FALSE(binding_work::resolveDocumentPath(fs, alias, "", metadata, document));
}

TEST(WorkbenchBindings, SemanticRenameNeedsExactGeneratedIdentityAndCannotFallbackOnResolutionFailure) {
    WorkDir dir("binding-rename-physical-admission");
    dir.write("project/main.lua", "local Cast = 1\n");
    dir.write("other/FrostEngine-impostor/frost_schema.lua", "function Cast() end\n");
    const auto metadata = dir.join("host-meta/FrostEngine-real/frost_schema.lua");
    auto& fs = io::realFileSystem();
    const auto definitions = [](const std::string& file) {
        return rpc::Json::array().push(rpc::Json::object().set("uri", lang::pathToUri(file).value())
            .set("range", lang::toJson(lang::LspRange{{0, 0}, {0, 1}})));
    };
    // No engine metadata is expected in an ordinary Lua workspace; actual nonbinding definition succeeds.
    auto ordinary = binding_work::admitRenameDefinitions(fs, definitions(dir.join("project/main.lua")), metadata);
    ASSERT_TRUE(ordinary);
    EXPECT_FALSE(ordinary->bound);
    dir.write("host-meta/FrostEngine-real/frost_schema.lua", "function Cast() end\n");
    const auto physical = fs.physicalPath(metadata).value();
    auto bound = binding_work::admitRenameDefinitions(fs, definitions(metadata), metadata, physical);
    ASSERT_TRUE(bound);
    EXPECT_TRUE(bound->bound);
    auto impostor = binding_work::admitRenameDefinitions(fs, definitions(dir.join("other/FrostEngine-impostor/frost_schema.lua")), metadata, physical);
    ASSERT_TRUE(impostor);
    EXPECT_FALSE(impostor->bound); // Directory/file names are insufficient evidence.
    EXPECT_FALSE(binding_work::admitRenameDefinitions(fs, definitions(dir.join("project/missing.lua")), metadata, physical));
    ASSERT_TRUE(fs.removeFile(metadata));
    EXPECT_FALSE(binding_work::admitRenameDefinitions(fs, definitions(dir.join("project/main.lua")), metadata, physical));
    EXPECT_FALSE(binding_work::admitRenameDefinitions(fs, rpc::Json::array(), metadata));
}

TEST(WorkbenchBindings, OriginalCdbInputAndMetadataAliasReceiptsDetectRetargetEvenWithIdenticalBytes) {
    WorkDir dir("binding-receipt-retarget");
    auto& fs = io::realFileSystem();
    for (const std::string name : {"compile_commands.json", "input.cpp", "frost_schema.lua"}) {
        dir.write("first/" + name, "identical bytes");
        dir.write("second/" + name, "identical bytes");
    }
    const auto alias = dir.join("analysis");
    ASSERT_NO_FATAL_FAILURE(frost::test::makeDirectoryLink(alias, dir.join("first")));
    binding_work::Graph graph;
    for (const std::string name : {"compile_commands.json", "input.cpp", "frost_schema.lua"}) {
        const auto spelling = io::joinPath(alias, name).value();
        graph.physicalReceipts.emplace(spelling, fs.physicalPath(spelling).value());
    }
    ASSERT_TRUE(graph.verifyReceipts(fs));
    ASSERT_TRUE(fs.removeFile(alias));
    ASSERT_NO_FATAL_FAILURE(frost::test::makeDirectoryLink(alias, dir.join("second")));
    EXPECT_FALSE(graph.verifyReceipts(fs));
    for (const auto& [spelling, original] : graph.physicalReceipts) {
        EXPECT_EQ(fs.readFile(spelling).value(), fs.readFile(original).value());
        EXPECT_NE(lang::documentKey(fs.physicalPath(spelling).value()), lang::documentKey(original));
    }
    ASSERT_TRUE(fs.removeFile(alias));
    EXPECT_FALSE(graph.verifyReceipts(fs)); // Dangling/missing original spelling never falls back.
}

TEST(WorkbenchBindings, ActualOpenMetadataAliasAndDirtyLuaCaptureRetainChosenLexicalIdentity) {
    WorkDir dir("binding-open-metadata-alias");
    const auto metadata = engine::generateEngineLuaMetadata(engine::SchemaDocument{});
    dir.write("project/frost_schema.lua", metadata.text);
    dir.write("project/main.lua", "local saved = 1\n");
    dir.write("project/input.cpp", "int saved = 1;\n");
    const auto alias = dir.join("open-alias");
    ASSERT_NO_FATAL_FAILURE(frost::test::makeDirectoryLink(alias, dir.join("project")));
    auto& fs = io::realFileSystem();
    binding_work::Graph graph;
    graph.physicalRoot = fs.physicalPath(dir.join("project")).value();
    graph.metadataPath = dir.join("project/frost_schema.lua");
    graph.physicalReceipts.emplace(graph.metadataPath, fs.physicalPath(graph.metadataPath).value());
    const auto aliasMetadata = io::joinPath(alias, "frost_schema.lua").value();
    const auto aliasLua = io::joinPath(alias, "main.lua").value();
    const std::vector<engine::BindingDocumentCapture> open{
        {aliasMetadata, text::TextBuffer::fromUtf8(metadata.text).value().snapshot(), 17},
        {aliasLua, text::TextBuffer::fromUtf8("local unsaved = 99\n").value().snapshot(), 23}};
    ASSERT_TRUE(binding_work::captureOpenDocumentReceipts(fs, graph, open));
    std::size_t bytes = 0;
    ASSERT_TRUE(binding_work::captureDocumentSnapshot(fs, graph, open, graph.metadataPath, bytes));
    ASSERT_TRUE(binding_work::captureDocumentSnapshot(fs, graph, open, dir.join("project/main.lua"), bytes));
    const auto* generated = graph.generatedDocument();
    ASSERT_NE(generated, nullptr);
    EXPECT_EQ(generated->path, aliasMetadata);
    EXPECT_EQ(generated->role, engine::BindingDocumentRole::GeneratedEngineMetadata);
    EXPECT_TRUE(generated->snapshot.sharesStorageWith(open[0].snapshot));
    EXPECT_EQ(generated->lspVersion, 17);
    const auto lua = std::ranges::find(graph.documents, aliasLua, &engine::BindingDocumentCapture::path);
    ASSERT_NE(lua, graph.documents.end());
    EXPECT_EQ(lua->snapshot.text(), "local unsaved = 99\n");
    EXPECT_TRUE(lua->snapshot.sharesStorageWith(open[1].snapshot));
    EXPECT_EQ(lua->lspVersion, 23);
    EXPECT_TRUE(graph.verifyReceipts(fs));
    ASSERT_TRUE(binding_work::captureDocumentSnapshot(fs, graph, open, aliasMetadata, bytes));
    EXPECT_EQ(graph.documents.size(), 2U); // No duplicate physical/provider capture.
    auto ambiguous = open;
    ambiguous.push_back({graph.metadataPath, open[0].snapshot, 18});
    EXPECT_FALSE(binding_work::captureOpenDocumentReceipts(fs, graph, ambiguous));
    const auto nativePath = dir.join("project/input.cpp");
    const auto aliasNative = io::joinPath(alias, "input.cpp").value();
    const std::vector<engine::BindingDocumentCapture> dirtyNative{
        {aliasNative, text::TextBuffer::fromUtf8("int unsaved = 99;\n").value().snapshot(), 29}};
    ASSERT_TRUE(binding_work::captureOpenDocumentReceipts(fs, graph, dirtyNative));
    graph.inputHashes.emplace(fs.physicalPath(nativePath).value(), io::sha256Hex(fs.readFile(nativePath).value()));
    EXPECT_FALSE(binding_work::captureDocumentSnapshot(fs, graph, dirtyNative, nativePath, bytes));
}

TEST(WorkbenchBindings, ConfiguredHostMetadataIsRequiredOnFirstRenameWithoutAnyBindingGraph) {
    WorkDir dir("binding-first-rename-host-expectation");
    dir.write("project/main.lua", "local ordinary = 1\n");
    auto& fs = io::realFileSystem();
    const auto directory = dir.join("state/host-meta/FrostEngine-real");
    const auto metadata = io::joinPath(directory, engine::kEngineLuaStubFileName).value();
    auto configured = lang::composeLuaConfiguration("Lua 5.4", {}, std::vector<std::string>{directory});
    EXPECT_TRUE(binding_work::expectsEngineMetadata(configured, directory));
    EXPECT_FALSE(binding_work::expectsEngineMetadata(lang::luaDefaults(), directory));
    const auto actualDefinition = rpc::Json::array().push(rpc::Json::object()
        .set("uri", lang::pathToUri(dir.join("project/main.lua")).value())
        .set("range", lang::toJson(lang::LspRange{{0, 0}, {0, 1}})));
    // The graph receipt is EMPTY: the actual effective host configuration is independent evidence.
    EXPECT_FALSE(binding_work::admitRenameDefinitions(fs, actualDefinition, metadata, {},
                 binding_work::expectsEngineMetadata(configured, directory)));
    auto ordinary = binding_work::admitRenameDefinitions(fs, actualDefinition, metadata, {},
                 binding_work::expectsEngineMetadata(lang::luaDefaults(), directory));
    ASSERT_TRUE(ordinary);
    EXPECT_FALSE(ordinary->bound);
    dir.write("state/host-meta/FrostEngine-real/frost_schema.lua", "---@meta\n");
    auto available = binding_work::admitRenameDefinitions(fs, actualDefinition, metadata, {}, true);
    ASSERT_TRUE(available);
    EXPECT_FALSE(available->bound); // Available host metadata does not make every actual definition bound.
    ASSERT_TRUE(fs.removeFile(metadata));
    EXPECT_FALSE(binding_work::admitRenameDefinitions(fs, actualDefinition, metadata, {}, true));
}

} // namespace
} // namespace frost::app::test
