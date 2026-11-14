// SPDX-License-Identifier: Apache-2.0
// FEC-13: immutable schema-to-native binding graph, built only from explicit schema/AST evidence.
#pragma once

#include "frost/engine/schema.hpp"
#include "frost/text/positions.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::engine {

// A half-open UTF-8 byte range emitted by LibTooling, or tracked while generating Lua metadata.
// AST producer ranges retain verified absolute physical paths. Provider/editor ranges retain
// the captured document spelling; their separate physical receipt lives in BindingDocumentCapture.
// Never rewrite immutable AST/index provenance when joining it to a provider document.
struct BindingSourceRange {
    std::string path;
    text::ByteRange bytes;
    friend bool operator==(const BindingSourceRange&, const BindingSourceRange&) = default;
};

struct BindingNameLiteral {
    BindingSourceRange range; // Whole C++ string-literal token, including its delimiters.
    std::string decodedValue; // Actual AST StringLiteral bytes, never decoded by searching source.
    std::string spelling;     // Exact original token bytes from the mapper's SourceManager.
    friend bool operator==(const BindingNameLiteral&, const BindingNameLiteral&) = default;
};
struct BindingSignatureLiteral {
    BindingNameLiteral literal;
    std::uint64_t decodedNameOffset = 0, decodedNameLength = 0;
    friend bool operator==(const BindingSignatureLiteral&, const BindingSignatureLiteral&) = default;
};

// Evidence from an actual AST registration argument, not a symbol search by spelling. The schema
// registration must match exactly. Multiple distinct USRs for one role remain ambiguous.
struct NativeBindingMapping {
    std::string luaQualifiedName;
    SchemaSource registration;
    std::string usr;
    std::string qualifiedName;
    BindingSourceRange declaration; // Name-token selection; lambda has only its actual AST range (navigation only).
    std::optional<BindingSourceRange> definition;
    BindingSourceRange registrationRange;
    std::string role = "callable"; // "callable", "getter" or "setter"; distinct property accessors stay distinct.
    std::string kind;              // The tool's actual function/method/lambda/field kind.
    BindingSourceRange argumentRange;
    std::optional<BindingNameLiteral> nameLiteral = std::nullopt;
    std::string nameLiteralReason = {};
    std::optional<BindingSignatureLiteral> docSignatureLiteral = std::nullopt;
    std::string docSignatureLiteralReason = {};
};

enum class BindingResolution : std::uint8_t { Unresolved, Resolved, Ambiguous };

struct BindingMappingIssue {
    std::string luaQualifiedName;
    SchemaSource registration;
    BindingResolution resolution = BindingResolution::Unresolved;
    std::string reason;
};

struct BindingMapReport {
    struct SourceInput {
        std::string path;
        std::string sha256; // Actual parsed SourceManager buffer digest, not a post-parse disk reread.
    };
    bool embeddedLuaProfileVerified = false;
    std::vector<BindingNameLiteral> embeddedLua;
    std::vector<std::string> embeddedLuaRefusals;
    std::vector<NativeBindingMapping> mappings;
    std::vector<BindingMappingIssue> issues;
    std::vector<SourceInput> sourceInputs;
};

// Builds the read-only frost-clang --binding-map request. IDs encode schema entry indexes and are
// opaque to the tool. Entries without C++ registration sources are omitted. Pure; O(entries).
// Errors: InvalidArgument for relative root/CDB/TU paths.
[[nodiscard]] Result<rpc::Json> bindingMapRequest(const SchemaDocument& schema, std::string_view engineRoot,
                                                  std::string_view compileCommandsDirectory,
                                                  std::string_view translationUnit,
                                                  std::optional<std::uint64_t> compileCommandIndex = {});
// Strictly reads the actual tool response against the exact request (including IDs/source/name).
// Errors: Corrupt for malformed, duplicate/missing/foreign results; Unsupported for another version.
// An unparsed TU or ambiguous/unavailable result preserves an issue and yields no usable edge.
[[nodiscard]] Result<BindingMapReport> parseBindingMapReport(const rpc::Json& request, const rpc::Json& response);

struct IndexedBinding {
    SchemaLuaEntry lua;
    BindingResolution resolution = BindingResolution::Unresolved;
    std::vector<NativeBindingMapping> native;
    // Exact schema sameAs target. It is a relation for navigation/usages, never rename consent.
    std::optional<std::size_t> sameAs;
    std::string reason;
};

/*
 * Pure, owning graph for one captured schema/native-analysis revision. No I/O, Qt or mutable cache.
 * Const queries can run concurrently. Producers must bind schema, AST output and file snapshots
 * to the same project revision; callers discard the graph when any of those captures changes.
 * Duplicate Lua identities and conflicting native USRs for one role stay ambiguous, never guessed.
 * Explicit sameAs edges can carry a unique native mapping to aliases, including through chains;
 * aliases retain their own Lua identities. No lexical spelling creates a cross-language edge.
 */
class BindingIndex {
public:
    // Errors: InvalidArgument for a relative root, zero revision or malformed/unmatched mapping.
    // O((entries + mappings)^2); intended for schema-sized binding catalogs, off the GUI thread.
    [[nodiscard]] static Result<BindingIndex> create(std::string engineRoot, std::uint64_t revision,
                                                     const SchemaDocument& schema,
                                                     std::span<const NativeBindingMapping> mappings,
                                                     std::span<const BindingMappingIssue> issues = {});

    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }
    [[nodiscard]] std::string_view engineRoot() const noexcept { return m_root; }
    [[nodiscard]] const SchemaDocument& schema() const noexcept { return m_schema; }
    [[nodiscard]] std::span<const IndexedBinding> bindings() const noexcept { return m_bindings; }
    // O(entries). Empty or duplicate identities return nullptr (never the first arbitrary entry).
    [[nodiscard]] const IndexedBinding* findLua(std::string_view qualifiedName) const noexcept;
    // All resolved Lua identities proven to reach exactly this USR, in schema order. O(entries).
    [[nodiscard]] std::vector<const IndexedBinding*> forNative(std::string_view usr) const;
    // Registration sites and native declaration/definition targets for this exact Lua identity.
    // Errors: NotFound for missing/unresolved, Busy for ambiguous. Does not follow text matches.
    [[nodiscard]] Result<std::vector<BindingSourceRange>> nativeTargets(std::string_view qualifiedName) const;

private:
    std::string m_root;
    std::uint64_t m_revision = 0;
    std::vector<IndexedBinding> m_bindings;
    SchemaDocument m_schema;
};

} // namespace frost::engine
