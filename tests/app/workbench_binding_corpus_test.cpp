// SPDX-License-Identifier: Apache-2.0
// FEC-13 mandatory Workbench/widget corpus: actual pinned schema, AST mapper, clangd and LuaLS.
// All previews are read-only. The separate engine corpus owns disposable application/build tests.
#include "frost/engine/binding_queries.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/uri.hpp"
#include "frost/settings/keys.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "../engine/binding_corpus_support.hpp"
#include "workbench_rig.hpp"
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace frost::app::test {
namespace {
namespace corpus = frost::test::binding;
using rpc::Json;
using namespace std::chrono_literals;
using Location = std::tuple<std::string, std::string, std::uint64_t, std::uint64_t>;

// Await with the GUI pumping: the actual Workbench providers and their queued lifecycle callbacks
// must make progress. No fabricated definition/readiness reply and no blocking GUI join.
struct OraclePendingTasks {
    std::vector<sched::JoinHandle<Result<Json>>> jobs;
    // Declared before WorkbenchRig: final drain runs only after its actual providers have
    // completed shutdown/destruction, while the oracle scheduler is still alive.
    ~OraclePendingTasks() {
        for (auto& job : jobs)
            (void)job.join();
    }
};
class RawOracle {
public:
    RawOracle(Workbench& workbench, sched::ThreadPool& pool, OraclePendingTasks& pending)
        : m_workbench(workbench), m_pool(pool), m_pending(pending) {}
    ~RawOracle() {
        if (m_pending.jobs.empty())
            return;
        // A stalled/cancelled request remains owned. Stop actual providers while the Workbench
        // and services still exist, then pump their cancellation before the oracle pool drains.
        m_workbench.shutdown();
        for (auto& job : m_pending.jobs) {
            EXPECT_TRUE(waitFor([&] { return job.ready(); }, 90s));
        }
    }
    Result<Json> request(lang::ILspRequester& provider, std::string method, Json params) {
        std::stop_source cancellation;
        auto job = m_pool.spawn(sched::Lane::UserVisible,
                                provider.request(std::move(method), std::move(params), cancellation.get_token()));
        if (!waitFor([&] { return job.ready(); }, 15min)) {
            cancellation.request_stop();
            if (!waitFor([&] { return job.ready(); }, 10s))
                m_pending.jobs.push_back(std::move(job));
            else
                (void)job.join();
            return fail(ErrorCode::Timeout, "real provider request exceeded corpus deadline and was cancelled");
        }
        return job.join(); // Already ready; no blocking GUI join.
    }

private:
    Workbench& m_workbench;
    sched::ThreadPool& m_pool;
    OraclePendingTasks& m_pending;
};

std::string physical(std::string_view path) {
    return io::realFileSystem().canonicalPath(path).value_or(std::string(path));
}

Result<std::set<Location>> rawLocations(const Json& raw, std::string language, lang::PositionEncoding encoding,
                                        const std::map<std::string, text::Snapshot>& snapshots) {
    if (!raw.isArray())
        return fail(ErrorCode::Corrupt, "real references did not return an array");
    std::set<Location> result;
    for (const auto& row : raw.items()) {
        auto location = lang::parseLocation(row);
        if (!location)
            return fail(ErrorCode::Corrupt, "malformed real provider location");
        const auto path = physical(location->path);
        const auto found = snapshots.find(lang::documentKey(path));
        if (found == snapshots.end())
            return fail(ErrorCode::NotFound, "raw oracle returned an uncaptured file");
        auto begin = lang::fromLspPosition(found->second, location->range.start, encoding);
        auto end = lang::fromLspPosition(found->second, location->range.end, encoding);
        if (!begin || !end || *end < *begin)
            return fail(ErrorCode::Corrupt, "invalid raw provider range");
        result.emplace(language, lang::documentKey(path), begin->value(), end->value());
    }
    return result;
}

TEST(WorkbenchBindingCorpus, ActualWidgetsAndGenericLuaActionsMatchRawProvidersAndKeepSourcesReadOnly) {
    frost::test::RealEngineBuild build;
    ASSERT_TRUE(build.prepare()); // Shared pin/build lock; reuses the core corpus's existing build.
    corpus::Evidence evidence;
    auto& fs = io::realFileSystem();
    const auto root = physical(build.corpus);
    const auto script = physical(root + "/TestProject/Assets/Scripts/FeatureTest/Hero.lua");
    const auto originalScript = fs.readFileSnapshot(script, 16U * 1024U * 1024U);
    ASSERT_TRUE(originalScript);
    const auto originalCdb = fs.readFileSnapshot(
        root + "/build/" FROST_CORPUS_FROSTENGINE_PRESET "/compile_commands.json", 32U * 1024U * 1024U);
    ASSERT_TRUE(originalCdb);

    auto clangd = lang::findClangd(fs, lang::defaultClangdSearch());
    ASSERT_TRUE(clangd);
    ASSERT_NE(clangd->origin, lang::ClangdOrigin::Path);
#ifdef _WIN32
    constexpr auto luaName = "lua-language-server.exe";
#else
    constexpr auto luaName = "lua-language-server";
#endif
    const auto sdk = frost::test::environment("FROST_SDK");
    ASSERT_FALSE(sdk.empty());
    const auto luaPath =
        io::joinPath(sdk, "luals-" + std::string(lang::pinnedLuaLanguageServerVersion()) + "/bin/" + luaName);
    ASSERT_TRUE(luaPath);
    auto lua = lang::findLuaLanguageServer(fs, {.configuredPath = *luaPath});
    ASSERT_TRUE(lua);

    WorkDir profile("binding-real-workbench");
    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.engineBindingToolPath = FROST_CLANG_BINARY;
    options.workbench.luaSearch = lang::LuaSearch{.configuredPath = *lua};
    // Providers/Workbench are destroyed before this pool, including on a failed ASSERT.
    sched::ThreadPool oraclePool({.workerCount = 2});
    OraclePendingTasks pendingOracleTasks;
    WorkbenchRig rig(profile, std::move(options));
    RawOracle oracle(*rig.workbench, oraclePool, pendingOracleTasks);
    ASSERT_TRUE(rig.settings->set(settings::Scope::Application, settings::key::toolsClangdPath, clangd->path));
    ASSERT_TRUE(rig.settings->set(settings::Scope::Application, settings::key::toolsLuaLanguageServerPath, *lua));
    ASSERT_TRUE(rig.trust->trust(root));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(root));
    ASSERT_TRUE(rig.workbench->openPath(script));
    ASSERT_TRUE(waitFor(
        [&] {
            return rig.workbench->clangd() && rig.workbench->lua() &&
                   rig.workbench->clangd()->state() == lang::ClangdState::Running &&
                   rig.workbench->lua()->state() == lang::LuaState::Running &&
                   rig.workbench->clangd()->compileCommands().has_value();
        },
        40min));
    auto* native = rig.workbench->clangd();
    auto* luaServer = rig.workbench->lua();
    ASSERT_TRUE(native->capabilities());
    ASSERT_TRUE(luaServer->capabilities());
    EXPECT_NE(native->capabilities()->serverVersion.find("23.1.4"), std::string::npos);
    EXPECT_NE(luaServer->capabilities()->serverVersion.find(lang::pinnedLuaLanguageServerVersion()), std::string::npos);

    // An actual unsaved editor overlay is the Lua reference oracle. The pinned source is never
    // rewritten, even when generic Rename is invoked later.
    auto* view = rig.workbench->currentView();
    ASSERT_NE(view, nullptr);
    view->editor().setState(text::EditorState::caretAt(ByteOffset{0}));
    const std::string prefix = "local fec13_frames = Application.GetFrameCount()\n";
    ASSERT_TRUE(view->editor().typeText(prefix));
    ASSERT_TRUE(view->document().dirty());
    const auto overlay = view->editor().editor().text();
    view->editor().setState(text::EditorState::caretAt(ByteOffset{prefix.find("GetFrameCount") + 2}));

    // Real host warmup and real mapper, independently captured before the app graph request.
    const auto host = rig.workbench->engineLuaHostProvider();
    ASSERT_NE(host, nullptr);
    std::optional<engine::EngineLuaSchemaCapture> schemaCapture;
    ASSERT_TRUE(waitFor(
        [&] {
            schemaCapture = host->cachedSchemaCapture(fs, root);
            return schemaCapture.has_value();
        },
        5min));
    const auto& schema = schemaCapture->schema;
    const auto selected = std::ranges::find_if(
        schema.luaEntries, [](const auto& row) { return row.qualifiedName == "Application.GetFrameCount"; });
    ASSERT_NE(selected, schema.luaEntries.end());
    ASSERT_TRUE(selected->source);
    const auto source = physical(root + "/" + selected->source->file);
    const auto originalSource = fs.readFileSnapshot(source, 16U * 1024U * 1024U);
    ASSERT_TRUE(originalSource);
    const auto actualDatabase = native->compileCommands();
    ASSERT_TRUE(actualDatabase);
    auto mapRequest = engine::bindingMapRequest(schema, root, actualDatabase->directory, source);
    ASSERT_TRUE(mapRequest);
    const auto requestPath = evidence.root + "/workbench-raw-map-request.json";
    ASSERT_TRUE(fs.writeFileAtomic(requestPath, mapRequest->dump()));
    auto mapRun =
        evidence.command("workbench-raw-map", FROST_CLANG_BINARY, {"--binding-map", requestPath}, root, 10min);
    ASSERT_TRUE(mapRun);
    ASSERT_EQ(mapRun->exitCode, 0) << mapRun->stderrBytes;
    auto mapJson = rpc::parseJson(mapRun->stdoutBytes);
    ASSERT_TRUE(mapJson);
    ASSERT_EQ(mapJson->boolAt("translationUnitParsed"), true);
    auto report = engine::parseBindingMapReport(*mapRequest, *mapJson);
    ASSERT_TRUE(report);
    auto index = engine::BindingIndex::create(root, 1, schema, report->mappings, report->issues);
    ASSERT_TRUE(index);
    const auto* binding = index->findLua(selected->qualifiedName);
    ASSERT_NE(binding, nullptr);
    ASSERT_EQ(binding->resolution, engine::BindingResolution::Resolved);
    ASSERT_EQ(binding->native.size(), 1u);
    ASSERT_TRUE(binding->native.front().nameLiteral);
    ASSERT_TRUE(binding->native.front().docSignatureLiteral);
    const auto generated = engine::generateEngineLuaMetadata(schema);
    auto metaDir = lang::luaHostMetaDirectory(profile.join("data/luals"), "FrostEngine", root);
    ASSERT_TRUE(metaDir);
    const auto metadataPath = physical(*metaDir + "/frost_schema.lua");
    auto metadataBytes = fs.readFileSnapshot(metadataPath, 16U * 1024U * 1024U);
    ASSERT_TRUE(metadataBytes);
    ASSERT_EQ(*metadataBytes, generated.text);
    auto metaBuffer = text::TextBuffer::fromUtf8(*metadataBytes);
    ASSERT_TRUE(metaBuffer);
    auto anchors = engine::captureBindingLuaAnchors(generated, metadataPath, metaBuffer->snapshot());
    ASSERT_TRUE(anchors);
    const auto anchor = std::ranges::find_if(*anchors, [&](const auto& value) {
        return value.luaQualifiedName == selected->qualifiedName && !value.ambiguous;
    });
    ASSERT_NE(anchor, anchors->end());
    // The actual Lua server's definition must reach the generated declaration before usages.
    bool definitionReady = false;
    ASSERT_TRUE(waitFor(
        [&] {
            auto params = lang::textDocumentPosition(
                script,
                lang::toLspPosition(overlay, ByteOffset{prefix.find("GetFrameCount") + 2}, luaServer->encoding()));
            auto raw = oracle.request(*luaServer, "textDocument/definition", *params);
            if (!raw)
                return false;
            if (raw->isObject())
                *raw = Json::array().push(*raw);
            if (!raw->isArray())
                return false;
            for (const auto& row : raw->items()) {
                auto where = lang::parseLocation(row);
                if (where && lang::documentKey(physical(where->path)) == lang::documentKey(metadataPath)) {
                    auto begin =
                        lang::fromLspPosition(metaBuffer->snapshot(), where->range.start, luaServer->encoding());
                    auto end = lang::fromLspPosition(metaBuffer->snapshot(), where->range.end, luaServer->encoding());
                    if (begin && end && *begin <= anchor->declaration.bytes.begin &&
                        *end >= anchor->declaration.bytes.end)
                        definitionReady = true;
                }
            }
            return definitionReady;
        },
        5min));

    rig.window->toolWindows().show("engineBindings");
    auto* refresh = rig.window->findChild<QPushButton*>(QStringLiteral("engineBindingsRefreshButton"));
    auto* identity = rig.window->findChild<QComboBox*>(QStringLiteral("engineBindingIdentity"));
    auto* usagesWidget = rig.window->findChild<QTreeWidget*>(QStringLiteral("engineBindingUsages"));
    auto* previewWidget = rig.window->findChild<QPlainTextEdit*>(QStringLiteral("engineBindingRenamePreview"));
    ASSERT_NE(refresh, nullptr);
    ASSERT_NE(identity, nullptr);
    ASSERT_NE(usagesWidget, nullptr);
    ASSERT_NE(previewWidget, nullptr);
    refresh->click();
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->engineBindingState().boolAt("busy").value_or(false); }, 40min));
    ASSERT_EQ(rig.workbench->engineBindingState().boolAt("available"), true)
        << rig.workbench->engineBindingState().dump();
    const int selectedIndex = identity->findData(QStringLiteral("Application.GetFrameCount"));
    ASSERT_GE(selectedIndex, 0);
    identity->setCurrentIndex(selectedIndex);
    // Standard Alt+F7 entry point must reach combined semantics, with no recursive stock fallback.
    ASSERT_EQ(rig.window->invoke("navigate.findUsages"), input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->engineBindingState().boolAt("busy").value_or(false); }, 40min));
    auto state = rig.workbench->engineBindingState();
    ASSERT_EQ(state.boolAt("available"), true) << state.dump();
    ASSERT_NE(state.find("usages"), nullptr);
    ASSERT_FALSE(state.find("usages")->items().empty()) << state.dump();
    ASSERT_TRUE(evidence.write("workbench-usages.json", state.dump()));

    std::map<std::string, text::Snapshot> snapshots;
    snapshots.emplace(lang::documentKey(script), overlay);
    snapshots.emplace(lang::documentKey(metadataPath), metaBuffer->snapshot());
    for (const auto& input : report->sourceInputs) {
        auto bytes = fs.readFileSnapshot(input.path, 16U * 1024U * 1024U);
        ASSERT_TRUE(bytes);
        ASSERT_EQ(io::sha256Hex(*bytes), input.sha256);
        auto buffer = text::TextBuffer::fromUtf8(*bytes);
        ASSERT_TRUE(buffer);
        snapshots.emplace(lang::documentKey(input.path), buffer->snapshot());
    }
    // Capture actual provider-returned files lazily for the independent raw oracle, without
    // assigning edit ownership or manufacturing references from spelling.
    const auto queryRaw = [&](lang::ILspRequester& provider, const std::string& path, ByteOffset offset,
                              const text::Snapshot& snapshot) -> Result<Json> {
        auto params = lang::textDocumentPosition(path, lang::toLspPosition(snapshot, offset, provider.encoding()));
        if (!params)
            return std::unexpected(params.error());
        params->set("context", Json::object().set("includeDeclaration", true));
        auto raw = oracle.request(provider, "textDocument/references", *params);
        if (raw && raw->isArray())
            for (const auto& row : raw->items()) {
                auto location = lang::parseLocation(row);
                if (!location)
                    return fail(ErrorCode::Corrupt, "malformed real provider location");
                const auto physicalPath = physical(location->path);
                const auto key = lang::documentKey(physicalPath);
                if (snapshots.contains(key))
                    continue;
                auto bytes = fs.readFileSnapshot(physicalPath, 16U * 1024U * 1024U);
                if (!bytes)
                    return std::unexpected(bytes.error());
                auto buffer = text::TextBuffer::fromUtf8(*bytes);
                if (!buffer)
                    return std::unexpected(buffer.error());
                snapshots.emplace(key, buffer->snapshot());
            }
        return raw;
    };
    std::set<Location> expected;
    const auto& nativeBinding = binding->native.front();
    ASSERT_TRUE(native->documentVersion(nativeBinding.declaration.path)); // Actual staged native lease.
    const auto nativeSnapshot = snapshots.at(lang::documentKey(nativeBinding.declaration.path));
    auto symbolParams = lang::textDocumentPosition(
        nativeBinding.declaration.path,
        lang::toLspPosition(nativeSnapshot, nativeBinding.declaration.bytes.begin, native->encoding()));
    ASSERT_TRUE(symbolParams);
    auto symbol = oracle.request(*native, "textDocument/symbolInfo", *symbolParams);
    ASSERT_TRUE(symbol);
    ASSERT_TRUE(symbol->isArray());
    ASSERT_EQ(symbol->size(), 1u);
    ASSERT_EQ(symbol->items().front().stringAt("usr"), nativeBinding.usr);
    auto nativeRaw =
        queryRaw(*native, nativeBinding.declaration.path, nativeBinding.declaration.bytes.begin, nativeSnapshot);
    ASSERT_TRUE(nativeRaw);
    ASSERT_TRUE(evidence.write("workbench-native-raw-references.json", nativeRaw->dump()));
    auto nativeLocations = rawLocations(*nativeRaw, "cpp", native->encoding(), snapshots);
    ASSERT_TRUE(nativeLocations);
    expected.insert(nativeLocations->begin(), nativeLocations->end());
    // Usage aliases come from the actual exported sameAs graph, never automatic rename expansion.
    auto seeds = engine::bindingUsageSeeds(
        *index, nativeBinding.usr, {engine::BindingLanguage::Native, "raw-native", 1, native->encoding()},
        {engine::BindingLanguage::Lua, "raw-lua", 1, luaServer->encoding()}, *anchors);
    ASSERT_TRUE(seeds);
    for (const auto& seed : *seeds) {
        if (seed.provider.language != engine::BindingLanguage::Lua)
            continue;
        auto raw = queryRaw(*luaServer, metadataPath, seed.anchor.bytes.begin, metaBuffer->snapshot());
        ASSERT_TRUE(raw);
        auto locations = rawLocations(*raw, "lua", luaServer->encoding(), snapshots);
        ASSERT_TRUE(locations);
        expected.insert(locations->begin(), locations->end());
    }
    std::set<Location> actual;
    for (const auto& row : state.find("usages")->items())
        actual.emplace(std::string(row.stringAt("language")), lang::documentKey(physical(row.stringAt("path"))),
                       row.unsignedAt("begin").value(), row.unsignedAt("end").value());
    EXPECT_EQ(actual, expected);
    EXPECT_TRUE(waitFor(
        [&] {
            return usagesWidget->topLevelItemCount() == static_cast<int>(std::min<std::size_t>(actual.size(), 10000));
        },
        30s));
    ASSERT_NE(state.find("coverage"), nullptr);
    EXPECT_EQ(state.find("coverage")->stringAt("scope"), "provider-indexed");
    EXPECT_EQ(state.find("coverage")->boolAt("embeddedLanguageReferencesVerified"), false);
    EXPECT_EQ(state.find("coverage")->boolAt("applicationSafe"), false);

    // A terminal selected-preview refusal must release actual temporary native documents;
    // otherwise an unsuccessful query silently leaves clangd ownership behind.
    ASSERT_TRUE(native->documentVersion(nativeBinding.declaration.path));
    ASSERT_TRUE(rig.workbench->previewEngineBindingRename("invalid-lua-identifier"));
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->engineBindingState().boolAt("busy").value_or(false); }, 40min));
    EXPECT_EQ(rig.workbench->engineBindingState().boolAt("previewReady"), false);
    EXPECT_FALSE(native->documentVersion(nativeBinding.declaration.path));
    EXPECT_EQ(fs.readFileSnapshot(source, 16U * 1024U * 1024U).value(), *originalSource);
    ASSERT_TRUE(rig.workbench->findEngineBindingUsages());
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->engineBindingState().boolAt("busy").value_or(false); }, 40min));
    ASSERT_EQ(rig.workbench->engineBindingState().boolAt("available"), true)
        << rig.workbench->engineBindingState().dump();
    ASSERT_TRUE(native->documentVersion(nativeBinding.declaration.path));

    // A real editor opening a document used by staged analysis takes ownership. Subsequent
    // cancellation/cleanup cannot close the ordinary document or overwrite its supplied text.
    ASSERT_TRUE(rig.workbench->openPath(nativeBinding.declaration.path));
    auto* nativeView = rig.workbench->currentView();
    ASSERT_NE(nativeView, nullptr);
    ASSERT_TRUE(native->documentVersion(nativeBinding.declaration.path));
    const auto adoptedVersion = native->documentVersion(nativeBinding.declaration.path);
    EXPECT_EQ(nativeView->editor().editor().text().text(), nativeSnapshot.text());
    ASSERT_TRUE(rig.workbench->openPath(script));
    EXPECT_EQ(native->documentVersion(nativeBinding.declaration.path), adoptedVersion);
    EXPECT_EQ(view->editor().editor().text().text(), overlay.text());
    view->editor().setState(text::EditorState::caretAt(ByteOffset{prefix.find("GetFrameCount") + 2}));

    // Generic Lua Rename must be intercepted after the real semantic definition, and must not
    // apply arbitrary LuaLS writes to generated metadata or to native source in R7.
    rig.workbench->language()->rename();
    ASSERT_TRUE(waitFor([&] { return rig.workbench->language()->renameInput().isVisible(); }, 30s));
    auto* renameField = rig.workbench->language()->renameInput().field();
    renameField->setText(QStringLiteral("GetFrameCountWorkbenchPreview"));
    sendKey(renameField, Qt::Key_Return, Qt::NoModifier);
    ASSERT_TRUE(waitFor(
        [&] {
            return rig.workbench->engineBindingState().boolAt("previewReady").value_or(false) ||
                   !rig.workbench->engineBindingState().boolAt("busy").value_or(false);
        },
        40min));
    state = rig.workbench->engineBindingState();
    ASSERT_EQ(state.boolAt("previewReady"), true) << state.dump();
    ASSERT_TRUE(evidence.write("workbench-readonly-preview.json", state.dump()));
    const auto* preview = state.find("preview");
    ASSERT_NE(preview, nullptr);
    EXPECT_EQ(preview->stringAt("previousIdentity"), "Application.GetFrameCount");
    EXPECT_EQ(preview->stringAt("renamedIdentity"), "Application.GetFrameCountWorkbenchPreview");
    EXPECT_EQ(preview->boolAt("nativeRenameRequested"), false);
    EXPECT_EQ(preview->boolAt("embeddedLanguageReferencesVerified"), true);
    EXPECT_EQ(preview->boolAt("applicationSafe"), true);
    EXPECT_NE(preview->find("coverageExclusions"), nullptr);
    EXPECT_TRUE(previewWidget->toPlainText().contains(QStringLiteral("Apply scope:")));
    EXPECT_TRUE(previewWidget->isReadOnly());
    EXPECT_TRUE(previewWidget->toPlainText().contains(QStringLiteral("Embedded Lua")));
    bool registrationEdit = false, signatureEdit = false;
    for (const auto& file : preview->find("files")->items())
        for (const auto& edit : file.find("edits")->items()) {
            if (edit.stringAt("origin") != "ast-literal")
                continue;
            registrationEdit |= edit.unsignedAt("begin") == nativeBinding.nameLiteral->range.bytes.begin.value() &&
                                edit.stringAt("before") == nativeBinding.nameLiteral->spelling;
            signatureEdit |=
                edit.unsignedAt("begin") == nativeBinding.docSignatureLiteral->literal.range.bytes.begin.value() &&
                edit.stringAt("before") == nativeBinding.docSignatureLiteral->literal.spelling;
        }
    EXPECT_TRUE(registrationEdit);
    EXPECT_TRUE(signatureEdit);
    ASSERT_NE(preview->find("generatedResource"), nullptr);
    EXPECT_EQ(preview->find("generatedResource")->stringAt("intent"), "regenerate");
    EXPECT_EQ(preview->find("generatedResource")->stringAt("beforeSha256"), io::sha256Hex(*metadataBytes));
    EXPECT_EQ(fs.readFileSnapshot(script, 16U * 1024U * 1024U).value(), *originalScript);
    EXPECT_EQ(fs.readFileSnapshot(source, 16U * 1024U * 1024U).value(), *originalSource);
    EXPECT_EQ(fs.readFileSnapshot(metadataPath, 16U * 1024U * 1024U).value(), *metadataBytes);
    EXPECT_EQ(view->editor().editor().text().text(), overlay.text());
    EXPECT_TRUE(view->document().dirty());

    // Invalidation between a real request's start and queued publication rejects its capture.
    ASSERT_TRUE(rig.workbench->findEngineBindingUsages());
    ASSERT_TRUE(rig.trust->untrust(root));
    ASSERT_TRUE(waitFor([&] { return !rig.workbench->engineBindingState().boolAt("busy").value_or(false); }, 30s));
    EXPECT_EQ(rig.workbench->engineBindingState().boolAt("available"), false);
    EXPECT_EQ(rig.workbench->engineBindingState().boolAt("previewReady"), false);
    EXPECT_EQ(fs.readFileSnapshot(script, 16U * 1024U * 1024U).value(), *originalScript);
    EXPECT_EQ(fs.readFileSnapshot(source, 16U * 1024U * 1024U).value(), *originalSource);
    // Revert only the unsaved fixture overlay before normal shutdown; no pinned file save.
    ASSERT_TRUE(view->editor().perform(ui::EditorAction::Undo));
    EXPECT_EQ(view->editor().editor().text().text(), *originalScript);
}
} // namespace
} // namespace frost::app::test
