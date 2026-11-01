// SPDX-License-Identifier: Apache-2.0
//
// BindingIndex: the sorted lookup structure the matcher searches, built from a resolved
// keymap, user overrides and the registered actions (keymap.hpp; ADR 0016 decision 8).

#include "frost/input/keymap.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace frost::input {

namespace {

// One (action, sequence) pair before sorting. `order` is the keymap order: the binding's
// position in the resolved list, then the sequence's position in its line.
struct Entry {
    Sequence sequence;
    int priority = 0;
    std::uint64_t order = 0;
    ActionId action = kNoAction;
};

// Sort key: (first stroke, second stroke, priority descending, keymap order).
bool entryLess(const Entry& a, const Entry& b) noexcept {
    if (a.sequence.first() != b.sequence.first()) {
        return a.sequence.first() < b.sequence.first();
    }
    if (a.sequence.second() != b.sequence.second()) {
        return a.sequence.second() < b.sequence.second();
    }
    if (a.priority != b.priority) {
        return a.priority > b.priority;
    }
    return a.order < b.order;
}

bool reachableOn(const Sequence& s, const LayoutTable& layout) noexcept {
    for (std::size_t i = 0; i < s.length(); ++i) {
        if (!layout.reachable(s.stroke(i).key)) {
            return false;
        }
    }
    return true;
}

void appendUnique(std::vector<ActionId>& out, ActionId id) {
    if (std::ranges::find(out, id) == out.end()) {
        out.push_back(id);
    }
}

} // namespace

BindingIndex BindingIndex::build(const Keymap& keymap, KeymapPlatform platform,
                                 std::span<const ActionOverride> overrides, const ActionRegistry& registry) {
    std::vector<Binding> resolved = keymap.bindings(platform);
    // Overrides replace an action's whole list; one for an action the keymap does not
    // name is appended. Later overrides of the same action win. An id-to-position map keeps
    // this O(B + O) (up to 4096 overrides may arrive on a keystroke-path rebuild); the
    // reserve keeps the map's views into the ids valid while overrides are appended.
    resolved.reserve(resolved.size() + overrides.size());
    std::unordered_map<std::string_view, std::size_t> position;
    position.reserve(resolved.capacity());
    for (std::size_t i = 0; i < resolved.size(); ++i) {
        position.emplace(resolved[i].actionId, i);
    }
    for (const ActionOverride& entry : overrides) {
        if (const auto it = position.find(entry.actionId); it != position.end()) {
            resolved[it->second].sequences = entry.sequences;
        } else {
            resolved.push_back(Binding{entry.actionId, entry.sequences});
            position.emplace(resolved.back().actionId, resolved.size() - 1);
        }
    }

    BindingIndex index;
    index.m_ranges.assign(registry.size(), Range{});
    std::vector<Entry> singles;
    std::vector<Entry> doubles;
    std::array<std::vector<Entry>, 2> gestures;
    std::vector<std::string> unresolved;

    // Per action, its sequences in keymap order, gathered before m_sequences is laid out.
    std::vector<std::vector<Sequence>> perAction(registry.size());
    for (std::size_t b = 0; b < resolved.size(); ++b) {
        const Binding& binding = resolved[b];
        const std::optional<ActionId> id = registry.find(binding.actionId);
        if (!id) {
            unresolved.push_back(binding.actionId);
            continue;
        }
        const int priority = registry.info(*id).priority;
        std::vector<Sequence>& mine = perAction[id->value()];
        for (std::size_t s = 0; s < binding.sequences.size(); ++s) {
            const Sequence& sequence = binding.sequences[s];
            if (!sequence.valid() || std::ranges::find(mine, sequence) != mine.end()) {
                continue;
            }
            mine.push_back(sequence);
            const Entry entry{sequence, priority, (static_cast<std::uint64_t>(b) << 32U) | s, *id};
            if (sequence.isGesture()) {
                gestures[sequence.gesture() == Gesture::DoubleShift ? 0 : 1].push_back(entry);
            } else if (sequence.length() == 1) {
                singles.push_back(entry);
            } else {
                doubles.push_back(entry);
            }
        }
    }

    for (std::size_t a = 0; a < perAction.size(); ++a) {
        index.m_ranges[a] = Range{static_cast<std::uint32_t>(index.m_sequences.size()),
                                  static_cast<std::uint32_t>(perAction[a].size())};
        index.m_sequences.insert(index.m_sequences.end(), perAction[a].begin(), perAction[a].end());
    }

    std::ranges::sort(singles, entryLess);
    std::ranges::sort(doubles, entryLess);
    index.m_singleKeys.reserve(singles.size());
    index.m_singleActions.reserve(singles.size());
    for (const Entry& e : singles) {
        index.m_singleKeys.push_back(e.sequence.first());
        index.m_singleActions.push_back(e.action);
    }
    index.m_doubleKeys.reserve(doubles.size());
    index.m_doubleActions.reserve(doubles.size());
    for (const Entry& e : doubles) {
        index.m_doubleKeys.emplace_back(e.sequence.first(), e.sequence.second());
        index.m_doubleActions.push_back(e.action);
    }
    for (std::size_t g = 0; g < gestures.size(); ++g) {
        std::ranges::sort(gestures[g], entryLess); // Same first/second (empty): priority, then order.
        for (const Entry& e : gestures[g]) {
            index.m_gestureActions[g].push_back(e.action);
        }
    }

    std::ranges::sort(unresolved);
    const auto [first, last] = std::ranges::unique(unresolved);
    unresolved.erase(first, last);
    index.m_unresolved = std::move(unresolved);
    return index;
}

std::span<const ActionId> BindingIndex::single(Stroke stroke) const noexcept {
    const auto [lo, hi] = std::ranges::equal_range(m_singleKeys, stroke);
    const auto begin = static_cast<std::size_t>(lo - m_singleKeys.begin());
    return std::span<const ActionId>(m_singleActions).subspan(begin, static_cast<std::size_t>(hi - lo));
}

std::span<const ActionId> BindingIndex::prefixed(Stroke first) const noexcept {
    const auto [lo, hi] = std::ranges::equal_range(m_doubleKeys, first, {}, &std::pair<Stroke, Stroke>::first);
    const auto begin = static_cast<std::size_t>(lo - m_doubleKeys.begin());
    return std::span<const ActionId>(m_doubleActions).subspan(begin, static_cast<std::size_t>(hi - lo));
}

std::span<const ActionId> BindingIndex::second(Stroke first, Stroke second) const noexcept {
    const auto [lo, hi] = std::ranges::equal_range(m_doubleKeys, std::pair(first, second));
    const auto begin = static_cast<std::size_t>(lo - m_doubleKeys.begin());
    return std::span<const ActionId>(m_doubleActions).subspan(begin, static_cast<std::size_t>(hi - lo));
}

std::span<const ActionId> BindingIndex::gesture(Gesture gesture) const noexcept {
    switch (gesture) {
    case Gesture::DoubleShift: return m_gestureActions[0];
    case Gesture::DoubleCtrl: return m_gestureActions[1];
    case Gesture::None: break;
    }
    return {};
}

std::span<const Sequence> BindingIndex::shortcutsFor(ActionId id) const noexcept {
    if (id.value() >= m_ranges.size()) {
        return {};
    }
    const Range range = m_ranges[id.value()];
    return std::span<const Sequence>(m_sequences).subspan(range.begin, range.count);
}

std::vector<Conflict> BindingIndex::conflicts() const {
    std::vector<Conflict> out;
    // Sequences bound to several actions.
    for (std::size_t i = 0; i < m_singleKeys.size();) {
        std::size_t j = i;
        std::vector<ActionId> actions;
        while (j < m_singleKeys.size() && m_singleKeys[j] == m_singleKeys[i]) {
            appendUnique(actions, m_singleActions[j++]);
        }
        if (actions.size() > 1) {
            out.push_back(Conflict{Sequence::of(m_singleKeys[i]), std::move(actions), false});
        }
        i = j;
    }
    for (std::size_t i = 0; i < m_doubleKeys.size();) {
        std::size_t j = i;
        std::vector<ActionId> actions;
        while (j < m_doubleKeys.size() && m_doubleKeys[j] == m_doubleKeys[i]) {
            appendUnique(actions, m_doubleActions[j++]);
        }
        if (actions.size() > 1) {
            out.push_back(
                Conflict{Sequence::of(m_doubleKeys[i].first, m_doubleKeys[i].second), std::move(actions), false});
        }
        i = j;
    }
    for (std::size_t g = 0; g < m_gestureActions.size(); ++g) {
        std::vector<ActionId> actions;
        for (const ActionId id : m_gestureActions[g]) {
            appendUnique(actions, id);
        }
        if (actions.size() > 1) {
            out.push_back(
                Conflict{Sequence::of(g == 0 ? Gesture::DoubleShift : Gesture::DoubleCtrl), std::move(actions), false});
        }
    }
    // Single strokes that are also the prefix of a two-stroke binding (the prefix wins).
    for (std::size_t i = 0; i < m_singleKeys.size();) {
        std::size_t j = i;
        while (j < m_singleKeys.size() && m_singleKeys[j] == m_singleKeys[i]) {
            ++j;
        }
        const std::span<const ActionId> continuations = prefixed(m_singleKeys[i]);
        if (!continuations.empty()) {
            std::vector<ActionId> actions;
            for (std::size_t k = i; k < j; ++k) {
                appendUnique(actions, m_singleActions[k]);
            }
            for (const ActionId id : continuations) {
                appendUnique(actions, id);
            }
            out.push_back(Conflict{Sequence::of(m_singleKeys[i]), std::move(actions), true});
        }
        i = j;
    }
    std::ranges::sort(out, [](const Conflict& a, const Conflict& b) {
        return std::tuple(a.sequence, a.prefixConflict) < std::tuple(b.sequence, b.prefixConflict);
    });
    return out;
}

std::vector<Sequence> BindingIndex::unreachable(const LayoutTable& layout) const {
    std::vector<Sequence> out;
    for (const Sequence& s : m_sequences) {
        if (!reachableOn(s, layout)) {
            out.push_back(s);
        }
    }
    std::ranges::sort(out);
    const auto [first, last] = std::ranges::unique(out);
    out.erase(first, last);
    return out;
}

std::optional<Sequence> BindingIndex::primaryShortcut(ActionId id, const LayoutTable& layout) const {
    for (const Sequence& s : shortcutsFor(id)) {
        if (reachableOn(s, layout)) {
            return s;
        }
    }
    return std::nullopt;
}

} // namespace frost::input
