// SPDX-License-Identifier: Apache-2.0
// FEC-13 selected Lua-surface previews: actual AST literal edits and explicit metadata regeneration.
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/uri.hpp"

#include "binding_queries_internal.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace frost::engine {
namespace {

bool luaIdentifier(std::string_view name) {
    const auto first = [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    };
    if (name.empty() || !first(static_cast<unsigned char>(name.front())))
        return false;
    for (const char ch : name)
        if (!first(static_cast<unsigned char>(ch)) && !(ch >= '0' && ch <= '9'))
            return false;
    constexpr std::array<std::string_view, 22> keywords{
        "and", "break", "do",  "else", "elseif", "end",    "false",  "for",  "function", "goto",  "if",
        "in",  "local", "nil", "not",  "or",     "repeat", "return", "then", "true",     "until", "while"};
    return !std::ranges::contains(keywords, name);
}

Result<std::string> renamedIdentity(std::string_view identity, std::string_view oldName, std::string_view newName) {
    if (identity == oldName)
        return std::string(newName);
    if (identity.size() <= oldName.size() || !identity.ends_with(oldName))
        return fail(ErrorCode::Unsupported, "The schema identity is not a direct member-name composition");
    const auto separator = identity.size() - oldName.size() - 1;
    if (identity[separator] != '.' && identity[separator] != ':')
        return fail(ErrorCode::Unsupported, "The schema identity has no explicit member-name boundary");
    return std::string(identity.substr(0, separator + 1)) + std::string(newName);
}

const BindingDocumentCapture* findDocument(std::span<const BindingDocumentCapture> documents, std::string_view path) {
    const BindingDocumentCapture* found = nullptr;
    const auto key = lang::documentKey(path);
    for (const auto& document : documents)
        if (lang::documentKey(document.path) == key) {
            if (found)
                return nullptr;
            found = &document;
        }
    return found;
}

struct SurfacePlan {
    std::vector<BindingPreviewEdit> registrations;
    GeneratedEngineLuaMetadata regenerated;
    const BindingDocumentCapture* generatedDocument = nullptr;
    std::string oldIdentity;
    std::string newIdentity;
    std::string providerMetadataProjection;
};

Result<BindingPreviewEdit> literalEdit(const BindingNameLiteral& literal, std::string_view replacement,
                                       const BindingProviderCapture& provider,
                                       std::span<const BindingDocumentCapture> documents) {
    const BindingDocumentCapture* source = nullptr;
    for (const auto& document : documents)
        if (lang::documentKey(document.path) == lang::documentKey(literal.range.path) ||
            (!document.physicalPath.empty() &&
             lang::documentKey(document.physicalPath) == lang::documentKey(literal.range.path))) {
            if (source)
                return fail(ErrorCode::Busy, "The AST literal has ambiguous captured physical aliases");
            source = &document;
        }
    if (!source || source->role != BindingDocumentRole::UserSource)
        return fail(ErrorCode::PermissionDenied, "The AST literal has no admitted user-source capture");
    const auto& bytes = literal.range.bytes;
    if (bytes.begin >= bytes.end || bytes.end > source->snapshot.size() ||
        !source->snapshot.isCharBoundary(bytes.begin) || !source->snapshot.isCharBoundary(bytes.end) ||
        source->snapshot.text(bytes) != literal.spelling)
        return fail(ErrorCode::Busy, "The AST literal no longer matches its captured source spelling");
    if ((!literal.spelling.starts_with('"') && !literal.spelling.starts_with("R\"")) ||
        !literal.spelling.ends_with('"') || !io::isValidUtf8(replacement))
        return fail(ErrorCode::Unsupported, "This C++ literal encoding needs an explicit semantic rewrite adapter");
    std::string encoded = "\"";
    for (const char character : replacement) {
        const auto ch = static_cast<unsigned char>(character);
        if (ch == '\\' || ch == '"') {
            encoded += '\\';
            encoded += character;
        } else if (ch == '\n')
            encoded += "\\n";
        else if (ch == '\r')
            encoded += "\\r";
        else if (ch == '\t')
            encoded += "\\t";
        else if (ch < 32 || ch == 127) {
            encoded += '\\';
            encoded += static_cast<char>('0' + ((ch >> 6U) & 7U));
            encoded += static_cast<char>('0' + ((ch >> 3U) & 7U));
            encoded += static_cast<char>('0' + (ch & 7U));
        } else
            encoded += character;
    }
    // Preserve physical line breaks so source_location lines of later registrations remain exact.
    // Phase-two C++ line splicing removes these continuations without changing decoded bytes.
    for (std::size_t i = 0; i < literal.spelling.size(); ++i) {
        const char ch = literal.spelling[i];
        if (ch == '\r') {
            encoded += "\\\r";
            if (i + 1 < literal.spelling.size() && literal.spelling[i + 1] == '\n') {
                encoded += '\n';
                ++i;
            }
        } else if (ch == '\n')
            encoded += "\\\n";
    }
    encoded += '"';
    return BindingPreviewEdit{provider,
                              {source->path, literal.range.bytes},
                              source->snapshot.version(),
                              literal.spelling,
                              std::move(encoded),
                              {},
                              source->snapshot,
                              BindingDocumentRole::UserSource,
                              BindingPreviewOrigin::RegistrationLiteral};
}

Result<SurfacePlan> planSurface(const BindingIndex& index, const SelectedLuaBindingRename& selected,
                                std::span<const BindingDocumentCapture> documents) {
    const auto& seed = selected.target.seed;
    if (seed.provider.language != BindingLanguage::Lua || seed.luaQualifiedName.empty() || !seed.nativeUsr.empty() ||
        !seed.anchorSnapshot || !luaIdentifier(selected.newName))
        return fail(ErrorCode::InvalidArgument, "Select one tracked Lua binding and a valid Lua identifier");
    const auto* binding = index.findLua(seed.luaQualifiedName);
    if (!binding || binding->resolution != BindingResolution::Resolved || !binding->lua.source)
        return fail(ErrorCode::Unsupported, "The selected Lua identity has no unique semantic registration");
    const auto& oldEntry = binding->lua;
    if (selected.newName == oldEntry.name)
        return fail(ErrorCode::InvalidArgument, "The selected Lua name is unchanged");
    if (oldEntry.kind != "Function" && oldEntry.kind != "GlobalFunction" && oldEntry.kind != "Method" &&
        oldEntry.kind != "Property" && oldEntry.kind != "GlobalValue")
        return fail(ErrorCode::Unsupported, "Structural type or namespace renaming needs additional semantic planning");

    const BindingNameLiteral* literal = nullptr;
    const BindingSignatureLiteral* signatureLiteral = nullptr;
    for (const auto& mapping : binding->native) {
        // Alias components retain each mapping's original identity. Never edit the first inherited
        // target's literal when the user selected a different Lua alias.
        if (mapping.luaQualifiedName != seed.luaQualifiedName || mapping.registration != *oldEntry.source ||
            !mapping.nameLiteral)
            continue;
        if (literal && *literal != *mapping.nameLiteral)
            return fail(ErrorCode::Busy, "The selected Lua name has conflicting AST literal evidence");
        literal = &*mapping.nameLiteral;
        if (mapping.docSignatureLiteral) {
            if (signatureLiteral && *signatureLiteral != *mapping.docSignatureLiteral)
                return fail(ErrorCode::Busy, "The selected Lua binding has conflicting ScriptDoc.Signature evidence");
            signatureLiteral = &*mapping.docSignatureLiteral;
        }
    }
    if (!literal)
        return fail(ErrorCode::Unsupported, "The selected registration has no proven single-token AST name literal");
    if (literal->decodedValue != oldEntry.name)
        return fail(ErrorCode::Busy, "The AST registration literal no longer matches its captured source bytes");
    auto nameEdit = literalEdit(*literal, selected.newName, seed.provider, documents);
    if (!nameEdit)
        return std::unexpected(nameEdit.error());
    std::vector<BindingPreviewEdit> registrationEdits{std::move(*nameEdit)};

    auto newIdentity = renamedIdentity(oldEntry.qualifiedName, oldEntry.name, selected.newName);
    if (!newIdentity)
        return std::unexpected(newIdentity.error());
    auto schema = index.schema();
    for (const auto& entry : schema.luaEntries) {
        if (entry.qualifiedName == *newIdentity)
            return fail(ErrorCode::AlreadyExists, "The renamed Lua identity already exists in the schema");
    }
    auto entry = std::ranges::find(schema.luaEntries, oldEntry.qualifiedName, &SchemaLuaEntry::qualifiedName);
    if (entry == schema.luaEntries.end())
        return fail(ErrorCode::Internal, "The graph's selected identity is absent from its captured schema");
    if (!entry->fullName.empty()) {
        auto fullName = renamedIdentity(entry->fullName, entry->name, selected.newName);
        if (!fullName)
            return std::unexpected(fullName.error());
        entry->fullName = std::move(*fullName);
    }
    entry->name = selected.newName;
    entry->qualifiedName = *newIdentity;
    if (oldEntry.kind == "Function" || oldEntry.kind == "GlobalFunction" || oldEntry.kind == "Method") {
        if (!signatureLiteral || oldEntry.signature.empty() ||
            signatureLiteral->literal.decodedValue != oldEntry.signature ||
            signatureLiteral->decodedNameOffset > oldEntry.signature.size() ||
            signatureLiteral->decodedNameLength == 0 ||
            signatureLiteral->decodedNameLength > oldEntry.signature.size() - signatureLiteral->decodedNameOffset ||
            oldEntry.signature.substr(signatureLiteral->decodedNameOffset, signatureLiteral->decodedNameLength) !=
                oldEntry.name)
            return fail(ErrorCode::Unsupported, "Callable rename requires exact AST ScriptDoc.Signature name evidence");
        entry->signature = oldEntry.signature;
        entry->signature.replace(signatureLiteral->decodedNameOffset, signatureLiteral->decodedNameLength,
                                 selected.newName);
        auto signatureEdit = literalEdit(signatureLiteral->literal, entry->signature, seed.provider, documents);
        if (!signatureEdit)
            return std::unexpected(signatureEdit.error());
        registrationEdits.push_back(std::move(*signatureEdit));
    }
    const auto providerProjection = generateEngineLuaMetadata(schema).text;
    // SchemaExport.cpp DescribeLuaEntries derives sameAs from actual NativeFunction pointer
    // identity and first registration order. Renaming the primary changes this relation's target
    // spelling, never the alias name. There is no source SameAs metadata string to rewrite.
    for (auto& alias : schema.luaEntries)
        if (alias.sameAs == oldEntry.qualifiedName)
            alias.sameAs = *newIdentity;

    const auto* generated = findDocument(documents, seed.anchor.path);
    if (!generated || generated->role != BindingDocumentRole::GeneratedEngineMetadata ||
        !seed.anchorSnapshot->sharesStorageWith(generated->snapshot) ||
        seed.anchorSnapshot->version() != generated->snapshot.version())
        return fail(ErrorCode::Busy, "The selected Lua anchor is not the explicitly captured generated resource");
    const auto oldMetadata = generateEngineLuaMetadata(index.schema());
    auto anchors = captureBindingLuaAnchors(oldMetadata, generated->path, generated->snapshot);
    if (!anchors)
        return std::unexpected(anchors.error());
    const auto count = std::ranges::count_if(*anchors, [&](const auto& anchor) {
        return anchor.luaQualifiedName == seed.luaQualifiedName && !anchor.ambiguous &&
               anchor.declaration.bytes == seed.anchor.bytes;
    });
    if (count != 1)
        return fail(ErrorCode::Busy, "The selected Lua seed is not one unambiguous emitted declaration");
    auto regenerated = generateEngineLuaMetadata(schema);
    if (std::ranges::none_of(regenerated.declarations, [&](const auto& declaration) {
            return declaration.qualifiedName == *newIdentity && !declaration.ambiguous;
        }))
        return fail(ErrorCode::Busy, "The renamed Lua declaration would collide in generated metadata");
    return SurfacePlan{std::move(registrationEdits), std::move(regenerated),  generated,
                       oldEntry.qualifiedName,       std::move(*newIdentity), providerProjection};
}
} // namespace

sched::Task<Result<BindingSelectedRenamePreview>>
querySelectedBindingRenamePreview(std::shared_ptr<const BindingIndex> index, SelectedLuaBindingRename selected,
                                  std::vector<BindingRenameRequest> nativeRenames,
                                  std::vector<BindingDocumentCapture> documents, BindingQueryAdmission current,
                                  std::stop_token stop, std::vector<BindingEmbeddedLuaCapture> embeddedLua) {
    if (!index)
        co_return fail(ErrorCode::InvalidArgument, "A selected binding preview requires its captured schema graph");
    const auto* binding = index->findLua(selected.target.seed.luaQualifiedName);
    for (const auto& request : nativeRenames)
        if (!binding || request.target.seed.provider.language != BindingLanguage::Native ||
            std::ranges::none_of(binding->native,
                                 [&](const auto& mapping) { return mapping.usr == request.target.seed.nativeUsr; }))
            co_return fail(ErrorCode::InvalidArgument,
                           "A native rename target does not belong to the selected Lua binding");
    nativeRenames.push_back({selected.target, selected.newName});
    if (auto ready = binding_detail::validateRenameRequests(nativeRenames, current, stop); !ready)
        co_return std::unexpected(ready.error());
    auto plan = planSurface(*index, selected, documents);
    if (!plan)
        co_return std::unexpected(plan.error());
    auto edits = co_await binding_detail::collectRenameEdits(nativeRenames, documents, current, stop);
    if (!edits)
        co_return std::unexpected(edits.error());
    edits->insert(edits->end(), plan->registrations.begin(), plan->registrations.end());
    auto files = binding_detail::combineCapturedPreview(*edits, documents, true);
    if (!files)
        co_return std::unexpected(files.error());
    // Convert provider edits in decoded chunks back through their exact AST string tokens.
    // Never apply edits in the temporary projection or infer byte offsets through C++ escapes.
    std::vector<BindingPreviewEdit> sourceEdits;
    for (const auto& file : *files) {
        const auto chunk = std::ranges::find_if(embeddedLua, [&](const auto& item) {
            return lang::documentKey(item.projection.path) == lang::documentKey(file.path);
        });
        if (chunk == embeddedLua.end()) {
            sourceEdits.insert(sourceEdits.end(), file.edits.begin(), file.edits.end());
            continue;
        }
        if (chunk->projection.snapshot.text() != chunk->literal.decodedValue ||
            !chunk->projection.snapshot.sharesStorageWith(file.snapshot) ||
            chunk->projection.snapshot.version() != file.snapshot.version())
            co_return fail(ErrorCode::Busy, "Embedded Lua projection lost its AST decoded-text identity");
        auto edit = literalEdit(chunk->literal, file.preview, selected.target.seed.provider, documents);
        if (!edit)
            co_return std::unexpected(edit.error());
        edit->origin = BindingPreviewOrigin::EmbeddedLuaLiteral;
        sourceEdits.push_back(std::move(*edit));
    }
    files = binding_detail::combineCapturedPreview(sourceEdits, documents, true);
    if (!files)
        co_return std::unexpected(files.error());
    BindingSelectedRenamePreview result;
    result.nativeRenameRequested = nativeRenames.size() > 1;
    result.previousLuaIdentity = plan->oldIdentity;
    result.renamedLuaIdentity = plan->newIdentity;
    bool observedGenerated = false;
    for (auto& file : *files) {
        const auto* document = findDocument(documents, file.path);
        if (!document)
            co_return fail(ErrorCode::Internal, "A normalized preview lost its captured document");
        if (document->role == BindingDocumentRole::GeneratedEngineMetadata) {
            if (observedGenerated || lang::documentKey(file.path) != lang::documentKey(plan->generatedDocument->path) ||
                (file.preview != plan->regenerated.text && file.preview != plan->providerMetadataProjection))
                co_return fail(ErrorCode::Unsupported,
                               "Actual LuaLS metadata edits do not exactly match the selected schema regeneration");
            observedGenerated = true;
            result.generatedResource = {file.path, file.snapshot, std::move(plan->regenerated), std::move(file.edits),
                                        std::move(file.preview)};
        } else {
            result.sourceFiles.push_back(std::move(file));
        }
    }
    if (!observedGenerated)
        co_return fail(ErrorCode::Unsupported, "LuaLS did not return the selected generated declaration edit");
    if (auto ready = binding_detail::validateRenameRequests(nativeRenames, current, stop); !ready)
        co_return std::unexpected(ready.error());
    co_return result;
}
} // namespace frost::engine
