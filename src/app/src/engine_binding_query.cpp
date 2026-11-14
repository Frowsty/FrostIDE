// SPDX-License-Identifier: Apache-2.0
// FEC-13 worker-side semantic caret resolution and actual per-provider combined query dispatch.
#include "frost/io/spawn_log.hpp"
#include "frost/io/utf.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/uri.hpp"

#include "engine_binding_work.hpp"

#include <algorithm>
#include <set>

namespace frost::app::binding_work {
namespace {
using engine::BindingLanguage;
using rpc::Json;

const engine::BindingDocumentCapture* documentFor(const Graph& graph, std::string_view path) {
    const auto key = lang::documentKey(path);
    const auto at =
        std::ranges::find_if(graph.documents, [&](const auto& value) { return lang::documentKey(value.path) == key; });
    return at == graph.documents.end() ? nullptr : &*at;
}

sched::Task<Result<Json>> request(const Capture& capture, bool native, std::string method, Json params) {
    if (!capture.current())
        co_return fail(ErrorCode::Cancelled, "The binding query capture changed before request dispatch");
    lang::ILspRequester& provider =
        native ? static_cast<lang::ILspRequester&>(*capture.native) : static_cast<lang::ILspRequester&>(*capture.lua);
    auto result = co_await provider.request(std::move(method), std::move(params), capture.stop);
    if (!capture.current())
        co_return fail(ErrorCode::Cancelled, "The binding query capture changed while awaiting the provider");
    co_return result;
}

struct Selection {
    std::string luaIdentity, nativeUsr;
};
sched::Task<Result<Selection>> resolve(const Capture& capture, const Graph& graph,
                                       std::span<const engine::LuaBindingAnchor> anchors) {
    if (!capture.selectedLuaIdentity.empty()) {
        const auto* binding = graph.index->findLua(capture.selectedLuaIdentity);
        if (!binding || binding->resolution != engine::BindingResolution::Resolved)
            co_return fail(ErrorCode::NotFound, "The selected schema binding has no unambiguous native mapping");
        co_return Selection{capture.selectedLuaIdentity, {}};
    }
    const auto* active = documentFor(graph, capture.activePath);
    if (!active || capture.caret > active->snapshot.size())
        co_return fail(ErrorCode::NotFound, "The active editor has no captured semantic query position");
    const bool native = !luaSource(capture.activePath);
    const auto encoding = native ? capture.nativeProvider.encoding : capture.luaProvider.encoding;
    auto params =
        lang::textDocumentPosition(active->path, lang::toLspPosition(active->snapshot, capture.caret, encoding));
    if (!params)
        co_return std::unexpected(params.error());
    auto raw = co_await request(capture, native, native ? "textDocument/symbolInfo" : "textDocument/definition",
                                std::move(*params));
    if (!raw)
        co_return std::unexpected(raw.error());
    if (native) {
        if (!raw->isArray() || raw->size() != 1 || raw->items().front().stringAt("usr").empty())
            co_return Selection{};
        co_return Selection{{}, std::string(raw->items().front().stringAt("usr"))};
    }
    if (raw->isObject())
        *raw = Json::array().push(std::move(*raw));
    auto locations = engine::normalizeBindingReferences(capture.luaProvider, *raw, graph.documents);
    if (!locations)
        co_return std::unexpected(locations.error());
    const auto* generated = graph.generatedDocument();
    if (!generated)
        co_return fail(ErrorCode::NotFound, "The unique captured generated metadata identity is unavailable");
    std::set<std::string> identities;
    for (const auto& location : *locations) {
        if (lang::documentKey(location.location.path) != lang::documentKey(generated->path))
            continue;
        for (const auto& anchor : anchors) {
            if (anchor.ambiguous)
                continue;
            const auto& range = location.location.bytes;
            if (range.begin <= anchor.declaration.bytes.begin && range.end >= anchor.declaration.bytes.end)
                identities.insert(anchor.luaQualifiedName);
        }
    }
    if (identities.size() > 1)
        co_return fail(ErrorCode::Busy, "LuaLS's actual definition spans multiple engine declarations");
    co_return Selection{identities.empty() ? std::string{} : *identities.begin(), {}};
}
} // namespace

sched::Task<Result<ResultData>> perform(Capture capture, std::shared_ptr<Graph> reusable) {
    co_await capture.scheduler->schedule(sched::Lane::Background);
    std::shared_ptr<Graph> graph;
    if (reusable && reusable->verify(capture))
        graph = std::move(reusable);
    else {
        auto built = buildGraph(capture);
        if (!built)
            co_return std::unexpected(built.error());
        graph = std::move(*built);
    }
    ResultData result;
    result.graph = graph;
    if (capture.operation == Operation::Refresh)
        co_return result;
    const auto* metadata = graph->generatedDocument();
    if (!metadata)
        co_return fail(ErrorCode::NotFound, "The actual generated engine metadata was not captured");
    const auto generated = engine::generateEngineLuaMetadata(graph->index->schema());
    auto anchors = engine::captureBindingLuaAnchors(generated, metadata->path, metadata->snapshot);
    if (!anchors)
        co_return std::unexpected(anchors.error());
    auto selected = co_await resolve(capture, *graph, *anchors);
    if (!selected)
        co_return std::unexpected(selected.error());
    co_await capture.scheduler->schedule(sched::Lane::Background);
    if (!graph->verify(capture))
        co_return fail(ErrorCode::Busy, "Binding AST/documents changed while resolving the semantic caret");
    std::set<std::string> usrs;
    if (!selected->nativeUsr.empty())
        usrs.insert(selected->nativeUsr);
    const engine::IndexedBinding* selectedBinding = nullptr;
    if (!selected->luaIdentity.empty()) {
        selectedBinding = graph->index->findLua(selected->luaIdentity);
        if (!selectedBinding || selectedBinding->resolution != engine::BindingResolution::Resolved)
            co_return fail(ErrorCode::Unsupported,
                           "LuaLS identifies an engine declaration whose native mapping is unavailable or ambiguous");
        if (selectedBinding && selectedBinding->resolution == engine::BindingResolution::Resolved)
            for (const auto& mapping : selectedBinding->native)
                usrs.insert(mapping.usr);
    }
    if (usrs.empty() || (!selected->nativeUsr.empty() && graph->index->forNative(selected->nativeUsr).empty())) {
        result.ordinaryUsages = true;
        co_return result;
    }
    std::vector<engine::BindingQuerySeed> seeds;
    for (const auto& usr : usrs) {
        auto found =
            engine::bindingUsageSeeds(*graph->index, usr, capture.nativeProvider, capture.luaProvider, *anchors);
        if (!found)
            co_return std::unexpected(found.error());
        for (auto& seed : *found)
            if (std::ranges::none_of(seeds, [&](const auto& old) {
                    return old.provider == seed.provider && old.anchor == seed.anchor &&
                           old.luaQualifiedName == seed.luaQualifiedName && old.nativeUsr == seed.nativeUsr;
                }))
                seeds.push_back(std::move(seed));
    }
    // clangd requires a live document for semantic queries at a mapped declaration. Opening
    // it is staged on the GUI thread to serialize with editor adoption; the second worker
    // validates the exact owned lease and provider generation before dispatch.
    auto queryDocuments = graph->documents;
    std::set<std::string> requiredNativePaths;
    for (const auto& seed : seeds) {
        if (seed.provider.language != BindingLanguage::Native ||
            !requiredNativePaths.insert(lang::documentKey(seed.anchor.path)).second)
            continue;
        auto document = std::ranges::find_if(queryDocuments, [&](const auto& value) {
            return lang::documentKey(value.path) == lang::documentKey(seed.anchor.path) ||
                   (!value.physicalPath.empty() &&
                    lang::documentKey(value.physicalPath) == lang::documentKey(seed.anchor.path));
        });
        if (document == queryDocuments.end())
            co_return fail(ErrorCode::NotFound, "A native binding seed has no captured source snapshot");
        auto lease = std::ranges::find_if(capture.analysisDocuments, [&](const auto& value) {
            return lang::documentKey(value.path) == lang::documentKey(document->path);
        });
        if (lease != capture.analysisDocuments.end()) {
            if (!capture.native->analysisDocumentCurrent(*lease) ||
                !lease->snapshot.sharesStorageWith(document->snapshot) ||
                lease->snapshot.version() != document->snapshot.version())
                co_return fail(ErrorCode::Busy, "A native analysis document was adopted or changed before dispatch");
            document->lspVersion = lease->lspVersion;
        } else if (!document->lspVersion) {
            if (capture.native->documentVersion(document->path))
                co_return fail(ErrorCode::Busy, "The native seed is open without this capture's snapshot ownership");
            result.nativeDocumentsToSynchronize.push_back(*document);
        }
    }
    if (!result.nativeDocumentsToSynchronize.empty()) {
        if (result.nativeDocumentsToSynchronize.size() > 256)
            co_return fail(ErrorCode::OutOfRange, "Binding query requires more than 256 temporary native documents");
        co_return result;
    }
    const engine::BindingQueryAdmission admission = [capture](const auto& provider) {
        return capture.current() && (provider == capture.nativeProvider || provider == capture.luaProvider);
    };
    if (capture.operation == Operation::Usages) {
        std::vector<engine::BindingProviderRequest> requests;
        for (auto& seed : seeds)
            requests.push_back({seed, seed.provider.language == BindingLanguage::Native
                                          ? static_cast<lang::ILspRequester*>(capture.native.get())
                                          : static_cast<lang::ILspRequester*>(capture.lua.get())});
        auto usages = co_await engine::queryBindingUsages(std::move(requests), queryDocuments, admission, capture.stop);
        if (!usages)
            co_return std::unexpected(usages.error());
        result.usages = std::move(*usages);
    } else {
        if (!selectedBinding) {
            const auto bindings = graph->index->forNative(selected->nativeUsr);
            if (bindings.size() != 1)
                co_return fail(
                    ErrorCode::Busy,
                    "Select the exact Lua identity in Bindings before previewing a rename; aliases are independent");
            selectedBinding = bindings.front();
        }
        const auto luaSeed = std::ranges::find_if(seeds, [&](const auto& seed) {
            return seed.provider.language == BindingLanguage::Lua &&
                   seed.luaQualifiedName == selectedBinding->lua.qualifiedName;
        });
        if (luaSeed == seeds.end())
            co_return fail(ErrorCode::NotFound, "The selected Lua identity has no tracked metadata seed");
        engine::SelectedLuaBindingRename rename{{*luaSeed, capture.lua.get()}, capture.newName};
        std::vector<engine::BindingRenameRequest> nativeRenames;
        if (capture.renameNative) {
            if (usrs.size() != 1)
                co_return fail(ErrorCode::Busy,
                               "Select an exact native accessor before renaming a multi-target property");
            const auto nativeSeed = std::ranges::find_if(
                seeds, [](const auto& seed) { return seed.provider.language == BindingLanguage::Native; });
            if (nativeSeed == seeds.end())
                co_return fail(ErrorCode::NotFound, "The selected binding has no native query seed");
            nativeRenames.push_back({{*nativeSeed, capture.native.get()}, capture.newName});
        }
        struct ProjectionLease {
            std::shared_ptr<lang::LuaService> provider;
            std::vector<std::string> paths;
            ~ProjectionLease() { for (const auto& path : paths) (void)provider->closeDocument(path); }
        } projections{capture.lua, {}};
        std::vector<engine::BindingEmbeddedLuaCapture> chunks;
        if (graph->embeddedLua.size() > 1024)
            co_return fail(ErrorCode::OutOfRange, "The bounded binding profile supports at most 1024 embedded Lua chunks");
        std::size_t projectedBytes = 0;
        for (const auto& literal : graph->embeddedLua) {
            if (literal.decodedValue.size() > 16U * 1024U * 1024U - projectedBytes)
                co_return fail(ErrorCode::OutOfRange, "The embedded Lua profile exceeds its 16 MiB projection bound");
            projectedBytes += literal.decodedValue.size();
            if (!capture.current())
                co_return fail(ErrorCode::Cancelled, "Embedded Lua capture was cancelled");
            auto text = text::TextBuffer::fromUtf8(literal.decodedValue);
            if (!text)
                co_return std::unexpected(text.error());
            // Reserved unique nonexistent sibling URI: no files/cache contents are written.
            auto path = io::joinPath(capture.root, ".frost-binding-" + capture.requestTag + "-" +
                std::to_string(chunks.size()) + ".lua");
            if (!path)
                co_return std::unexpected(path.error());
            auto opened = capture.lua->openDocument(*path, text->snapshot());
            if (!opened)
                co_return std::unexpected(opened.error());
            projections.paths.push_back(*path);
            engine::BindingDocumentCapture projection{*path, text->snapshot(),
                capture.lua->documentVersion(*path), engine::BindingDocumentRole::UserSource};
            queryDocuments.push_back(projection);
            chunks.push_back({literal, std::move(projection)});
        }
        auto preview = co_await engine::querySelectedBindingRenamePreview(
            graph->index, std::move(rename), std::move(nativeRenames), queryDocuments, admission, capture.stop, std::move(chunks));
        if (!preview)
            co_return std::unexpected(preview.error());
        preview->embeddedLanguageReferencesVerified = graph->embeddedLuaProfileVerified;
        preview->applicationSafe = preview->embeddedLanguageReferencesVerified;
        preview->supportedProfile = "Indexed Lua sources, native/registration semantic sites, and direct literal Frost::ScriptEngine::ExecuteString chunks";
        preview->coverageExclusions = graph->embeddedLuaRefusals;
        if (!graph->embeddedLuaProfileVerified)
            preview->applicationRefusal = "The AST tool did not verify embedded Lua coverage for every compilation unit";

        result.preview = std::move(*preview);
        result.selectedIdentity = selectedBinding->lua.qualifiedName;
        if (result.preview->applicationSafe) {
            document::RefactoringPlan plan;
            plan.label = "Rename C++/Lua binding " + result.preview->previousLuaIdentity;
            for (const auto& file : result.preview->sourceFiles) {
                const auto open = std::ranges::find_if(capture.openDocuments, [&](const auto& item) {
                    return lang::documentKey(item.path) == lang::documentKey(file.path);
                });
                if (open != capture.openDocuments.end())
                    plan.buffers.push_back({file.path, file.snapshot.text(), file.preview});
                else
                    plan.disk.push_back({file.path, file.snapshot.text(), file.preview});
            }
            // Freeze exact source/config dependencies. Derived metadata is checked by the initial
            // graph admission, not a durable Undo dependency: settled history regenerates it.
            for (const auto& document : graph->documents) {
                if (document.role == engine::BindingDocumentRole::GeneratedEngineMetadata)
                    continue;
                auto disk = capture.fs->readFileSnapshot(document.path, 16U * 1024U * 1024U);
                if (!disk)
                    co_return std::unexpected(disk.error());
                plan.dependencies.push_back({document.path, std::move(*disk)});
            }
            plan.dependencies.push_back({graph->databasePath, graph->databaseBytes});
            for (const auto& open : capture.openDocuments)
                plan.bufferDependencies.push_back({open.path, open.snapshot.text()});
            plan.revalidateContext = [capture, graph]() -> Result<void> {
                if (!graph->verify(capture))
                    return fail(ErrorCode::Busy, "Binding source/schema/trust/provider capture changed before Apply");
                return {};
            };
            result.applyPlan = std::make_shared<const document::RefactoringPlan>(std::move(plan));
        }
        constexpr std::size_t limit = 1024U * 1024U;
        const auto append = [&](std::string_view value) {
            const auto remaining = limit - result.previewDisplay.size();
            const auto prefix = value.substr(0, std::min(remaining, value.size()));
            result.previewDisplay.append(prefix.substr(0, io::validUtf8Prefix(prefix)));
        };
        const auto editText = [&](const engine::BindingPreviewEdit& edit) {
            const auto point = edit.capturedSnapshot.pointAt(edit.location.bytes.begin);
            append("  Line " + std::to_string(point.line.value() + 1) + ": " + edit.before + " -> " + edit.after +
                   "\n");
        };
        append("Apply scope: " + result.preview->supportedProfile + "\n");
        append("Computed/macro/concatenated Lua chunks excluded: " +
            std::to_string(result.preview->coverageExclusions.size()) + "\n");
        for (const auto& exclusion : result.preview->coverageExclusions)
            append("  Excluded: " + exclusion + "\n");
        append("\n");
        for (const auto& file : result.preview->sourceFiles) {
            append("File: " + file.path + "\n");
            for (const auto& edit : file.edits)
                editText(edit);
            append("\n");
        }
        append("Generated resource (regeneration intent only): " + result.preview->generatedResource.path + "\n");
        for (const auto& edit : result.preview->generatedResource.observedServerEdits)
            editText(edit);
        append("\nEmbedded Lua coverage is limited to the direct literal chunks listed by the AST profile. "
               "Runtime-computed scripts and unsupported embedding constructs are excluded.\n");
        if (result.previewDisplay.size() >= limit - 4)
            result.previewDisplay += "\n[Display limit reached; full immutable preview retained]";
        auto files = Json::array();
        std::size_t summaryBytes = 0;
        bool summaryComplete = true;
        for (const auto& file : result.preview->sourceFiles) {
            auto edits = Json::array();
            for (const auto& edit : file.edits) {
                const auto size = edit.before.size() + edit.after.size() + file.path.size() + 256;
                if (summaryBytes + size > 2U * 1024U * 1024U) {
                    summaryComplete = false;
                    break;
                }
                summaryBytes += size;
                edits.push(Json::object()
                               .set("begin", edit.location.bytes.begin.value())
                               .set("end", edit.location.bytes.end.value())
                               .set("before", edit.before)
                               .set("after", edit.after)
                               .set("origin", edit.origin == engine::BindingPreviewOrigin::RegistrationLiteral
                                                  ? "ast-literal"
                                                  : edit.origin == engine::BindingPreviewOrigin::EmbeddedLuaLiteral
                                                      ? "embedded-lua-literal" : "language-server"));
            }
            auto summary = Json::object()
                               .set("path", file.path)
                               .set("edits", std::move(edits))
                               .set("editCount", file.edits.size());
            if (file.snapshot.size().value() <= 16384 && file.preview.size() <= 16384 &&
                summaryBytes + file.snapshot.size().value() + file.preview.size() <= 2U * 1024U * 1024U) {
                summaryBytes += static_cast<std::size_t>(file.snapshot.size().value()) + file.preview.size();
                summary.set("before", file.snapshot.text()).set("after", file.preview);
            }
            files.push(std::move(summary));
            if (!summaryComplete)
                break;
        }
        const auto& resource = result.preview->generatedResource;
        auto generatedSummary =
            Json::object()
                .set("path", resource.path)
                .set("intent", "regenerate")
                .set("beforeSha256", io::sha256Hex(resource.before.text()))
                .set("regeneratedSha256", io::sha256Hex(resource.regenerated.text))
                .set("observedLanguageServerSha256", io::sha256Hex(resource.observedLanguageServerText));
        if (resource.regenerated.text.size() <= 16384)
            generatedSummary.set("before", resource.before.text())
                .set("regenerated", resource.regenerated.text)
                .set("observedLanguageServerText", resource.observedLanguageServerText);
        auto exclusions = Json::array();
        for (const auto& exclusion : result.preview->coverageExclusions)
            exclusions.push(exclusion);
        result.previewSummary = Json::object()
                                    .set("previousIdentity", result.preview->previousLuaIdentity)
                                    .set("renamedIdentity", result.preview->renamedLuaIdentity)
                                    .set("files", std::move(files))
                                    .set("generatedResource", std::move(generatedSummary))
                                    .set("nativeRenameRequested", result.preview->nativeRenameRequested)
                                    .set("embeddedLanguageReferencesVerified", result.preview->embeddedLanguageReferencesVerified)
                                    .set("applicationSafe", result.preview->applicationSafe)
                                    .set("applicationRefusal", result.preview->applicationRefusal)
                                    .set("coverage", result.preview->supportedProfile)
                                    .set("coverageExclusions", std::move(exclusions))
                                    .set("excludedChunkCount", result.preview->coverageExclusions.size())
                                    .set("summaryComplete", summaryComplete)
                                    .set("sourceFileCount", result.preview->sourceFiles.size());
    }
    co_await capture.scheduler->schedule(sched::Lane::Background);
    if (!graph->verify(capture))
        co_return fail(ErrorCode::Busy, "Binding source/AST/schema/provider capture changed before publication");
    co_return result;
}
} // namespace frost::app::binding_work
