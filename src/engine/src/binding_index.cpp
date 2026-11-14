// SPDX-License-Identifier: Apache-2.0
// FEC-13 schema/native graph and strict read-only LibTooling boundary decoding.
#include "frost/engine/binding_index.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <utility>

namespace frost::engine {
namespace {
using rpc::Json;

bool validSource(const SchemaSource& source) {
    if (source.line == 0 || source.file.empty() || source.file.front() == '/' ||
        source.file.find('\\') != std::string::npos || source.file.find(':') != std::string::npos ||
        source.file.find('\0') != std::string::npos)
        return false;
    std::string_view remaining = source.file;
    while (!remaining.empty()) {
        const auto separator = remaining.find('/');
        const auto component = remaining.substr(0, separator);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (separator == std::string_view::npos)
            return true;
        remaining.remove_prefix(separator + 1);
    }
    return false;
}

bool validRange(const BindingSourceRange& range) {
    return io::normalizePath(range.path).has_value() && range.bytes.begin < range.bytes.end;
}

Result<BindingSourceRange> readRange(const Json* value) {
    if (!value || !value->isObject())
        return fail(ErrorCode::Corrupt, "A binding range must be an object");
    const auto offset = value->find("offset");
    const auto length = value->find("length");
    if (!offset || !length)
        return fail(ErrorCode::Corrupt, "A binding range requires offset and length");
    const auto begin = offset->asUnsigned();
    const auto count = length->asUnsigned();
    if (offset->kind() != Json::Kind::Integer || length->kind() != Json::Kind::Integer || !begin || !count ||
        *count == 0 || *count > std::numeric_limits<std::uint64_t>::max() - *begin)
        return fail(ErrorCode::Corrupt, "A binding range has an invalid byte extent");
    BindingSourceRange range{std::string(value->stringAt("path")), {ByteOffset{*begin}, ByteOffset{*begin + *count}}};
    if (!validRange(range))
        return fail(ErrorCode::Corrupt, "A binding range needs an absolute path and nonempty byte extent");
    return range;
}

std::optional<SchemaSource> readSource(const Json* value) {
    if (!value || !value->isObject())
        return {};
    const auto line = value->integerAt("line");
    const auto* lineValue = value->find("line");
    const auto file = value->stringAt("file");
    if (!lineValue || lineValue->kind() != Json::Kind::Integer || !line || *line <= 0 ||
        *line > std::numeric_limits<std::uint32_t>::max() || file.empty())
        return {};
    SchemaSource source{std::string(file), static_cast<std::uint32_t>(*line)};
    return validSource(source) ? std::optional(source) : std::nullopt;
}

bool validRole(std::string_view role) { return role == "callable" || role == "getter" || role == "setter"; }

bool sameMapping(const NativeBindingMapping& a, const NativeBindingMapping& b) {
    return a.usr == b.usr && a.role == b.role && a.declaration == b.declaration && a.definition == b.definition &&
           a.registrationRange == b.registrationRange && a.argumentRange == b.argumentRange &&
           a.nameLiteral == b.nameLiteral && a.docSignatureLiteral == b.docSignatureLiteral;
}

void appendUnique(std::vector<BindingSourceRange>& result, const BindingSourceRange& range) {
    if (std::ranges::find(result, range) == result.end())
        result.push_back(range);
}
} // namespace

Result<Json> bindingMapRequest(const SchemaDocument& schema, std::string_view root, std::string_view commands,
                               std::string_view translationUnit, std::optional<std::uint64_t> commandIndex) {
    if (!io::normalizePath(root) || !io::normalizePath(commands) || !io::normalizePath(translationUnit))
        return fail(ErrorCode::InvalidArgument, "Binding analysis requires absolute workspace, CDB and TU paths");
    Json registrations = Json::array();
    for (std::size_t index = 0; index < schema.luaEntries.size(); ++index) {
        const auto& entry = schema.luaEntries[index];
        if (!entry.source)
            continue;
        if (!validSource(*entry.source))
            return fail(ErrorCode::InvalidArgument,
                        "A schema registration source is not a safe relative source location");
        registrations.push(
            Json::object()
                .set("id", std::to_string(index))
                .set("qualifiedName", entry.qualifiedName)
                .set("name", entry.name)
                .set("kind", entry.kind)
                .set("source", Json::object().set("file", entry.source->file).set("line", entry.source->line)));
    }
    auto request = Json::object()
                       .set("schemaVersion", 1)
                       .set("workspaceRoot", std::string(root))
                       .set("compileCommandsDirectory", std::string(commands))
                       .set("translationUnit", std::string(translationUnit))
                       .set("registrations", std::move(registrations));
    if (commandIndex)
        request.set("compileCommandIndex", *commandIndex);
    return request;
}

Result<BindingMapReport> parseBindingMapReport(const Json& request, const Json& response) {
    const auto* requestVersion = request.find("schemaVersion");
    const auto* responseVersion = response.find("schemaVersion");
    if (!requestVersion || !responseVersion || requestVersion->kind() != Json::Kind::Integer ||
        responseVersion->kind() != Json::Kind::Integer || request.integerAt("schemaVersion") != 1 ||
        response.integerAt("schemaVersion") != 1)
        return fail(ErrorCode::Unsupported, "Unsupported binding analysis version");
    const auto* registrations = request.find("registrations");
    const auto* results = response.find("results");
    const auto* parsed = response.find("translationUnitParsed");
    if (!registrations || !registrations->isArray() || !results || !results->isArray() || !parsed || !parsed->isBool())
        return fail(ErrorCode::Corrupt, "Binding analysis response is incomplete");
    std::map<std::string, const Json*, std::less<>> expected;
    for (const auto& entry : registrations->items()) {
        const auto id = entry.stringAt("id");
        if (id.empty() || !readSource(entry.find("source")) || entry.stringAt("qualifiedName").empty() ||
            !expected.emplace(std::string(id), &entry).second)
            return fail(ErrorCode::Corrupt, "Invalid or duplicated binding request identity");
    }
    BindingMapReport report;
    if (response.stringAt("embeddedLuaProfile") == "Frost::ScriptEngine::ExecuteString/v1") {
        const auto* chunks = response.find("embeddedLua");
        const auto* refusals = response.find("embeddedLuaRefusals");
        if (!chunks || !chunks->isArray() || !refusals || !refusals->isArray())
            return fail(ErrorCode::Corrupt, "Missing AST embedded Lua coverage");
        report.embeddedLuaProfileVerified = parsed->asBool() == true;
        for (const auto& chunk : chunks->items()) {
            auto range = readRange(chunk.find("range"));
            const auto* decoded = chunk.find("decodedValue");
            const auto* spelling = chunk.find("spelling");
            if (!range || !decoded || !decoded->isString() || !spelling || !spelling->isString() ||
                spelling->asString()->size() != range->bytes.size())
                return fail(ErrorCode::Corrupt, "Malformed AST embedded Lua literal");
            report.embeddedLua.push_back({*range, *decoded->asString(), *spelling->asString()});
        }
        for (const auto& refusal : refusals->items()) {
            if (!refusal.isString())
                return fail(ErrorCode::Corrupt, "Malformed AST embedded Lua refusal");
            report.embeddedLuaRefusals.push_back(*refusal.asString());
        }
    }
    if (const auto* inputs = response.find("sourceInputs")) {
        if (!inputs->isArray())
            return fail(ErrorCode::Corrupt, "AST source inputs must be an array");
        std::set<std::string, std::less<>> inputPaths;
        for (const auto& input : inputs->items()) {
            const auto path = input.stringAt("path");
            const auto hash = input.stringAt("sha256");
            if (!io::normalizePath(path) || !inputPaths.insert(lang::documentKey(path)).second || hash.size() != 64 ||
                std::ranges::any_of(hash,
                                    [](char ch) { return !(ch >= '0' && ch <= '9') && !(ch >= 'a' && ch <= 'f'); }))
                return fail(ErrorCode::Corrupt, "Malformed or duplicate AST source-input digest");
            report.sourceInputs.push_back({std::string(path), std::string(hash)});
        }
    }
    std::set<std::string, std::less<>> seen;
    for (const auto& result : results->items()) {
        const std::string id(result.stringAt("id"));
        const auto it = expected.find(id);
        const auto source = readSource(result.find("source"));
        if (it == expected.end() || !seen.insert(id).second || !source ||
            source != readSource(it->second->find("source")) ||
            result.stringAt("qualifiedName") != it->second->stringAt("qualifiedName"))
            return fail(ErrorCode::Corrupt, "Binding response does not match the captured request");
        const std::string name(result.stringAt("qualifiedName"));
        const auto status = result.stringAt("status");
        if (status != "mapped" && status != "ambiguous" && status != "unavailable")
            return fail(ErrorCode::Corrupt, "Unknown binding analysis status");
        if (status != "mapped" || parsed->asBool() != true) {
            if (status == "mapped")
                return fail(ErrorCode::Corrupt, "An unparsed translation unit cannot report mapped bindings");
            report.issues.push_back(
                {name, *source, status == "ambiguous" ? BindingResolution::Ambiguous : BindingResolution::Unresolved,
                 std::string(result.stringAt("reason"))});
            continue;
        }
        auto registrationRange = readRange(result.find("registrationRange"));
        const auto* targets = result.find("targets");
        if (!registrationRange || !targets || !targets->isArray() || targets->size() == 0)
            return fail(ErrorCode::Corrupt, "A mapped binding requires actual registration and target ranges");
        std::optional<BindingNameLiteral> nameLiteral;
        if (const auto* value = result.find("nameLiteral")) {
            auto range = readRange(value->find("range"));
            const auto* decoded = value->find("decodedValue");
            const auto* spelling = value->find("spelling");
            if (!value->isObject() || !range || !decoded || !decoded->isString() || !spelling ||
                !spelling->isString() || decoded->asString()->empty() || spelling->asString()->empty() ||
                range->bytes.size() != spelling->asString()->size() ||
                *decoded->asString() != it->second->stringAt("name"))
                return fail(ErrorCode::Corrupt, "Malformed or mismatching AST registration name literal");
            nameLiteral = BindingNameLiteral{std::move(*range), *decoded->asString(), *spelling->asString()};
        }
        std::optional<BindingSignatureLiteral> signatureLiteral;
        if (const auto* value = result.find("docSignatureLiteral")) {
            auto range = readRange(value->find("range"));
            const auto* decoded = value->find("decodedValue");
            const auto* spelling = value->find("spelling");
            const auto* offset = value->find("decodedNameOffset");
            const auto* length = value->find("decodedNameLength");
            if (!value->isObject() || !range || !decoded || !decoded->isString() || !spelling ||
                !spelling->isString() || !offset || !length || offset->kind() != Json::Kind::Integer ||
                length->kind() != Json::Kind::Integer || !offset->asUnsigned() || !length->asUnsigned() ||
                *length->asUnsigned() == 0 || *offset->asUnsigned() > decoded->asString()->size() ||
                *length->asUnsigned() > decoded->asString()->size() - *offset->asUnsigned() ||
                range->bytes.size() != spelling->asString()->size() ||
                decoded->asString()->substr(*offset->asUnsigned(), *length->asUnsigned()) !=
                    it->second->stringAt("name"))
                return fail(ErrorCode::Corrupt, "Malformed AST ScriptDoc.Signature name-token evidence");
            signatureLiteral = BindingSignatureLiteral{{std::move(*range), *decoded->asString(), *spelling->asString()},
                                                       *offset->asUnsigned(),
                                                       *length->asUnsigned()};
        }
        for (const auto& target : targets->items()) {
            auto declaration = readRange(target.find("declaration"));
            auto argument = readRange(target.find("argumentRange"));
            const std::string usr(target.stringAt("usr"));
            const std::string role(target.stringAt("role"));
            const std::string kind(target.stringAt("kind"));
            if (!declaration || !argument || usr.empty() || !validRole(role) ||
                (kind != "function" && kind != "method" && kind != "lambda" && kind != "field") ||
                target.stringAt("qualifiedName").empty())
                return fail(ErrorCode::Corrupt, "Malformed semantic native target");
            std::optional<BindingSourceRange> definition;
            if (const auto* value = target.find("definition"); value && !value->isNull()) {
                auto range = readRange(value);
                if (!range)
                    return std::unexpected(range.error());
                definition = std::move(*range);
            }
            report.mappings.push_back({name, *source, usr, std::string(target.stringAt("qualifiedName")),
                                       std::move(*declaration), std::move(definition), *registrationRange, role, kind,
                                       std::move(*argument), nameLiteral,
                                       std::string(result.stringAt("nameLiteralReason")), signatureLiteral,
                                       std::string(result.stringAt("docSignatureLiteralReason"))});
        }
    }
    if (seen.size() != expected.size())
        return fail(ErrorCode::Corrupt, "Binding response omitted captured registrations");
    return report;
}

Result<BindingIndex> BindingIndex::create(std::string root, std::uint64_t revision, const SchemaDocument& schema,
                                          std::span<const NativeBindingMapping> mappings,
                                          std::span<const BindingMappingIssue> issues) {
    if (!io::normalizePath(root) || revision == 0)
        return fail(ErrorCode::InvalidArgument, "A binding graph needs an absolute root and nonzero capture revision");
    BindingIndex index;
    index.m_root = std::move(root);
    index.m_revision = revision;
    index.m_schema = schema;
    for (const auto& entry : schema.luaEntries) {
        if (entry.source && !validSource(*entry.source))
            return fail(ErrorCode::InvalidArgument,
                        "A schema registration source is not a safe relative source location");
        index.m_bindings.push_back({entry, BindingResolution::Unresolved, {}, {}, {}});
    }
    auto& bindings = index.m_bindings;
    std::vector<std::size_t> parents(bindings.size());
    std::iota(parents.begin(), parents.end(), 0);
    const auto representative = [&parents](std::size_t at) {
        auto rootIndex = at;
        while (parents[rootIndex] != rootIndex)
            rootIndex = parents[rootIndex];
        while (parents[at] != at) {
            const auto next = parents[at];
            parents[at] = rootIndex;
            at = next;
        }
        return rootIndex;
    };
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        if (bindings[i].lua.qualifiedName.empty())
            return fail(ErrorCode::InvalidArgument, "A Lua binding identity is empty");
        std::size_t count = 0;
        for (const auto& other : bindings)
            count += other.lua.qualifiedName == bindings[i].lua.qualifiedName;
        if (count != 1) {
            bindings[i].resolution = BindingResolution::Ambiguous;
            bindings[i].reason = "The schema repeats this Lua identity";
        }
        const auto& alias = bindings[i].lua.sameAs;
        if (alias.empty())
            continue;
        std::vector<std::size_t> targets;
        for (std::size_t j = 0; j < bindings.size(); ++j)
            if (bindings[j].lua.qualifiedName == alias)
                targets.push_back(j);
        if (targets.empty())
            return fail(ErrorCode::InvalidArgument, "A schema sameAs edge names no binding");
        if (targets.size() != 1) {
            bindings[i].resolution = BindingResolution::Ambiguous;
            bindings[i].reason = "The schema sameAs target is ambiguous";
            continue;
        }
        bindings[i].sameAs = targets.front();
        parents[representative(i)] = representative(targets.front());
    }
    for (const auto& mapping : mappings) {
        if (mapping.usr.empty() || mapping.qualifiedName.empty() || !validRole(mapping.role) ||
            !validRange(mapping.declaration) || !validRange(mapping.registrationRange) ||
            !validRange(mapping.argumentRange) || (mapping.definition && !validRange(*mapping.definition)))
            return fail(ErrorCode::InvalidArgument, "Malformed semantic native binding evidence");
        bool matched = false;
        for (auto& binding : bindings) {
            if (binding.lua.qualifiedName != mapping.luaQualifiedName || !binding.lua.source ||
                *binding.lua.source != mapping.registration)
                continue;
            auto path = io::joinPath(index.m_root, mapping.registration.file);
            if (!path || lang::documentKey(*path) != lang::documentKey(mapping.registrationRange.path) ||
                lang::documentKey(mapping.argumentRange.path) != lang::documentKey(mapping.registrationRange.path) ||
                mapping.argumentRange.bytes.begin < mapping.registrationRange.bytes.begin ||
                mapping.argumentRange.bytes.end > mapping.registrationRange.bytes.end)
                return fail(ErrorCode::InvalidArgument, "Native evidence does not belong to the schema registration");
            if (mapping.nameLiteral &&
                (!validRange(mapping.nameLiteral->range) ||
                 lang::documentKey(mapping.nameLiteral->range.path) !=
                     lang::documentKey(mapping.registrationRange.path) ||
                 mapping.nameLiteral->range.bytes.begin < mapping.registrationRange.bytes.begin ||
                 mapping.nameLiteral->range.bytes.end > mapping.registrationRange.bytes.end ||
                 mapping.nameLiteral->decodedValue != binding.lua.name ||
                 mapping.nameLiteral->range.bytes.size() != mapping.nameLiteral->spelling.size()))
                return fail(ErrorCode::InvalidArgument, "AST name literal does not belong to the schema registration");
            if (mapping.docSignatureLiteral) {
                const auto& signature = *mapping.docSignatureLiteral;
                const auto& literal = signature.literal;
                if (!validRange(literal.range) ||
                    lang::documentKey(literal.range.path) != lang::documentKey(mapping.registrationRange.path) ||
                    literal.range.bytes.begin < mapping.registrationRange.bytes.begin ||
                    literal.range.bytes.end > mapping.registrationRange.bytes.end ||
                    literal.range.bytes.size() != literal.spelling.size() ||
                    literal.decodedValue != binding.lua.signature ||
                    signature.decodedNameOffset > literal.decodedValue.size() || signature.decodedNameLength == 0 ||
                    signature.decodedNameLength > literal.decodedValue.size() - signature.decodedNameOffset ||
                    literal.decodedValue.substr(signature.decodedNameOffset, signature.decodedNameLength) !=
                        binding.lua.name)
                    return fail(ErrorCode::InvalidArgument,
                                "AST ScriptDoc.Signature does not match its selected schema entry");
            }
            binding.native.push_back(mapping);
            matched = true;
        }
        if (!matched)
            return fail(ErrorCode::InvalidArgument, "A native mapping names no exact schema registration");
    }
    for (const auto& issue : issues) {
        bool matched = false;
        for (auto& binding : bindings) {
            if (binding.lua.qualifiedName == issue.luaQualifiedName && binding.lua.source &&
                *binding.lua.source == issue.registration) {
                if (issue.resolution == BindingResolution::Resolved)
                    return fail(ErrorCode::InvalidArgument, "A mapping issue cannot claim a resolved target");
                if (binding.resolution != BindingResolution::Ambiguous)
                    binding.resolution = issue.resolution;
                binding.reason = issue.reason;
                matched = true;
            }
        }
        if (!matched)
            return fail(ErrorCode::InvalidArgument, "A mapping issue names no exact schema registration");
    }
    for (std::size_t rootIndex = 0; rootIndex < bindings.size(); ++rootIndex) {
        if (representative(rootIndex) != rootIndex)
            continue;
        std::vector<NativeBindingMapping> evidence;
        bool ambiguous = false;
        std::map<std::string, std::set<std::string>, std::less<>> roleUsrs;
        for (std::size_t j = 0; j < bindings.size(); ++j) {
            if (representative(j) != rootIndex)
                continue;
            ambiguous |= bindings[j].resolution == BindingResolution::Ambiguous;
            for (const auto& mapping : bindings[j].native) {
                roleUsrs[mapping.role].insert(mapping.usr);
                if (std::ranges::none_of(evidence, [&](const auto& old) { return sameMapping(old, mapping); }))
                    evidence.push_back(mapping);
            }
        }
        for (const auto& [role, usrs] : roleUsrs) {
            static_cast<void>(role);
            ambiguous |= usrs.size() > 1;
        }
        for (std::size_t j = 0; j < bindings.size(); ++j) {
            if (representative(j) != rootIndex)
                continue;
            auto& binding = bindings[j];
            binding.native = evidence;
            binding.resolution = ambiguous          ? BindingResolution::Ambiguous
                                 : evidence.empty() ? BindingResolution::Unresolved
                                                    : BindingResolution::Resolved;
            if (ambiguous && binding.reason.empty())
                binding.reason = "Conflicting or ambiguous semantic binding evidence";
            else if (evidence.empty() && binding.reason.empty())
                binding.reason = "No semantic native registration target was reported";
            else if (!ambiguous && !evidence.empty())
                binding.reason.clear();
        }
    }
    return index;
}

const IndexedBinding* BindingIndex::findLua(std::string_view name) const noexcept {
    const IndexedBinding* result = nullptr;
    for (const auto& binding : m_bindings) {
        if (binding.lua.qualifiedName != name)
            continue;
        if (result)
            return nullptr;
        result = &binding;
    }
    return result;
}

std::vector<const IndexedBinding*> BindingIndex::forNative(std::string_view usr) const {
    std::vector<const IndexedBinding*> result;
    if (usr.empty())
        return result;
    for (const auto& binding : m_bindings)
        if (binding.resolution == BindingResolution::Resolved &&
            std::ranges::any_of(binding.native, [&](const auto& mapping) { return mapping.usr == usr; }))
            result.push_back(&binding);
    return result;
}

Result<std::vector<BindingSourceRange>> BindingIndex::nativeTargets(std::string_view name) const {
    const auto* binding = findLua(name);
    if (!binding)
        return fail(ErrorCode::NotFound, "No unique schema binding has this Lua identity");
    if (binding->resolution != BindingResolution::Resolved)
        return fail(binding->resolution == BindingResolution::Ambiguous ? ErrorCode::Busy : ErrorCode::NotFound,
                    binding->reason);
    std::vector<BindingSourceRange> result;
    for (const auto& mapping : binding->native) {
        appendUnique(result, mapping.declaration);
        if (mapping.definition)
            appendUnique(result, *mapping.definition);
        appendUnique(result, mapping.registrationRange);
    }
    return result;
}
} // namespace frost::engine
