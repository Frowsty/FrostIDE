// SPDX-License-Identifier: Apache-2.0
//
// Semantic highlighting from the language server (PLAN.md M3 "semantic tokens, inactive regions";
// ADR 0047): the LSP 3.17 semanticTokens legend, the relative token stream decoded into byte
// ranges with a type and modifiers, full and delta requests, and clangd's inactive-regions
// extension (textDocument/inactiveRegions, clangd 17+). Written from the LSP 3.17 specification
// ("Semantic Tokens") and clangd's public extension documentation (clangd.llvm.org/extensions).
/*
 * Legend. The server names its token types and modifiers in
 * capabilities.semanticTokensProvider.legend; a token's type is an index into the type list and
 * its modifiers a bit set over the modifier list. parseSemanticTokensLegend translates both into
 * SemanticTokenType and SemanticModifier bits once, so decoding never compares strings. Names this
 * client does not know become SemanticTokenType::Unknown and no modifier bit.
 *
 * Stream. `data` holds five integers per token: deltaLine, deltaStart (relative to the previous
 * token's start when on the same line), length, type and modifiers; positions and lengths are in
 * the negotiated position encoding. Tokens never span lines here (multilineTokenSupport is not
 * announced). decodeSemanticTokens maps them to byte ranges of the snapshot the server saw;
 * tokens past the snapshot's end are dropped.
 *
 * Deltas. A delta response lists edits to the previous result's integer array (start index,
 * delete count, inserted integers). applySemanticTokensEdits applies them to a copy; the result
 * is the new full array, which is decoded as above.
 *
 * Inactive regions. clangd sends textDocument/inactiveRegions ({textDocument: {uri}, regions:
 * Range[]}) after each AST build when the client announces
 * textDocument.inactiveRegionsCapabilities.inactiveRegions; without it, older clangd versions mark
 * the same code with the "comment" token type, which the editor bridge treats as inactive.
 *
 * Every function is pure and thread-safe; requestSemanticTokens runs on the requester's scheduler.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/sched/task.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::lang {

// The LSP 3.17 SemanticTokenTypes plus clangd's own ("concept", "bracket", "label", "unknown").
enum class SemanticTokenType : std::uint8_t {
    Namespace,
    Type,
    Class,
    Enum,
    Interface,
    Struct,
    TypeParameter,
    Parameter,
    Variable,
    Property,
    EnumMember,
    Event,
    Function,
    Method,
    Macro,
    Keyword,
    Modifier,
    Comment,
    String,
    Number,
    Regexp,
    Operator,
    Decorator,
    Concept,
    Bracket,
    Label,
    Unknown,
    kCount
};

inline constexpr std::size_t kSemanticTokenTypeCount = static_cast<std::size_t>(SemanticTokenType::kCount);

// The protocol name of a type ("namespace", "typeParameter", ...). O(1).
[[nodiscard]] std::string_view toString(SemanticTokenType type) noexcept;
// The type of a protocol name; Unknown for names this client does not know. O(types).
[[nodiscard]] SemanticTokenType parseSemanticTokenType(std::string_view name) noexcept;

// The LSP 3.17 SemanticTokenModifiers and clangd's, as bits of SemanticToken::modifiers.
enum class SemanticModifier : std::uint32_t {
    Declaration = 1U << 0,
    Definition = 1U << 1,
    Readonly = 1U << 2,
    Static = 1U << 3,
    Deprecated = 1U << 4,
    Abstract = 1U << 5,
    Async = 1U << 6,
    Modification = 1U << 7,
    Documentation = 1U << 8,
    DefaultLibrary = 1U << 9,
    // clangd's extensions.
    Deduced = 1U << 10,
    Virtual = 1U << 11,
    DependentName = 1U << 12,
    UsedAsMutableReference = 1U << 13,
    UsedAsMutablePointer = 1U << 14,
    ConstructorOrDestructor = 1U << 15,
    UserDefined = 1U << 16,
    FunctionScope = 1U << 17,
    ClassScope = 1U << 18,
    FileScope = 1U << 19,
    GlobalScope = 1U << 20,
};

inline constexpr std::size_t kSemanticModifierCount = 21;

// The protocol name of the modifier with bit `index` (0-based; "" past the last). O(1).
[[nodiscard]] std::string_view semanticModifierName(std::size_t index) noexcept;
// The bit of a protocol name, 0 for names this client does not know. O(modifiers).
[[nodiscard]] std::uint32_t parseSemanticModifier(std::string_view name) noexcept;
// True when `modifiers` has `modifier`. O(1).
[[nodiscard]] constexpr bool hasModifier(std::uint32_t modifiers, SemanticModifier modifier) noexcept {
    return (modifiers & static_cast<std::uint32_t>(modifier)) != 0;
}

// The server's legend translated to this client's enums (see the header comment).
struct SemanticTokensLegend {
    std::vector<SemanticTokenType> types;    // Index: the token's type number.
    std::vector<std::uint32_t> modifierBits; // Index: the bit position in the token's modifiers.
    friend bool operator==(const SemanticTokensLegend&, const SemanticTokensLegend&) = default;
};

// What the server offers: its legend and whether it answers /full and /full/delta.
struct SemanticTokensSupport {
    SemanticTokensLegend legend;
    bool full = false;
    bool delta = false;
};

// Reads capabilities.semanticTokensProvider ({legend, full: bool | {delta}, range}). nullopt when
// the server offers no semantic tokens or its legend is malformed. O(legend).
[[nodiscard]] std::optional<SemanticTokensSupport> semanticTokensSupport(const ServerCapabilities& capabilities);
// The legend object alone ({tokenTypes, tokenModifiers}). nullopt when malformed.
[[nodiscard]] std::optional<SemanticTokensLegend> parseSemanticTokensLegend(const Json& legend);

// One decoded token.
struct SemanticToken {
    text::ByteRange range;
    SemanticTokenType type = SemanticTokenType::Unknown;
    std::uint32_t modifiers = 0; // SemanticModifier bits.
    friend bool operator==(const SemanticToken&, const SemanticToken&) = default;
};

/*
 * Decodes a relative token stream against `snapshot`, the text the server saw. Tokens are
 * returned in stream order (sorted by start); a token whose line is past the end is dropped, its
 * end is clamped to its line. Errors: Corrupt (the size is not a multiple of five). O(tokens ×
 * log n).
 */
[[nodiscard]] Result<std::vector<SemanticToken>> decodeSemanticTokens(std::span<const std::uint32_t> data,
                                                                      const SemanticTokensLegend& legend,
                                                                      const text::Snapshot& snapshot,
                                                                      PositionEncoding encoding);

// One SemanticTokensEdit: replace `deleteCount` integers at `start` by `data`.
struct SemanticTokensEdit {
    std::uint32_t start = 0;
    std::uint32_t deleteCount = 0;
    std::vector<std::uint32_t> data;
    friend bool operator==(const SemanticTokensEdit&, const SemanticTokensEdit&) = default;
};

/*
 * `previous` with `edits` applied. The edits refer to indices of `previous`; they are applied
 * from the highest start down, so each keeps its meaning. Errors: OutOfRange (an edit reaches past
 * the end), InvalidArgument (edits overlap). O(size + edits × log edits).
 */
[[nodiscard]] Result<std::vector<std::uint32_t>> applySemanticTokensEdits(std::span<const std::uint32_t> previous,
                                                                          std::span<const SemanticTokensEdit> edits);

// A result's integers and its id: what a delta request needs to name the previous result.
struct SemanticTokensData {
    std::string resultId; // "" when the server sent none (no delta is possible then).
    std::vector<std::uint32_t> data;
    friend bool operator==(const SemanticTokensData&, const SemanticTokensData&) = default;
};

// A /full or /full/delta result: SemanticTokens ({resultId?, data}) or SemanticTokensDelta
// ({resultId?, edits}).
struct SemanticTokensResult {
    std::string resultId;
    std::optional<std::vector<std::uint32_t>> data;       // Set for SemanticTokens.
    std::optional<std::vector<SemanticTokensEdit>> edits; // Set for SemanticTokensDelta.
};

// Parses a result. Errors: InvalidArgument (null, or neither data nor edits; a non-integer).
[[nodiscard]] Result<SemanticTokensResult> parseSemanticTokensResult(const Json& result);

/*
 * Requests the tokens of `path`: textDocument/semanticTokens/full/delta when `previous` has a
 * result id and `useDelta` is set, else textDocument/semanticTokens/full. A delta that cannot be
 * applied falls back to one full request. Returns the new full integer array. Errors: the
 * requester's, InvalidArgument (path), and parseSemanticTokensResult's.
 */
[[nodiscard]] sched::Task<Result<SemanticTokensData>> requestSemanticTokens(ILspRequester& server, std::string path,
                                                                            std::optional<SemanticTokensData> previous,
                                                                            bool useDelta, std::stop_token token = {});

// The parts of clientCapabilities() for semantic tokens and inactive regions (lsp_client.cpp
// adds them under textDocument). Every type and modifier above is announced.
[[nodiscard]] Json semanticTokensClientCapabilities();
[[nodiscard]] Json inactiveRegionsClientCapabilities();

// A textDocument/inactiveRegions notification, decoded (ranges not mapped yet).
struct InactiveRegions {
    std::string file; // Absolute path.
    std::string uri;
    std::vector<LspRange> lspRanges;
    // Filled by mapInactiveRegions: the byte ranges in the snapshot of `textVersion`.
    std::vector<text::ByteRange> ranges;
    std::optional<std::uint64_t> textVersion;
    friend bool operator==(const InactiveRegions&, const InactiveRegions&) = default;
};

// The LSP method of the notification ("textDocument/inactiveRegions").
inline constexpr std::string_view kInactiveRegionsMethod = "textDocument/inactiveRegions";

// Decodes the params. Errors: InvalidArgument (no textDocument.uri, a non-file URI, no regions).
[[nodiscard]] Result<InactiveRegions> decodeInactiveRegions(const Json& params);
// Fills `ranges` and `textVersion` from `snapshot`; ranges outside it are dropped. O(r × log n).
void mapInactiveRegions(InactiveRegions& regions, const text::Snapshot& snapshot, PositionEncoding encoding);

// --- Inlay Hints (LSP 3.17 textDocument/inlayHint & clangd/inlayHints) ---

enum class LspInlayHintKind : std::uint8_t { Type = 1, Parameter = 2 };

struct LspInlayHint {
    LspPosition position;
    std::string label;
    LspInlayHintKind kind = LspInlayHintKind::Parameter;
    std::string tooltip;
    bool paddingLeft = false;
    bool paddingRight = false;
    friend bool operator==(const LspInlayHint&, const LspInlayHint&) = default;
};

// Parses an LSP 3.17 InlayHint[] or clangd's clangd/inlayHints result.
[[nodiscard]] std::vector<LspInlayHint> parseInlayHints(const Json& json);

// Requests the advertised standard or legacy-clangd hint protocol over `range`; never retries
// arbitrary failures using another protocol. Unsupported when no provider is advertised.
[[nodiscard]] sched::Task<Result<std::vector<LspInlayHint>>>
requestInlayHints(ILspRequester& server, std::string path, LspRange range, std::stop_token token = {});

} // namespace frost::lang
