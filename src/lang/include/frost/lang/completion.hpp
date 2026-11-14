// SPDX-License-Identifier: Apache-2.0
//
// Code completion (W2, ADR 0041; PLAN.md 3.4 "Completion"): textDocument/completion decoded into
// CompletionItems, the auto-popup triggers, completionItem/resolve, the popup's model with
// client-side fuzzy filtering and Rider-like sorting while the user keeps typing, and the edits
// that commit an item (snippets as plain text with tab stops, additional edits included).
/*
 * Session. A request is made once per word (Ctrl+Space, the first identifier character, '.',
 * '->', '::', or '<' '"' '/' in an #include). While the popup is open, typing more identifier
 * characters only re-filters the items locally (CompletionModel::setPrefix); a new request is
 * made only when the server marked the list incomplete. Esc cancels; Enter and Tab commit.
 *
 * Order (CompletionModel). Items whose filter text (filterText, else the label) does not match
 * the typed prefix are hidden. The rest are sorted by match kind (exact, prefix, prefix ignoring
 * case, word starts, fuzzy: fuzzy.hpp), then deprecated items last, then for word-start and
 * fuzzy matches the higher fuzzy score, then the server's sortText (clangd ranks with it), then
 * the shorter and then the alphabetically smaller label. The server preselect is honored within the best
 * non-deprecated match class; otherwise the first entry is selected.
 *
 * Commit (commitCompletion). The main edit replaces [range start, caret): the item's textEdit
 * range (stretched to the caret, since the user may have typed on) or the identifier before the
 * caret; its text is the snippet's plain text or insertText (else the label). The additional
 * edits (clangd's #include insertions) are mapped onto the same snapshot; the caret and the tab
 * stops are shifted past the edits before them.
 *
 * Threads: the requests are coroutines (run them on the scheduler); everything else is pure or a
 * value type used by one thread (the UI).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/fuzzy.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/sched/task.hpp"
#include "frost/text/snapshot.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::lang {

// LSP CompletionItemKind; the numbers are the protocol's.
enum class CompletionItemKind : std::uint8_t {
    Text = 1,
    Method,
    Function,
    Constructor,
    Field,
    Variable,
    Class,
    Interface,
    Module,
    Property,
    Unit,
    Value,
    Enum,
    Keyword,
    Snippet,
    Color,
    File,
    Reference,
    Folder,
    EnumMember,
    Constant,
    Struct,
    Event,
    Operator,
    TypeParameter,
};

// "method", "class", ... (lower-case English, for tooltips and tests).
[[nodiscard]] std::string_view toString(CompletionItemKind kind) noexcept;

struct CompletionItem {
    std::string label;      // As shown, with clangd's leading space or bullet removed.
    std::string filterText; // What the prefix is matched against (filterText, else the label).
    std::string sortText;   // The server's ranking key (sortText, else the label).
    std::string detail;     // Return type or signature.
    Markup documentation;
    CompletionItemKind kind = CompletionItemKind::Text;
    bool deprecated = false;
    bool preselect = false;
    bool snippet = false;   // insertText is a snippet (insertTextFormat 2).
    std::string insertText; // textEdit.newText, else insertText, else the label.
    std::optional<LspRange> editRange;
    std::vector<TextEdit> additionalTextEdits;
    std::vector<std::string> commitCharacters;
    double score = 0.0;    // clangd's "score" extension, when present.
    bool resolved = false; // completionItem/resolve already ran.
    Json raw;              // The item as received, for completionItem/resolve.
};

// Call snippets use actual server item kind/text; arbitrary server commands are never executed.
[[nodiscard]] bool completionRequestsParameterHints(const CompletionItem& item);

struct CompletionList {
    bool isIncomplete = false;
    std::vector<CompletionItem> items;
};

[[nodiscard]] CompletionItem parseCompletionItem(const Json& json);
// A CompletionList, a CompletionItem array or null.
[[nodiscard]] CompletionList parseCompletionList(const Json& json);

enum class CompletionTriggerKind : std::uint8_t {
    Invoked = 1,
    TriggerCharacter = 2,
    TriggerForIncompleteCompletions = 3
};

struct CompletionTrigger {
    CompletionTriggerKind kind = CompletionTriggerKind::Invoked;
    std::string character; // For TriggerCharacter.
    friend bool operator==(const CompletionTrigger&, const CompletionTrigger&) = default;
};

/*
 * Whether typing the last character of `lineBeforeCaret` (the caret's line up to the caret, the
 * typed character included) opens completion: the first character of an identifier (Invoked),
 * '.' after an identifier or ')' or ']' (not in a number), "->", "::", and '<', '"' or '/' on an
 * #include or #import line. Pure.
 */
[[nodiscard]] std::optional<CompletionTrigger> autoCompletionTrigger(std::string_view lineBeforeCaret) noexcept;

// The byte index in `lineBeforeCaret` where the identifier ending at its end starts (its size
// when it ends with no identifier character).
[[nodiscard]] std::size_t identifierStart(std::string_view lineBeforeCaret) noexcept;
// True for [A-Za-z0-9_] and any non-ASCII byte.
[[nodiscard]] bool isIdentifierByte(char c) noexcept;

// textDocument/completion at `position`. Errors: those of request (Busy, Cancelled, ...).
[[nodiscard]] sched::Task<Result<CompletionList>> requestCompletion(ILspRequester& server, std::string path,
                                                                    LspPosition position, CompletionTrigger trigger,
                                                                    std::stop_token token = {});
// completionItem/resolve; returns `item` unchanged (marked resolved) when the server has no
// resolve support. Errors: those of request.
[[nodiscard]] sched::Task<Result<CompletionItem>> resolveCompletionItem(ILspRequester& server, CompletionItem item,
                                                                        std::stop_token token = {});

// One visible popup row: an index into CompletionModel::items() and how it matched.
struct CompletionEntry {
    std::size_t item = 0;
    FuzzyMatch match;
};

// The popup's model (see the header comment). A value type for the UI thread.
class CompletionModel {
public:
    // Replaces the items and re-filters with the current prefix; selects the first entry.
    void setList(CompletionList list);
    // Re-filters and re-sorts for `prefix` (the identifier typed so far). O(items x match).
    void setPrefix(std::string_view prefix);
    // Replaces item `index` (after resolve) without re-sorting.
    void replaceItem(std::size_t index, CompletionItem item);

    [[nodiscard]] const std::string& prefix() const noexcept { return m_prefix; }
    [[nodiscard]] bool isIncomplete() const noexcept { return m_incomplete; }
    [[nodiscard]] std::span<const CompletionItem> items() const noexcept { return m_items; }
    [[nodiscard]] std::span<const CompletionEntry> entries() const noexcept { return m_entries; }
    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }

    // The selected entry's index in entries(), if any entry is visible.
    [[nodiscard]] std::optional<std::size_t> selected() const noexcept;
    // Selects entry `index` (clamped). No-op when nothing is visible.
    void select(std::size_t index) noexcept;
    // Moves the selection by `delta`, wrapping at both ends (Up/Down).
    void moveSelection(int delta) noexcept;
    // The selected item, or nullptr.
    [[nodiscard]] const CompletionItem* selectedItem() const noexcept;
    [[nodiscard]] std::optional<std::size_t> selectedItemIndex() const noexcept;

private:
    void refilter();

    std::vector<CompletionItem> m_items;
    std::vector<CompletionEntry> m_entries;
    std::string m_prefix;
    bool m_incomplete = false;
    std::size_t m_selected = 0;
};

// What committing an item does to the document.
struct CompletionCommit {
    std::vector<ByteEdit> edits;           // Sorted; apply as one undo step.
    ByteOffset caret;                      // In the text after the edits.
    std::vector<text::ByteRange> tabStops; // In the text after the edits, in visiting order.
};

/*
 * The edits for committing `item` (see the header comment). `wordStart` is where the typed
 * identifier starts and `caret` the caret, both in `snapshot`. Errors: those of toByteEdits
 * (additional edits overlapping the main edit are InvalidArgument).
 */
[[nodiscard]] Result<CompletionCommit> commitCompletion(const CompletionItem& item, const text::Snapshot& snapshot,
                                                        ByteOffset wordStart, ByteOffset caret,
                                                        PositionEncoding encoding);

} // namespace frost::lang
