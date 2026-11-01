// SPDX-License-Identifier: Apache-2.0
//
// FindActionModel: the model behind Find Action (PLAN.md row 78, ADR 0016 decision 10):
// searches registered actions by name, synonyms and shortcut and ranks the hits. The
// popup is m1/shell-nav's; it shows names as plain text (Qt::PlainText, never AutoText).
/*
 * Scoring (our own; deterministic integer arithmetic). Matching works on whole code
 * points (invalid UTF-8 bytes count as one unit each), folding ASCII case only; non-ASCII
 * code points compare exactly. TODO(M2): Unicode case folding once utf8proc is available.
 * Words of a text split at spaces, ASCII punctuation and lower-to-upper transitions.
 * Per candidate text T and query Q (whitespace collapsed):
 *   1000  T equals Q.
 *    900  T starts with Q.
 *    800 - 10 * (words skipped before the first segment): Q, spaces ignored, splits into
 *         segments that are prefixes of strictly increasing words of T ("rc" and "refco"
 *         match "Reformat Code"; "code" matches it with one word skipped).
 *    600  Q is a substring of T.
 *    100-500 Q (spaces ignored) is a subsequence of T: 500 - 5 * (shortest span - |Q|),
 *         at least 100.
 * Bonuses: +20 when the match starts at byte 0; -1 per 4 bytes of T. Synonym hits score
 * 50 less. Shortcut tier, only when isShortcutQuery(Q): an action bound to exactly the
 * parsed sequence scores 950, one with a two-stroke binding starting with it 850. An
 * action's score is its best over name, synonyms and shortcuts (ties prefer the name, then
 * the earliest synonym, then the shortcut).
 *
 * Exclusions: actions with searchable=false, and actions not applicable in the context.
 * Actions disabled for another reason are listed with enabled=false and their reason
 * (dropped with includeDisabled=false). Order: score desc, enabled first, usage desc,
 * name length asc, name bytes asc, string id asc: a total order independent of
 * registration order.
 */
#pragma once

#include "frost/input/action.hpp"
#include "frost/input/key.hpp"
#include "frost/input/keymap.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::input {

// A UTF-8 byte range of the matched text; always on code-point boundaries.
struct TextRange {
    std::uint16_t offset = 0;
    std::uint16_t length = 0;
    friend constexpr bool operator==(TextRange, TextRange) noexcept = default;
};

enum class MatchSource : std::uint8_t { Name, Synonym, Shortcut };

struct FindActionHit {
    ActionId id = kNoAction;
    int score = 0;
    MatchSource source = MatchSource::Name;
    // 0 = the name; n = synonym n-1. 0 for shortcut hits.
    std::uint8_t textIndex = 0;
    // Matched ranges of the text, in order; empty for shortcut hits.
    std::array<TextRange, 16> highlight{};
    std::uint8_t highlightCount = 0;
    bool highlightTruncated = false;
    bool enabled = true;
    DisabledReason disabledReason = DisabledReason::None;
};

struct FindActionOptions {
    // At most this many hits; 0 means unbounded.
    std::size_t limit = 100;
    bool includeDisabled = true;
};

// Queries are cut to this many bytes (at a code-point boundary); the rest is ignored.
inline constexpr std::size_t kMaxFindActionQueryBytes = 256;

// True when `query` enters the shortcut tier: it parses with parseSequence(Lenient) and
// contains a modifier, is a function key F1..F24, or contains an explicit '+' or ", ".
// Bare words ("up", "end", "tab") and bare letters never do.
[[nodiscard]] bool isShortcutQuery(std::string_view query);

/*
 * Not thread-safe; must not run concurrently with registry mutation. The registry must
 * outlive the model. Usage counts are session-local, keyed by string action id (so
 * imported counts for actions not registered yet are kept until they are).
 */
class FindActionModel {
public:
    explicit FindActionModel(const ActionRegistry& registry) noexcept : m_registry(&registry) {}

    /*
     * Ranked hits for `text`. `context` must be the one captured when Find Action was
     * invoked (the focused component before the popup opened), not the popup's own; use the
     * same focus keys for tryInvoke, with `level` and `trusted` re-read from the live state
     * (trust may be revoked while the popup is open). Empty or whitespace-only queries
     * return no hits.
     * O(A * L * Q) for A actions, text length L and query length Q (Q <= 256 bytes).
     */
    [[nodiscard]] std::vector<FindActionHit> query(std::string_view text, const BindingIndex& bindings,
                                                   const ActionContext& context, FindActionOptions options = {}) const;
    // Actions bound to `sequence` (950) or, for a single stroke, with a two-stroke binding
    // starting with it (850): the "press a shortcut" mode of the Keymap page.
    [[nodiscard]] std::vector<FindActionHit> queryShortcut(const Sequence& sequence, const BindingIndex& bindings,
                                                           const ActionContext& context,
                                                           FindActionOptions options = {}) const;

    // Counts one invocation (the shell calls it after tryInvoke returned Ran).
    void noteInvoked(ActionId id);
    // The usage counts by string id, sorted by id.
    [[nodiscard]] std::vector<std::pair<std::string, std::uint32_t>> exportUsage() const;
    // Replaces the counts of the given ids (unknown ids are kept until registered).
    void importUsage(std::span<const std::pair<std::string, std::uint32_t>> usage);

private:
    [[nodiscard]] std::uint32_t usageOf(std::string_view id) const noexcept;
    [[nodiscard]] std::vector<FindActionHit> rank(std::vector<FindActionHit> hits, FindActionOptions options) const;

    const ActionRegistry* m_registry;
    std::vector<std::pair<std::string, std::uint32_t>> m_usage; // Sorted by id.
};

} // namespace frost::input
