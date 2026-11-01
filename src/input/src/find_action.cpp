// SPDX-License-Identifier: Apache-2.0
//
// FindActionModel (find_action.hpp; ADR 0016 decision 10): query normalisation, the
// scoring tiers, the shortcut tier and the total ranking order. The scoring is our own
// design, documented in find_action.hpp.

#include "frost/input/find_action.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::input {

namespace {

// One code point of a text: the ASCII-folded value (an invalid byte b is 0x110000 + b, so
// it equals only itself) and where it is in the UTF-8 text.
struct Unit {
    char32_t cp = 0;
    std::uint16_t offset = 0;
    std::uint8_t length = 0;
    bool upper = false; // An ASCII uppercase letter before folding.
    bool lower = false; // An ASCII lowercase letter.
    bool space = false;
    bool punct = false; // ASCII punctuation.
};

constexpr bool isAsciiPunct(char32_t c) noexcept {
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}

// The units of `text` (at most 65535 bytes are considered; names are at most 256).
std::vector<Unit> decode(std::string_view text) {
    text = text.substr(0, std::numeric_limits<std::uint16_t>::max());
    std::vector<Unit> units;
    units.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto [cp, length, _] = detail::decodeUtf8At(text, i);
        Unit u;
        u.upper = cp >= U'A' && cp <= U'Z';
        u.lower = cp >= U'a' && cp <= U'z';
        u.cp = u.upper ? cp + 0x20 : cp;
        u.space = cp == U' ' || cp == U'\t';
        if (u.space) {
            u.cp = U' ';
        }
        u.punct = isAsciiPunct(cp);
        u.offset = static_cast<std::uint16_t>(i);
        u.length = static_cast<std::uint8_t>(length);
        units.push_back(u);
        i += length;
    }
    return units;
}

// Leading and trailing whitespace dropped, inner runs collapsed to their first unit.
std::vector<Unit> collapse(const std::vector<Unit>& units) {
    std::vector<Unit> out;
    out.reserve(units.size());
    for (const Unit& u : units) {
        if (u.space && (out.empty() || out.back().space)) {
            continue;
        }
        out.push_back(u);
    }
    if (!out.empty() && out.back().space) {
        out.pop_back();
    }
    return out;
}

std::vector<Unit> withoutSpaces(const std::vector<Unit>& units) {
    std::vector<Unit> out;
    out.reserve(units.size());
    for (const Unit& u : units) {
        if (!u.space) {
            out.push_back(u);
        }
    }
    return out;
}

// The query cut to kMaxFindActionQueryBytes at a code-point boundary.
std::string_view capQuery(std::string_view text) noexcept {
    if (text.size() <= kMaxFindActionQueryBytes) {
        return text;
    }
    std::size_t end = kMaxFindActionQueryBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

struct Ranges {
    std::array<TextRange, 16> ranges{};
    std::uint8_t count = 0;
    bool truncated = false;

    // Adds the byte range of units [first, last], merging with the previous range when adjacent.
    void add(const Unit& first, const Unit& last) noexcept {
        const auto offset = first.offset;
        const auto end = static_cast<std::uint16_t>(last.offset + last.length);
        if (count > 0) {
            TextRange& previous = ranges[static_cast<std::size_t>(count) - 1];
            if (previous.offset + previous.length == offset) {
                previous.length = static_cast<std::uint16_t>(end - previous.offset);
                return;
            }
        }
        if (count == ranges.size()) {
            truncated = true;
            return;
        }
        ranges[count++] = TextRange{offset, static_cast<std::uint16_t>(end - offset)};
    }
};

struct TextScore {
    int score = 0; // 0: no match.
    Ranges highlight;
};

bool sameUnits(const std::vector<Unit>& t, std::size_t at, const std::vector<Unit>& q) noexcept {
    if (at + q.size() > t.size()) {
        return false;
    }
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (t[at + i].cp != q[i].cp) {
            return false;
        }
    }
    return true;
}

// The words of `t` as [begin, end) unit ranges: runs of non-space, non-punctuation units,
// also split before an uppercase letter that follows a lowercase one.
std::vector<std::pair<std::size_t, std::size_t>> words(const std::vector<Unit>& t) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    std::size_t i = 0;
    while (i < t.size()) {
        if (t[i].space || t[i].punct) {
            ++i;
            continue;
        }
        const std::size_t begin = i++;
        while (i < t.size() && !t[i].space && !t[i].punct && !(t[i].upper && t[i - 1].lower)) {
            ++i;
        }
        out.emplace_back(begin, i);
    }
    return out;
}

// The segmented word-prefix tier: q (no spaces) split into prefixes of strictly increasing
// words. Returns the first word index of the best (fewest words skipped) match and the
// highlight, or nullopt.
std::optional<std::pair<std::size_t, Ranges>> wordPrefix(const std::vector<Unit>& t, const std::vector<Unit>& q) {
    const auto w = words(t);
    const std::size_t m = q.size();
    const std::size_t n = w.size();
    if (m == 0 || n == 0) {
        return std::nullopt;
    }
    // ok[qi * (n + 1) + wi]: q[qi..m) can be matched using words wi..n-1 (in increasing order).
    std::vector<std::uint8_t> ok((m + 1) * (n + 1), 0);
    const auto at = [&](std::size_t qi, std::size_t wi) -> std::uint8_t& { return ok[qi * (n + 1) + wi]; };
    // How many units of q from qi are a prefix of word wi.
    const auto common = [&](std::size_t qi, std::size_t wi) {
        std::size_t k = 0;
        const auto [begin, end] = w[wi];
        while (qi + k < m && begin + k < end && t[begin + k].cp == q[qi + k].cp) {
            ++k;
        }
        return k;
    };
    for (std::size_t wi = 0; wi <= n; ++wi) {
        at(m, wi) = 1;
    }
    for (std::size_t qi = m; qi-- > 0;) {
        for (std::size_t wi = n; wi-- > 0;) {
            bool good = at(qi, wi + 1) != 0; // Skip word wi.
            const std::size_t k = common(qi, wi);
            for (std::size_t len = k; len >= 1 && !good; --len) {
                good = at(qi + len, wi + 1) != 0;
            }
            at(qi, wi) = good ? 1 : 0;
        }
    }
    // The first segment's word: the smallest wi where q starts on word wi.
    for (std::size_t first = 0; first < n; ++first) {
        const std::size_t k = common(0, first);
        std::size_t len = k;
        while (len >= 1 && at(len, first + 1) == 0) {
            --len;
        }
        if (len == 0) {
            continue;
        }
        // Reconstruct: at each step the earliest feasible word, with its longest feasible segment.
        Ranges ranges;
        ranges.add(t[w[first].first], t[w[first].first + len - 1]);
        std::size_t qi = len;
        std::size_t wi = first + 1;
        for (; qi < m && wi < n; ++wi) {
            std::size_t seg = common(qi, wi);
            while (seg >= 1 && at(qi + seg, wi + 1) == 0) {
                --seg;
            }
            if (seg >= 1) {
                ranges.add(t[w[wi].first], t[w[wi].first + seg - 1]);
                qi += seg;
            }
        }
        return std::pair(first, ranges);
    }
    return std::nullopt;
}

// The subsequence tier: the shortest window of t containing q (no spaces) in order.
std::optional<std::pair<std::size_t, Ranges>> subsequence(const std::vector<Unit>& t, const std::vector<Unit>& q) {
    if (q.empty()) {
        return std::nullopt;
    }
    std::size_t bestStart = 0;
    std::size_t bestSpan = std::numeric_limits<std::size_t>::max();
    for (std::size_t s = 0; s < t.size(); ++s) {
        if (t[s].cp != q[0].cp) {
            continue;
        }
        std::size_t qi = 1;
        std::size_t i = s + 1;
        while (qi < q.size() && i < t.size()) {
            if (t[i].cp == q[qi].cp) {
                ++qi;
            }
            ++i;
        }
        if (qi < q.size()) {
            break; // No later start can complete either.
        }
        if (i - s < bestSpan) {
            bestSpan = i - s;
            bestStart = s;
        }
    }
    if (bestSpan == std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    Ranges ranges;
    std::size_t qi = 0;
    for (std::size_t i = bestStart; qi < q.size(); ++i) {
        if (t[i].cp == q[qi].cp) {
            ranges.add(t[i], t[i]);
            ++qi;
        }
    }
    return std::pair(bestSpan, ranges);
}

// TODO(M2): cache the decoded names and synonyms per registry generation if Find Action over
// the full M2 action set nears its budget; re-decoding 500 actions per query measures 0.43 ms.
TextScore scoreText(std::string_view text, const std::vector<Unit>& qCollapsed, const std::vector<Unit>& qBare) {
    const std::vector<Unit> units = decode(text);
    const std::vector<Unit> tCollapsed = collapse(units);
    TextScore best;
    const int lengthPenalty = static_cast<int>(text.size() / 4);
    // Every matching text scores at least 1, whatever the length penalty.
    const auto consider = [&](int tier, Ranges ranges) {
        const int bonus = ranges.count > 0 && ranges.ranges[0].offset == 0 ? 20 : 0;
        const int score = std::max(1, tier + bonus - lengthPenalty);
        if (score > best.score) {
            best.score = score;
            best.highlight = ranges;
        }
    };
    if (tCollapsed.empty() || qCollapsed.empty()) {
        return best;
    }
    // 1000: equal; 900: prefix.
    if (sameUnits(tCollapsed, 0, qCollapsed)) {
        Ranges ranges;
        ranges.add(tCollapsed.front(), tCollapsed[qCollapsed.size() - 1]);
        consider(qCollapsed.size() == tCollapsed.size() ? 1000 : 900, ranges);
    }
    // 800 - 10 * skipped words.
    if (auto match = wordPrefix(units, qBare)) {
        consider(800 - 10 * static_cast<int>(match->first), match->second);
    }
    // 600: substring.
    for (std::size_t i = 0; i + qCollapsed.size() <= tCollapsed.size(); ++i) {
        if (sameUnits(tCollapsed, i, qCollapsed)) {
            Ranges ranges;
            ranges.add(tCollapsed[i], tCollapsed[i + qCollapsed.size() - 1]);
            consider(600, ranges);
            break;
        }
    }
    // 100-500: subsequence.
    if (best.score == 0) {
        if (auto match = subsequence(units, qBare)) {
            const auto extra = static_cast<long long>(match->first) - static_cast<long long>(qBare.size());
            const long long tier = std::max<long long>(100, 500 - 5 * extra);
            consider(static_cast<int>(tier), match->second);
        }
    }
    return best;
}

// The shortcut tier for one action: 950 when bound to `sequence`, 850 when a two-stroke
// binding starts with the single stroke `sequence`, else 0.
int shortcutScore(const Sequence& sequence, std::span<const Sequence> bound) noexcept {
    int score = 0;
    for (const Sequence& s : bound) {
        if (s == sequence) {
            return 950;
        }
        if (sequence.length() == 1 && s.length() == 2 && s.first() == sequence.first()) {
            score = 850;
        }
    }
    return score;
}

} // namespace

namespace {

// The sequence a capped query names when isShortcutQuery() holds for it, parsed once.
std::optional<Sequence> shortcutOfQuery(std::string_view query) {
    auto parsed = parseSequence(query, SyntaxMode::Lenient);
    if (!parsed) {
        return std::nullopt;
    }
    bool shortcut = parsed->isGesture() || (parsed->length() == 1 && isFunctionKey(parsed->first().key)) ||
                    query.find('+') != std::string_view::npos || query.find(", ") != std::string_view::npos;
    for (std::size_t i = 0; i < parsed->length(); ++i) {
        shortcut = shortcut || !parsed->stroke(i).mods.empty();
    }
    return shortcut ? std::optional<Sequence>(*parsed) : std::nullopt;
}

} // namespace

bool isShortcutQuery(std::string_view query) { return shortcutOfQuery(capQuery(query)).has_value(); }

std::uint32_t FindActionModel::usageOf(std::string_view id) const noexcept {
    const auto at =
        std::ranges::lower_bound(m_usage, id, {}, [](const auto& entry) { return std::string_view(entry.first); });
    return at != m_usage.end() && at->first == id ? at->second : 0;
}

std::vector<FindActionHit> FindActionModel::rank(std::vector<FindActionHit> hits, FindActionOptions options) const {
    const ActionRegistry& registry = *m_registry;
    std::ranges::sort(hits, [&](const FindActionHit& a, const FindActionHit& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.enabled != b.enabled) {
            return a.enabled;
        }
        const ActionInfo& ia = registry.info(a.id);
        const ActionInfo& ib = registry.info(b.id);
        const std::uint32_t ua = usageOf(ia.id);
        const std::uint32_t ub = usageOf(ib.id);
        if (ua != ub) {
            return ua > ub;
        }
        if (ia.name.size() != ib.name.size()) {
            return ia.name.size() < ib.name.size();
        }
        if (ia.name != ib.name) {
            return ia.name < ib.name;
        }
        return ia.id < ib.id;
    });
    if (options.limit != 0 && hits.size() > options.limit) {
        hits.resize(options.limit);
    }
    return hits;
}

std::vector<FindActionHit> FindActionModel::query(std::string_view text, const BindingIndex& bindings,
                                                  const ActionContext& context, FindActionOptions options) const {
    text = capQuery(text);
    const std::vector<Unit> qUnits = decode(text);
    const std::vector<Unit> qCollapsed = collapse(qUnits);
    if (qCollapsed.empty()) {
        return {};
    }
    const std::vector<Unit> qBare = withoutSpaces(qUnits);
    const std::optional<Sequence> shortcut = shortcutOfQuery(text);

    const ActionRegistry& registry = *m_registry;
    std::vector<FindActionHit> hits;
    for (const ActionId id : registry.all()) {
        const ActionInfo& info = registry.info(id);
        if (!info.searchable || !registry.isApplicable(id, context)) {
            continue;
        }
        FindActionHit hit;
        hit.id = id;
        const auto take = [&](int score, MatchSource source, std::size_t textIndex, const Ranges& ranges) {
            if (score > hit.score) {
                hit.score = score;
                hit.source = source;
                hit.textIndex = static_cast<std::uint8_t>(textIndex);
                hit.highlight = ranges.ranges;
                hit.highlightCount = ranges.count;
                hit.highlightTruncated = ranges.truncated;
            }
        };
        {
            const TextScore s = scoreText(info.name, qCollapsed, qBare);
            take(s.score, MatchSource::Name, 0, s.highlight);
        }
        for (std::size_t i = 0; i < info.synonyms.size(); ++i) {
            const TextScore s = scoreText(info.synonyms[i], qCollapsed, qBare);
            if (s.score > 0) {
                take(std::max(1, s.score - 50), MatchSource::Synonym, i + 1, s.highlight);
            }
        }
        if (shortcut) {
            take(shortcutScore(*shortcut, bindings.shortcutsFor(id)), MatchSource::Shortcut, 0, Ranges{});
        }
        if (hit.score <= 0) {
            continue;
        }
        hit.disabledReason = registry.whyDisabled(id, context);
        hit.enabled = hit.disabledReason == DisabledReason::None;
        if (!hit.enabled && !options.includeDisabled) {
            continue;
        }
        hits.push_back(hit);
    }
    return rank(std::move(hits), options);
}

std::vector<FindActionHit> FindActionModel::queryShortcut(const Sequence& sequence, const BindingIndex& bindings,
                                                          const ActionContext& context,
                                                          FindActionOptions options) const {
    const ActionRegistry& registry = *m_registry;
    std::vector<FindActionHit> hits;
    if (!sequence.valid()) {
        return hits;
    }
    for (const ActionId id : registry.all()) {
        const ActionInfo& info = registry.info(id);
        if (!info.searchable || !registry.isApplicable(id, context)) {
            continue;
        }
        const int score = shortcutScore(sequence, bindings.shortcutsFor(id));
        if (score == 0) {
            continue;
        }
        FindActionHit hit;
        hit.id = id;
        hit.score = score;
        hit.source = MatchSource::Shortcut;
        hit.disabledReason = registry.whyDisabled(id, context);
        hit.enabled = hit.disabledReason == DisabledReason::None;
        if (!hit.enabled && !options.includeDisabled) {
            continue;
        }
        hits.push_back(hit);
    }
    return rank(std::move(hits), options);
}

void FindActionModel::noteInvoked(ActionId id) {
    const std::string& key = m_registry->info(id).id;
    const auto at = std::ranges::lower_bound(m_usage, std::string_view(key), {},
                                             [](const auto& entry) { return std::string_view(entry.first); });
    if (at != m_usage.end() && at->first == key) {
        if (at->second != std::numeric_limits<std::uint32_t>::max()) {
            ++at->second;
        }
        return;
    }
    m_usage.insert(at, {key, 1});
}

std::vector<std::pair<std::string, std::uint32_t>> FindActionModel::exportUsage() const { return m_usage; }

void FindActionModel::importUsage(std::span<const std::pair<std::string, std::uint32_t>> usage) {
    for (const auto& [id, count] : usage) {
        const auto at = std::ranges::lower_bound(m_usage, std::string_view(id), {},
                                                 [](const auto& entry) { return std::string_view(entry.first); });
        if (at != m_usage.end() && at->first == id) {
            at->second = count;
        } else {
            m_usage.insert(at, {id, count});
        }
    }
}

} // namespace frost::input
