// SPDX-License-Identifier: Apache-2.0
// FEC-13: provider-specific position conversion, actual LSP queries and immutable rename previews.
#include "frost/engine/binding_queries.hpp"

#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/uri.hpp"

#include "binding_queries_internal.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <tuple>
#include <type_traits>
#include <utility>

namespace frost::engine {
namespace {
using rpc::Json;

bool validProvider(const BindingProviderCapture& provider) {
    return !provider.id.empty() && provider.generation > 0 &&
           (provider.language == BindingLanguage::Native || provider.language == BindingLanguage::Lua) &&
           (provider.encoding == lang::PositionEncoding::Utf8 || provider.encoding == lang::PositionEncoding::Utf16);
}

Result<void> validateDocuments(std::span<const BindingDocumentCapture> documents) {
    std::map<std::string, std::size_t, std::less<>> seen;
    std::size_t identity = 0;
    for (const auto& document : documents) {
        if (!io::normalizePath(document.path) ||
            (!document.physicalPath.empty() && !io::normalizePath(document.physicalPath)))
            return fail(ErrorCode::InvalidArgument, "Captured binding documents need absolute paths");
        for (const auto& path : {document.path, document.physicalPath}) {
            if (path.empty())
                continue;
            auto [at, inserted] = seen.emplace(lang::documentKey(path), identity);
            if (!inserted && at->second != identity)
                return fail(ErrorCode::InvalidArgument, "Captured binding document aliases are ambiguous");
        }
        ++identity;
    }
    return {};
}

const BindingDocumentCapture* documentFor(std::span<const BindingDocumentCapture> documents, std::string_view path) {
    const auto key = lang::documentKey(path);
    for (const auto& document : documents)
        if (lang::documentKey(document.path) == key)
            return &document;
    return nullptr;
}

std::optional<lang::LspRange> readLspRange(const Json* value) {
    if (!value || !value->isObject())
        return {};
    for (std::string_view key : {"start", "end"}) {
        const auto* position = value->find(key);
        if (!position || !position->isObject())
            return {};
        for (std::string_view part : {"line", "character"}) {
            const auto* number = position->find(part);
            if (!number || number->kind() != Json::Kind::Integer || !number->asUnsigned() ||
                *number->asUnsigned() > std::numeric_limits<std::uint32_t>::max())
                return {};
        }
    }
    return lang::parseRange(*value);
}

Result<text::ByteRange> strictRange(const text::Snapshot& snapshot, const lang::LspRange& range,
                                    lang::PositionEncoding encoding) {
    if (range.end < range.start)
        return fail(ErrorCode::Corrupt, "A language-server range is reversed");
    auto begin = lang::fromLspPosition(snapshot, range.start, encoding);
    auto end = lang::fromLspPosition(snapshot, range.end, encoding);
    if (!begin || !end)
        return fail(ErrorCode::OutOfRange, "A language-server range is outside the captured document");
    // Snapshot conversion deliberately clamps normal LSP navigation. A refactoring preview must
    // reject rather than reinterpret stale columns or a split UTF-8/UTF-16 character.
    if (lang::toLspPosition(snapshot, *begin, encoding) != range.start ||
        lang::toLspPosition(snapshot, *end, encoding) != range.end)
        return fail(ErrorCode::OutOfRange, "A language-server range does not exactly address captured text");
    return text::ByteRange{*begin, *end};
}

Result<void> admitted(const BindingProviderRequest& request, const BindingQueryAdmission& current,
                      std::stop_token stop) {
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "The binding query was cancelled");
    if (!request.requester || !validProvider(request.seed.provider) || !current)
        return fail(ErrorCode::InvalidArgument, "A binding query requires a provider and current-capture admission");
    if (!current(request.seed.provider) || request.requester->encoding() != request.seed.provider.encoding)
        return fail(ErrorCode::Cancelled, "The binding query's project, documents or language server changed");
    return {};
}

Result<Json> queryParams(const BindingQuerySeed& seed, std::span<const BindingDocumentCapture> documents) {
    const BindingDocumentCapture* document = nullptr;
    for (const auto& candidate : documents)
        if (lang::documentKey(candidate.path) == lang::documentKey(seed.anchor.path) ||
            (!candidate.physicalPath.empty() &&
             lang::documentKey(candidate.physicalPath) == lang::documentKey(seed.anchor.path))) {
            if (document)
                return fail(ErrorCode::InvalidArgument, "The semantic anchor has ambiguous document aliases");
            document = &candidate;
        }
    if (!document)
        return fail(ErrorCode::NotFound, "The semantic binding anchor has no captured document");
    if (seed.provider.language == BindingLanguage::Lua && !seed.anchorSnapshot)
        return fail(ErrorCode::InvalidArgument, "A Lua binding seed requires its tracked declaration snapshot");
    if (seed.anchorSnapshot && (!seed.anchorSnapshot->sharesStorageWith(document->snapshot) ||
                                seed.anchorSnapshot->version() != document->snapshot.version()))
        return fail(ErrorCode::Busy, "The binding anchor belongs to a different document capture");
    const auto& bytes = seed.anchor.bytes;
    if (bytes.begin >= bytes.end || bytes.end > document->snapshot.size() ||
        !document->snapshot.isCharBoundary(bytes.begin) || !document->snapshot.isCharBoundary(bytes.end))
        return fail(ErrorCode::OutOfRange, "The semantic binding anchor is outside its captured document");
    if ((seed.provider.language == BindingLanguage::Native &&
         (seed.nativeUsr.empty() || !seed.luaQualifiedName.empty())) ||
        (seed.provider.language == BindingLanguage::Lua && (seed.luaQualifiedName.empty() || !seed.nativeUsr.empty())))
        return fail(ErrorCode::InvalidArgument, "A binding query needs its provider's exact semantic identity");
    return lang::textDocumentPosition(document->path,
                                      lang::toLspPosition(document->snapshot, bytes.begin, seed.provider.encoding));
}

sched::Task<Result<void>> verifyNativeIdentity(const BindingProviderRequest& request, const Json& params,
                                               const BindingQueryAdmission& current, std::stop_token stop) {
    if (request.seed.provider.language != BindingLanguage::Native)
        co_return Result<void>{};
    if (auto ready = admitted(request, current, stop); !ready)
        co_return std::unexpected(ready.error());
    auto raw = co_await request.requester->request("textDocument/symbolInfo", params, stop);
    if (auto ready = admitted(request, current, stop); !ready)
        co_return std::unexpected(ready.error());
    if (!raw)
        co_return std::unexpected(raw.error());
    // Clangd's extension returns SymbolDetails[]. A containing function, overload candidates or
    // an unnamed-lambda range can never borrow the requested USR because its spelling is close.
    if (!raw->isArray() || raw->size() != 1 || !raw->items().front().isObject() ||
        raw->items().front().stringAt("usr") != request.seed.nativeUsr)
        co_return fail(ErrorCode::Unsupported,
                       "Clangd did not confirm this candidate position as the exact native binding USR");
    co_return Result<void>{};
}

bool sameLocation(const BindingSourceRange& a, const BindingSourceRange& b) {
    return lang::documentKey(a.path) == lang::documentKey(b.path) && a.bytes == b.bytes;
}

Result<void> appendEdits(std::vector<BindingPreviewEdit>& result, const BindingProviderCapture& provider,
                         std::string_view uri, const Json& rawEdits, const Json* version,
                         std::span<const BindingDocumentCapture> documents) {
    auto path = lang::uriToPath(uri);
    if (!path || !rawEdits.isArray())
        return fail(ErrorCode::Corrupt, "A rename result requires file URIs and arrays of text edits");
    const auto* document = documentFor(documents, *path);
    if (!document)
        return fail(ErrorCode::NotFound, "A rename result names a document absent from the capture");
    if (document->role != BindingDocumentRole::UserSource &&
        document->role != BindingDocumentRole::GeneratedEngineMetadata)
        return fail(ErrorCode::PermissionDenied, "A rename result targets an external or unclassified document");
    if (version && !version->isNull()) {
        if (version->kind() != Json::Kind::Integer || !version->asInteger() || !document->lspVersion ||
            version->asInteger() != document->lspVersion)
            return fail(ErrorCode::Busy, "A rename result was computed for another document version");
    }
    for (const auto& rawEdit : rawEdits.items()) {
        const auto* range = rawEdit.find("range");
        const auto* text = rawEdit.find("newText");
        const auto parsed = readLspRange(range);
        if (!rawEdit.isObject() || !parsed || !text || !text->isString() || !io::isValidUtf8(*text->asString()))
            return fail(ErrorCode::Corrupt, "A rename result contains a malformed text edit");
        auto bytes = strictRange(document->snapshot, *parsed, provider.encoding);
        if (!bytes)
            return std::unexpected(bytes.error());
        result.push_back({provider,
                          {document->path, *bytes},
                          document->snapshot.version(),
                          document->snapshot.text(*bytes),
                          *text->asString(),
                          {},
                          document->snapshot,
                          document->role});
    }
    return {};
}

template<class Request>
Result<void> validateAll(const std::vector<Request>& requests, const BindingQueryAdmission& current,
                         std::stop_token stop) {
    if (requests.empty() || !current)
        return fail(ErrorCode::InvalidArgument, "A binding query requires explicit targets and capture admission");
    for (const auto& request : requests) {
        if constexpr (std::is_same_v<Request, BindingProviderRequest>) {
            if (auto result = admitted(request, current, stop); !result)
                return result;
        } else {
            if (auto result = admitted(request.target, current, stop); !result)
                return result;
        }
    }
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "The binding query was cancelled");
    return {};
}
} // namespace

Result<std::vector<LuaBindingAnchor>> captureBindingLuaAnchors(const GeneratedEngineLuaMetadata& metadata,
                                                               std::string path, const text::Snapshot& snapshot) {
    if (!io::normalizePath(path) || snapshot.text() != metadata.text)
        return fail(ErrorCode::InvalidArgument, "Lua binding anchors require the exact generated metadata capture");
    std::vector<LuaBindingAnchor> result;
    for (const auto& declaration : metadata.declarations) {
        if (declaration.qualifiedName.empty() || declaration.selection.begin >= declaration.selection.end ||
            declaration.selection.end > snapshot.size() || !snapshot.isCharBoundary(declaration.selection.begin) ||
            !snapshot.isCharBoundary(declaration.selection.end))
            return fail(ErrorCode::Corrupt, "A generated Lua declaration has an invalid captured range");
        result.push_back({declaration.qualifiedName, {path, declaration.selection}, declaration.ambiguous, snapshot});
    }
    return result;
}

Result<std::vector<BindingQuerySeed>> bindingUsageSeeds(const BindingIndex& index, std::string_view usr,
                                                        const BindingProviderCapture& native,
                                                        const BindingProviderCapture& lua,
                                                        std::span<const LuaBindingAnchor> anchors) {
    if (!validProvider(native) || !validProvider(lua) || native.language != BindingLanguage::Native ||
        lua.language != BindingLanguage::Lua)
        return fail(ErrorCode::InvalidArgument, "Combined binding queries require captured native and Lua providers");
    const auto bindings = index.forNative(usr);
    if (bindings.empty())
        return fail(ErrorCode::NotFound, "No unambiguous schema binding maps this native USR");
    std::vector<BindingQuerySeed> seeds;
    for (const auto* binding : bindings) {
        for (const auto& mapping : binding->native) {
            if (mapping.usr != usr)
                continue;
            if (std::ranges::none_of(seeds, [&](const auto& old) {
                    return old.provider.language == BindingLanguage::Native &&
                           sameLocation(old.anchor, mapping.declaration);
                }))
                seeds.push_back({native, mapping.declaration, {}, std::string(usr)});
        }
        const LuaBindingAnchor* anchor = nullptr;
        for (const auto& candidate : anchors) {
            if (candidate.luaQualifiedName != binding->lua.qualifiedName)
                continue;
            if (candidate.ambiguous)
                return fail(ErrorCode::Busy, "The generated Lua declaration has a schema or emitted-name collision");
            if (anchor && !sameLocation(anchor->declaration, candidate.declaration))
                return fail(ErrorCode::Busy, "A Lua binding has multiple tracked semantic declarations");
            if (anchor && (!anchor->capturedSnapshot || !candidate.capturedSnapshot ||
                           !anchor->capturedSnapshot->sharesStorageWith(*candidate.capturedSnapshot) ||
                           anchor->capturedSnapshot->version() != candidate.capturedSnapshot->version()))
                return fail(ErrorCode::Busy, "A Lua binding declaration belongs to conflicting captures");
            anchor = &candidate;
        }
        if (!anchor)
            return fail(ErrorCode::NotFound, "A bound Lua identity has no tracked semantic declaration");
        for (const auto& other : anchors)
            if (other.luaQualifiedName != anchor->luaQualifiedName &&
                sameLocation(other.declaration, anchor->declaration))
                return fail(ErrorCode::Busy, "Different Lua identities share one tracked declaration");
        if (!io::normalizePath(anchor->declaration.path) ||
            anchor->declaration.bytes.begin >= anchor->declaration.bytes.end || !anchor->capturedSnapshot ||
            anchor->declaration.bytes.end > anchor->capturedSnapshot->size() ||
            !anchor->capturedSnapshot->isCharBoundary(anchor->declaration.bytes.begin) ||
            !anchor->capturedSnapshot->isCharBoundary(anchor->declaration.bytes.end))
            return fail(ErrorCode::InvalidArgument, "A tracked Lua declaration has an invalid range");
        seeds.push_back({lua, anchor->declaration, binding->lua.qualifiedName, {}, anchor->capturedSnapshot});
    }
    return seeds;
}

Result<std::vector<BindingUsage>> normalizeBindingReferences(const BindingProviderCapture& provider, const Json& raw,
                                                             std::span<const BindingDocumentCapture> documents) {
    if (!validProvider(provider))
        return fail(ErrorCode::InvalidArgument, "Missing captured language-server identity");
    if (auto valid = validateDocuments(documents); !valid)
        return std::unexpected(valid.error());
    std::vector<BindingUsage> result;
    if (raw.isNull())
        return result;
    if (!raw.isArray())
        return fail(ErrorCode::Corrupt, "A references result must be an array or null");
    for (const auto& entry : raw.items()) {
        auto location = lang::parseLocation(entry);
        const auto* rawRange = entry.find("range");
        if (!rawRange)
            rawRange = entry.find("targetSelectionRange");
        if (!rawRange)
            rawRange = entry.find("targetRange");
        if (!location || !readLspRange(rawRange))
            return fail(ErrorCode::Corrupt, "A references result contains an invalid location");
        const auto* document = documentFor(documents, location->path);
        if (!document)
            return fail(ErrorCode::NotFound, "A reference names a document absent from the capture");
        auto bytes = strictRange(document->snapshot, location->range, provider.encoding);
        if (!bytes)
            return std::unexpected(bytes.error());
        BindingSourceRange range{document->path, *bytes};
        if (std::ranges::none_of(result, [&](const auto& old) { return sameLocation(old.location, range); }))
            result.push_back({provider, std::move(range), document->snapshot.version(), document->snapshot.text(*bytes),
                              document->snapshot});
    }
    return result;
}

Result<std::vector<BindingPreviewEdit>> normalizeBindingRename(const BindingProviderCapture& provider, const Json& raw,
                                                               std::span<const BindingDocumentCapture> documents) {
    if (!validProvider(provider))
        return fail(ErrorCode::InvalidArgument, "Missing captured language-server identity");
    if (auto valid = validateDocuments(documents); !valid)
        return std::unexpected(valid.error());
    std::vector<BindingPreviewEdit> result;
    if (raw.isNull())
        return result;
    if (!raw.isObject())
        return fail(ErrorCode::Corrupt, "A rename result must be a workspace edit or null");
    const auto* changes = raw.find("changes");
    const auto* documentsChanged = raw.find("documentChanges");
    if (changes && documentsChanged)
        return fail(ErrorCode::Unsupported, "A combined changes/documentChanges rename requires disambiguation");
    if (changes) {
        if (!changes->isObject())
            return fail(ErrorCode::Corrupt, "Rename changes must be an object");
        for (std::size_t i = 0; i < changes->size(); ++i)
            if (auto added = appendEdits(result, provider, changes->keys()[i], changes->items()[i], nullptr, documents);
                !added)
                return std::unexpected(added.error());
    }
    if (documentsChanged) {
        if (!documentsChanged->isArray())
            return fail(ErrorCode::Corrupt, "Rename documentChanges must be an array");
        for (const auto& change : documentsChanged->items()) {
            if (change.find("kind"))
                return fail(ErrorCode::Unsupported, "Binding rename preview does not support file operations");
            const auto* document = change.find("textDocument");
            const auto* edits = change.find("edits");
            if (!document || !document->isObject() || !edits)
                return fail(ErrorCode::Corrupt, "Malformed rename document change");
            if (auto added = appendEdits(result, provider, document->stringAt("uri"), *edits, document->find("version"),
                                         documents);
                !added)
                return std::unexpected(added.error());
        }
    }
    return result;
}

Result<std::vector<BindingPreviewFile>>
binding_detail::combineCapturedPreview(std::span<const BindingPreviewEdit> edits,
                                       std::span<const BindingDocumentCapture> documents, bool allowGenerated) {
    if (auto valid = validateDocuments(documents); !valid)
        return std::unexpected(valid.error());
    std::map<std::string, BindingPreviewFile, std::less<>> files;
    for (const auto& edit : edits) {
        const auto* document = documentFor(documents, edit.location.path);
        if (!document || document->snapshot.version() != edit.snapshotVersion ||
            !document->snapshot.sharesStorageWith(edit.capturedSnapshot) ||
            edit.capturedSnapshot.version() != edit.snapshotVersion)
            return fail(ErrorCode::Busy, "A preview edit belongs to a different document capture");
        if (edit.resourceRole != document->role ||
            (document->role != BindingDocumentRole::UserSource &&
             !(document->role == BindingDocumentRole::GeneratedEngineMetadata && allowGenerated)))
            return fail(ErrorCode::PermissionDenied,
                        "Generated metadata requires an explicit regeneration preview; external edits are refused");
        const auto& range = edit.location.bytes;
        if (!validProvider(edit.provider) || range.begin > range.end || range.end > document->snapshot.size() ||
            !document->snapshot.isCharBoundary(range.begin) || !document->snapshot.isCharBoundary(range.end) ||
            document->snapshot.text(range) != edit.before || !io::isValidUtf8(edit.after))
            return fail(ErrorCode::Corrupt, "A preview edit does not match its captured bytes");
        const auto key = lang::documentKey(document->path);
        auto [it, inserted] = files.try_emplace(key, BindingPreviewFile{document->path, document->snapshot, {}, {}});
        static_cast<void>(inserted);
        it->second.edits.push_back(edit);
    }
    std::vector<BindingPreviewFile> result;
    for (auto& [key, file] : files) {
        static_cast<void>(key);
        std::ranges::sort(file.edits, [](const auto& a, const auto& b) {
            return std::tie(a.location.bytes.begin, a.location.bytes.end) <
                   std::tie(b.location.bytes.begin, b.location.bytes.end);
        });
        std::vector<BindingPreviewEdit> unique;
        for (auto& edit : file.edits) {
            if (!unique.empty()) {
                auto& previous = unique.back();
                if (previous.location.bytes == edit.location.bytes && previous.after == edit.after) {
                    if (previous.origin != edit.origin &&
                        std::ranges::find(previous.corroboratingOrigins, edit.origin) ==
                            previous.corroboratingOrigins.end())
                        previous.corroboratingOrigins.push_back(edit.origin);
                    if (previous.provider != edit.provider &&
                        std::ranges::find(previous.corroboratingProviders, edit.provider) ==
                            previous.corroboratingProviders.end())
                        previous.corroboratingProviders.push_back(edit.provider);
                    continue;
                }
                if (edit.location.bytes.begin < previous.location.bytes.end ||
                    edit.location.bytes.begin == previous.location.bytes.begin)
                    return fail(ErrorCode::Busy, "Language servers proposed conflicting binding rename edits");
            }
            unique.push_back(std::move(edit));
        }
        file.edits = std::move(unique);
        ByteOffset cursor{0};
        for (const auto& edit : file.edits) {
            file.preview += file.snapshot.text({cursor, edit.location.bytes.begin});
            file.preview += edit.after;
            cursor = edit.location.bytes.end;
        }
        file.preview += file.snapshot.text({cursor, file.snapshot.size()});
        result.push_back(std::move(file));
    }
    return result;
}

Result<std::vector<BindingPreviewFile>> combineBindingPreview(std::span<const BindingPreviewEdit> edits,
                                                              std::span<const BindingDocumentCapture> documents) {
    return binding_detail::combineCapturedPreview(edits, documents, false);
}

sched::Task<Result<std::vector<BindingUsage>>> queryBindingUsages(std::vector<BindingProviderRequest> requests,
                                                                  std::vector<BindingDocumentCapture> documents,
                                                                  BindingQueryAdmission current, std::stop_token stop) {
    if (auto valid = validateDocuments(documents); !valid)
        co_return std::unexpected(valid.error());
    std::vector<BindingUsage> result;
    for (const auto& request : requests) {
        if (auto ready = validateAll(requests, current, stop); !ready)
            co_return std::unexpected(ready.error());
        auto params = queryParams(request.seed, documents);
        if (!params)
            co_return std::unexpected(params.error());
        if (auto verified = co_await verifyNativeIdentity(request, *params, current, stop); !verified)
            co_return std::unexpected(verified.error());
        if (auto ready = validateAll(requests, current, stop); !ready)
            co_return std::unexpected(ready.error());
        params->set("context", Json::object().set("includeDeclaration", true));
        auto raw = co_await request.requester->request("textDocument/references", std::move(*params), stop);
        if (auto ready = validateAll(requests, current, stop); !ready)
            co_return std::unexpected(ready.error());
        if (!raw)
            co_return std::unexpected(raw.error());
        auto usages = normalizeBindingReferences(request.seed.provider, *raw, documents);
        if (!usages)
            co_return std::unexpected(usages.error());
        for (auto& usage : *usages)
            if (std::ranges::none_of(result, [&](const auto& old) {
                    return old.provider == usage.provider && sameLocation(old.location, usage.location);
                }))
                result.push_back(std::move(usage));
    }
    if (auto ready = validateAll(requests, current, stop); !ready)
        co_return std::unexpected(ready.error());
    co_return result;
}

Result<void> binding_detail::validateRenameRequests(const std::vector<BindingRenameRequest>& requests,
                                                    const BindingQueryAdmission& current, std::stop_token stop) {
    return validateAll(requests, current, stop);
}

sched::Task<Result<std::vector<BindingPreviewEdit>>>
binding_detail::collectRenameEdits(std::vector<BindingRenameRequest> requests,
                                   std::vector<BindingDocumentCapture> documents, BindingQueryAdmission current,
                                   std::stop_token stop) {
    if (auto valid = validateDocuments(documents); !valid)
        co_return std::unexpected(valid.error());
    std::vector<BindingPreviewEdit> edits;
    for (const auto& request : requests) {
        if (auto ready = validateAll(requests, current, stop); !ready)
            co_return std::unexpected(ready.error());
        if (request.newName.empty() || !io::isValidUtf8(request.newName))
            co_return fail(ErrorCode::InvalidArgument, "A binding rename requires an explicit valid new name");
        auto params = queryParams(request.target.seed, documents);
        if (!params)
            co_return std::unexpected(params.error());
        if (auto verified = co_await verifyNativeIdentity(request.target, *params, current, stop); !verified)
            co_return std::unexpected(verified.error());
        if (auto ready = validateAll(requests, current, stop); !ready)
            co_return std::unexpected(ready.error());
        params->set("newName", request.newName);
        auto raw = co_await request.target.requester->request("textDocument/rename", std::move(*params), stop);
        if (auto ready = validateAll(requests, current, stop); !ready)
            co_return std::unexpected(ready.error());
        if (!raw)
            co_return std::unexpected(raw.error());
        auto normalized = normalizeBindingRename(request.target.seed.provider, *raw, documents);
        if (!normalized)
            co_return std::unexpected(normalized.error());
        if (normalized->empty())
            co_return fail(ErrorCode::Unsupported,
                           "The language server did not return edits for an explicitly selected rename");
        edits.insert(edits.end(), std::make_move_iterator(normalized->begin()),
                     std::make_move_iterator(normalized->end()));
    }
    if (auto ready = validateAll(requests, current, stop); !ready)
        co_return std::unexpected(ready.error());
    co_return edits;
}

sched::Task<Result<std::vector<BindingPreviewFile>>>
queryBindingRenamePreview(std::vector<BindingRenameRequest> requests, std::vector<BindingDocumentCapture> documents,
                          BindingQueryAdmission current, std::stop_token stop) {
    auto edits = co_await binding_detail::collectRenameEdits(requests, documents, current, stop);
    if (!edits)
        co_return std::unexpected(edits.error());
    auto combined = combineBindingPreview(*edits, documents);
    if (auto ready = validateAll(requests, current, stop); !ready)
        co_return std::unexpected(ready.error());
    co_return combined;
}
} // namespace frost::engine
