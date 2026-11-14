// SPDX-License-Identifier: Apache-2.0
// FEC-13: raw language-server usages and read-only combined rename previews over captured text.
#pragma once

#include "frost/engine/binding_index.hpp"
#include "frost/engine/engine_lua.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/text/snapshot.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>

namespace frost::engine {

enum class BindingLanguage : std::uint8_t { Native, Lua };

// Captured before dispatch, including the provider's own negotiated encoding. Generation changes
// invalidate replies even when a replacement server happens to use the same encoding.
struct BindingProviderCapture {
    BindingLanguage language = BindingLanguage::Native;
    std::string id;
    std::uint64_t generation = 0;
    lang::PositionEncoding encoding = lang::PositionEncoding::Utf16;
    friend bool operator==(const BindingProviderCapture&, const BindingProviderCapture&) = default;
};

enum class BindingDocumentRole : std::uint8_t { ExternalReadOnly, UserSource, GeneratedEngineMetadata };

struct BindingDocumentCapture {
    std::string path;
    text::Snapshot snapshot;
    std::optional<std::int64_t> lspVersion;
    // The producer physically verifies UserSource belongs to the admitted workspace. Merely
    // capturing an arbitrary external file never grants rename permission.
    BindingDocumentRole role = BindingDocumentRole::ExternalReadOnly;
    // Immutable worker-resolved admission receipt. path stays the exact provider/document spelling.
    // Empty permits identity-only pure callers; filesystem-admitted producers must populate it.
    // The producer revalidates the original spelling; this observation grants no retained authority.
    std::string physicalPath{};
};

// The generator records this range while emitting that EXACT schema entry, or the caller verifies
// a real LuaLS definition against such a tracked declaration. Text/token-name searches are not
// valid producers. Sanitization collisions and duplicate declarations must remain ambiguous.
struct LuaBindingAnchor {
    std::string luaQualifiedName;
    BindingSourceRange declaration;
    bool ambiguous = false;
    std::optional<text::Snapshot> capturedSnapshot;
};

// Attaches generator-recorded identities to the exact captured output. Refuses another text or
// malformed ranges; propagates ambiguity. Pure; O(output bytes + declarations).
[[nodiscard]] Result<std::vector<LuaBindingAnchor>>
captureBindingLuaAnchors(const GeneratedEngineLuaMetadata& metadata, std::string path, const text::Snapshot& snapshot);

struct BindingQuerySeed {
    BindingProviderCapture provider;
    BindingSourceRange anchor;    // Actual name selection or a position verified by the provider's exact USR.
    std::string luaQualifiedName; // Lua seed's exact schema identity; empty for a native seed.
    std::string nativeUsr;        // Native seed's exact AST identity; empty for a Lua seed.
    std::optional<text::Snapshot> anchorSnapshot;
};

// Creates native+Lua seeds for usages of an AST USR. Explicit schema sameAs aliases participate in
// usages, but no names outside that resolved component do. Every Lua identity needs one tracked
// declaration; missing/ambiguous anchors fail the combined query instead of silently omitting Lua.
// Native seeds are candidates: dispatch must confirm their exact USR with raw clangd symbolInfo.
// Pure; O(bindings * anchors). Errors: NotFound, Busy, InvalidArgument.
[[nodiscard]] Result<std::vector<BindingQuerySeed>>
bindingUsageSeeds(const BindingIndex& index, std::string_view nativeUsr, const BindingProviderCapture& nativeProvider,
                  const BindingProviderCapture& luaProvider, std::span<const LuaBindingAnchor> luaAnchors);

struct BindingUsage {
    BindingProviderCapture provider;
    BindingSourceRange location;
    std::uint64_t snapshotVersion = 0;
    std::string text; // Exact captured bytes, including multiline ranges; never a guessed name.
    text::Snapshot capturedSnapshot;
};

enum class BindingPreviewOrigin : std::uint8_t { LanguageServer, RegistrationLiteral };

struct BindingPreviewEdit {
    BindingProviderCapture provider;
    BindingSourceRange location;
    std::uint64_t snapshotVersion = 0;
    std::string before;
    std::string after;
    std::vector<BindingProviderCapture> corroboratingProviders; // Identical edits merged during combination.
    text::Snapshot capturedSnapshot; // Retains lineage as well as version; unrelated version-zero text is stale.
    BindingDocumentRole resourceRole = BindingDocumentRole::ExternalReadOnly;
    BindingPreviewOrigin origin = BindingPreviewOrigin::LanguageServer;
    std::vector<BindingPreviewOrigin> corroboratingOrigins = {};
};

struct BindingPreviewFile {
    std::string path;
    text::Snapshot snapshot;
    std::vector<BindingPreviewEdit> edits; // Sorted, nonoverlapping; duplicate identical edits coalesce.
    std::string preview;                   // Computed in memory from snapshot; never written to disk.
};

// All returned files must already be in documents, captured for this query. A missing snapshot,
// malformed/partial response, clamped/mid-codepoint range, or mismatching documentChanges version
// refuses the whole result. Pure, thread-safe with immutable inputs; O(results * documents).
[[nodiscard]] Result<std::vector<BindingUsage>>
normalizeBindingReferences(const BindingProviderCapture& provider, const rpc::Json& raw,
                           std::span<const BindingDocumentCapture> documents);
[[nodiscard]] Result<std::vector<BindingPreviewEdit>>
normalizeBindingRename(const BindingProviderCapture& provider, const rpc::Json& raw,
                       std::span<const BindingDocumentCapture> documents);
// Refuses conflicting/overlapping edits from either provider, including different insertions at
// the same position. No application/rollback API: transactional mutation belongs to R8.
[[nodiscard]] Result<std::vector<BindingPreviewFile>>
combineBindingPreview(std::span<const BindingPreviewEdit> edits, std::span<const BindingDocumentCapture> documents);

struct BindingProviderRequest {
    BindingQuerySeed seed;
    lang::ILspRequester* requester = nullptr; // Must outlive the coroutine.
};
struct BindingRenameRequest {
    BindingProviderRequest target;
    std::string newName;
};

struct SelectedLuaBindingRename {
    BindingProviderRequest target;
    std::string newName; // Explicitly selected Lua surface identity only, never all sameAs aliases.
};

struct BindingGeneratedResourcePreview {
    std::string path;
    text::Snapshot before;
    GeneratedEngineLuaMetadata regenerated;
    std::vector<BindingPreviewEdit> observedServerEdits;
    std::string observedLanguageServerText; // May omit nonsemantic derived Same as documentation updates.
};
struct BindingSelectedRenamePreview {
    std::vector<BindingPreviewFile> sourceFiles;
    BindingGeneratedResourcePreview generatedResource; // Regeneration intent; never an arbitrary cache-file write.
    std::string previousLuaIdentity;
    std::string renamedLuaIdentity;
    bool nativeRenameRequested = false;
    // These language servers do not semantically index embedded Lua inside native string literals.
    // R7 exposes an immutable provider-scoped preview, never application-safe/exhaustive coverage.
    bool embeddedLanguageReferencesVerified = false;
    bool applicationSafe = false;
};

// Must check captured project/schema/doc/provider generations and current trust. Invoked before
// EACH request, after EACH response and before publication; absent callback fails closed.
using BindingQueryAdmission = std::function<bool(const BindingProviderCapture&)>;

// Confirms native candidates with actual textDocument/symbolInfo (one exact matching USR), then
// calls actual textDocument/references for each semantic seed. Inputs are owned by the coroutine;
// references remain valid only for their captured provider/project/document revisions. No I/O or
// synchronous waits beyond the supplied LSP requester. Errors: admission/cancellation/LSP/decoder.
[[nodiscard]] sched::Task<Result<std::vector<BindingUsage>>>
queryBindingUsages(std::vector<BindingProviderRequest> requests, std::vector<BindingDocumentCapture> documents,
                   BindingQueryAdmission stillCurrent, std::stop_token stop = {});
// Every target/newName is explicitly selected. This NEVER expands sameAs aliases on its own.
// Sends only textDocument/rename, reads raw edits and returns an in-memory preview. Does not call
// workspace/applyEdit, update documents or launch a process. R7 is strictly preview-only.
[[nodiscard]] sched::Task<Result<std::vector<BindingPreviewFile>>>
queryBindingRenamePreview(std::vector<BindingRenameRequest> requests, std::vector<BindingDocumentCapture> documents,
                          BindingQueryAdmission stillCurrent, std::stop_token stop = {});

// Selected Lua surface rename: verifies the mapper's exact AST name-literal range/decoded name/
// spelling against the captured UserSource, then combines its explicit literal edit with actual
// LuaLS edits and optionally explicitly selected native renames. Generated metadata is returned
// as regeneration intent only; LuaLS's observed cache edits MUST exactly reproduce that generated
// declaration projection (or full regenerated text). Runtime-pointer-derived sameAs relation
// documentation is regenerated separately without renaming aliases or claiming LuaLS changed
// comments. Unknown/external edits, computed names and structural type renames refuse the preview.
// Callable edits also require exact AST ScriptDoc.Signature token evidence. No apply/cache writes.
// Inputs own their lifetime; caller admission checks the captured index revision as well as trust,
// project/doc/provider generations. Errors retain actual provider/decoder/refusal explanations.
[[nodiscard]] sched::Task<Result<BindingSelectedRenamePreview>>
querySelectedBindingRenamePreview(std::shared_ptr<const BindingIndex> index, SelectedLuaBindingRename selected,
                                  std::vector<BindingRenameRequest> nativeRenames,
                                  std::vector<BindingDocumentCapture> documents, BindingQueryAdmission stillCurrent,
                                  std::stop_token stop = {});

} // namespace frost::engine
