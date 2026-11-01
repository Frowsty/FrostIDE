// SPDX-License-Identifier: Apache-2.0
//
// ChangeCoalescer (change_coalescer.hpp): the coalescing rules of file-watch batches (ADR 0020).

#include "change_coalescer.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace frost::io::detail {
namespace {

[[nodiscard]] bool knownNonDirectory(const std::optional<FileType>& type, bool nonDirectory) noexcept {
    return nonDirectory || (type.has_value() && *type != FileType::Directory);
}

} // namespace

bool isStrictlyBelow(std::string_view path, std::string_view ancestor, char separator) noexcept {
    if (path.size() <= ancestor.size() || !path.starts_with(ancestor)) {
        return false;
    }
    if (!ancestor.empty() && ancestor.back() == separator) {
        return true; // A root such as "C:\" or "/".
    }
    return path[ancestor.size()] == separator;
}

bool isAtOrBelow(std::string_view path, std::string_view ancestor, char separator) noexcept {
    return path == ancestor || isStrictlyBelow(path, ancestor, separator);
}

ChangeCoalescer::ChangeCoalescer(std::string root, char separator, std::size_t maxPendingPaths)
    : m_root(std::move(root)), m_separator(separator), m_maxPendingPaths(maxPendingPaths) {
    FROST_PRE(maxPendingPaths >= 1);
    FROST_PRE(!m_root.empty());
}

bool ChangeCoalescer::empty() const noexcept { return m_states.empty() && m_rescans.empty() && m_cookies.empty(); }

std::size_t ChangeCoalescer::pendingPaths() const noexcept { return m_states.size() + m_rescans.size(); }

std::string_view ChangeCoalescer::parentOf(std::string_view path) const noexcept {
    if (!isStrictlyBelow(path, m_root, m_separator)) {
        return {}; // The root itself (or outside it): no parent inside the watch.
    }
    const std::size_t at = path.rfind(m_separator);
    if (at == std::string_view::npos || at < m_root.size()) {
        return m_root; // Directly below a root that ends in a separator ("C:\a").
    }
    return path.substr(0, at);
}

bool ChangeCoalescer::underPendingRescan(std::string_view path) const {
    if (m_rescans.empty()) {
        return false;
    }
    for (std::string_view p = path; !p.empty(); p = parentOf(p)) {
        if (m_rescans.contains(p)) {
            return true;
        }
    }
    return false;
}

// Whether a strict ancestor's entry is not its start entry, so start facts observed below it are relative
// to the new directory (created, replaced or moved in) rather than to the consumer's pre-batch tree.
bool ChangeCoalescer::replacedFrame(std::string_view key) const {
    for (std::string_view p = parentOf(key); !p.empty(); p = parentOf(p)) {
        const auto it = m_states.find(p);
        if (it == m_states.end()) {
            continue;
        }
        const State& state = it->second;
        if (!state.exists || state.identityChanged || state.start != Start::Present || state.origin.has_value()) {
            return true; // Removed, replaced, created or moved in during the batch.
        }
    }
    return false;
}

std::pair<ChangeCoalescer::State*, bool> ChangeCoalescer::stateFor(const std::string& key, Start start) {
    const auto found = m_states.find(key);
    if (found != m_states.end()) {
        return {&found->second, false};
    }
    State state;
    state.start = start;
    state.startKey = key;
    state.preBatchFrame = !replacedFrame(key);
    if (start == Start::Unknown) {
        state.identityChanged = true;
    }
    auto [it, inserted] = m_states.emplace(key, std::move(state));
    FROST_ASSERT(inserted);
    return {&it->second, true};
}

bool ChangeCoalescer::existsAtEnd(std::string_view path) const {
    const auto it = m_states.find(path);
    if (it != m_states.end() && !it->second.exists) {
        return false;
    }
    for (std::string_view p = parentOf(path); !p.empty(); p = parentOf(p)) {
        const auto ancestor = m_states.find(p);
        if (ancestor == m_states.end()) {
            continue;
        }
        const State& state = ancestor->second;
        if (!state.exists) {
            return false;
        }
        // A new or replaced directory holds only what events created in it, and those have states.
        if (it == m_states.end() && (state.start != Start::Present || state.identityChanged || state.origin)) {
            return false;
        }
    }
    return true;
}

// Every pending state strictly below `path` no longer exists (its directory was removed or moved away).
void ChangeCoalescer::markBelowGone(std::string_view path) {
    std::string prefix(path);
    if (prefix.back() != m_separator) {
        prefix += m_separator;
    }
    for (auto it = m_states.lower_bound(prefix); it != m_states.end() && it->first.starts_with(prefix); ++it) {
        it->second.exists = false;
    }
}

// Forgets everything strictly below `path` (and at it when `inclusive`): states, Rescan roots and the
// cookies of unpaired moves whose source lies there.
void ChangeCoalescer::dropBelow(std::string_view path, bool inclusive) {
    const auto inScope = [&](std::string_view key) {
        return inclusive ? isAtOrBelow(key, path, m_separator) : isStrictlyBelow(key, path, m_separator);
    };
    std::string prefix(path);
    if (prefix.back() != m_separator) {
        prefix += m_separator;
    }
    if (inclusive) {
        m_states.erase(std::string(path));
        m_rescans.erase(std::string(path));
    }
    for (auto it = m_states.lower_bound(prefix); it != m_states.end() && it->first.starts_with(prefix);) {
        it = m_states.erase(it);
    }
    for (auto it = m_rescans.lower_bound(prefix); it != m_rescans.end() && it->starts_with(prefix);) {
        it = m_rescans.erase(it);
    }
    std::erase_if(m_cookies, [&](const auto& entry) { return inScope(entry.second); });
}

/*
 * A directory moved from `from` to `to`: pending states, Rescan roots and unpaired-move sources strictly
 * below `from` move below `to`, keeping their start facts (relative to the subtree the consumer places
 * at `to`). An origin inside the moved directory refers to a path the consumer can only reach after the
 * placement, so such nested renames degrade (the origin is dropped and rule 6 applies).
 */
void ChangeCoalescer::repath(std::string_view from, std::string_view to) {
    std::string prefix(from);
    if (prefix.back() != m_separator) {
        prefix += m_separator;
    }
    const auto moved = [&](std::string_view key) { return std::string(to) + std::string(key.substr(from.size())); };
    std::vector<StateMap::node_type> nodes;
    for (auto it = m_states.lower_bound(prefix); it != m_states.end() && it->first.starts_with(prefix);) {
        auto next = std::next(it);
        nodes.push_back(m_states.extract(it));
        it = next;
    }
    for (StateMap::node_type& node : nodes) {
        node.key() = moved(node.key());
        if (node.mapped().origin && isAtOrBelow(*node.mapped().origin, from, m_separator)) {
            node.mapped().origin.reset();
        }
        m_states.insert(std::move(node));
    }
    std::vector<std::string> rescans;
    for (auto it = m_rescans.lower_bound(prefix); it != m_rescans.end() && it->starts_with(prefix);) {
        rescans.push_back(moved(*it));
        it = m_rescans.erase(it);
    }
    m_rescans.insert(rescans.begin(), rescans.end());
    for (auto& [cookie, key] : m_cookies) {
        if (isStrictlyBelow(key, from, m_separator)) {
            key = moved(key);
        }
    }
}

// Moves still waiting for their MovedTo when any other event arrives (or take() runs) went outside the
// watch: their sources' subtrees are gone.
void ChangeCoalescer::resolveUnpaired() {
    for (const auto& [cookie, key] : m_cookies) {
        markBelowGone(key);
    }
    m_cookies.clear();
}

void ChangeCoalescer::collapse() {
    m_states.clear();
    m_cookies.clear();
    m_rescans.clear();
    m_rescans.insert(m_root);
}

namespace {

// Records an event's type as the end type: `sameEntry` keeps a known type when the event has none.
template<class State>
void setEnd(State& state, const RawEvent& event, bool sameEntry) {
    if (event.type) {
        state.endType = event.type;
        state.endNonDirectory = *event.type != FileType::Directory;
        return;
    }
    if (!sameEntry) {
        state.endType.reset();
        state.endNonDirectory = event.nonDirectory;
        return;
    }
    if (event.nonDirectory) {
        state.endNonDirectory = true;
        if (state.endType == FileType::Directory) {
            state.endType.reset();
        }
    }
}

} // namespace

void ChangeCoalescer::onCreated(const RawEvent& event) {
    auto [state, created] = stateFor(event.path, Start::Absent);
    if (created) {
        setEnd(*state, event, /*sameEntry=*/false);
        return;
    }
    /*
     * Gone, or still there: a duplicate (a setup listing) or a replacement whose removal was never reported
     * (RDCW reports a move onto an existing name from another directory as just ADDED; inotify a rename-over
     * as just IN_MOVED_TO). The entry may be another one, so it is treated as replaced, which costs at most
     * a re-read.
     */
    if (!state->exists) {
        // Recreated: the gone states below belong to the old entry, and must not move with the new one.
        dropBelow(event.path, /*inclusive=*/false);
    }
    state->exists = true;
    if (state->start != Start::Absent) {
        state->identityChanged = true;
    }
    state->origin.reset();
    setEnd(*state, event, /*sameEntry=*/false);
}

void ChangeCoalescer::onModified(const RawEvent& event) {
    auto [state, created] = stateFor(event.path, Start::Present);
    if (created) {
        state->startType = event.type;
        state->startNonDirectory = knownNonDirectory(event.type, event.nonDirectory);
        setEnd(*state, event, /*sameEntry=*/false);
        return;
    }
    if (!state->exists) {
        dropBelow(event.path, /*inclusive=*/false); // Recreated (see onCreated).
        state->exists = true;
        if (state->start != Start::Absent) {
            state->identityChanged = true;
        }
        state->origin.reset();
        setEnd(*state, event, /*sameEntry=*/false);
        return;
    }
    setEnd(*state, event, /*sameEntry=*/true);
}

void ChangeCoalescer::onRemoved(const RawEvent& event, bool moved) {
    auto [state, created] = stateFor(event.path, Start::Present);
    if (created) {
        state->startType = event.type;
        state->startNonDirectory = knownNonDirectory(event.type, event.nonDirectory);
    }
    state->exists = false;
    if (event.type) {
        state->endType = event.type;
        state->endNonDirectory = *event.type != FileType::Directory;
    } else if (event.nonDirectory) {
        state->endNonDirectory = true;
        if (state->endType == FileType::Directory) {
            state->endType.reset();
        }
    }
    if (moved && event.cookie != 0) {
        m_cookies[event.cookie] = event.path; // Its subtree is re-pathed or gone once the pair is known.
    } else {
        markBelowGone(event.path);
    }
}

void ChangeCoalescer::onMovedTo(const RawEvent& event) {
    const auto unpaired = [&] {
        dropBelow(event.path, /*inclusive=*/false); // Whatever was below the old entry was replaced.
        auto [state, created] = stateFor(event.path, Start::Unknown);
        if (created) {
            state->firstWasMovedToNonDirectory = knownNonDirectory(event.type, event.nonDirectory);
        } else {
            if (state->start != Start::Absent) {
                state->identityChanged = true;
            }
            state->origin.reset();
        }
        state->exists = true;
        setEnd(*state, event, /*sameEntry=*/false);
    };
    const auto cookie = event.cookie != 0 ? m_cookies.find(event.cookie) : m_cookies.end();
    if (cookie == m_cookies.end()) {
        unpaired();
        return;
    }
    const std::string sourceKey = cookie->second;
    m_cookies.erase(cookie);
    const auto source = m_states.find(sourceKey);
    if (source == m_states.end() || sourceKey == event.path || isAtOrBelow(event.path, sourceKey, m_separator) ||
        isAtOrBelow(sourceKey, event.path, m_separator)) {
        if (source != m_states.end()) {
            markBelowGone(sourceKey);
        }
        unpaired();
        return;
    }
    const State& from = source->second;
    // The moving entry's start path: inherited, or the source's own when the entry there is still the
    // start entry (not one that was moved or created onto it during the batch).
    std::optional<std::string> origin = from.origin;
    if (!origin && from.start == Start::Present && from.preBatchFrame && !from.identityChanged) {
        origin = from.startKey;
    }
    const std::optional<FileType> type = event.type ? event.type : from.endType;
    const bool nonDirectory = knownNonDirectory(type, event.nonDirectory || from.endNonDirectory);
    dropBelow(event.path, /*inclusive=*/false); // The rename replaces the destination's old subtree.
    repath(sourceKey, event.path);
    auto [state, created] = stateFor(event.path, Start::Unknown);
    if (created) {
        state->firstWasMovedToNonDirectory = nonDirectory;
    } else if (state->start != Start::Absent) {
        state->identityChanged = true;
    }
    state->exists = true;
    state->origin = std::move(origin);
    state->endType = type;
    state->endNonDirectory = nonDirectory;
}

void ChangeCoalescer::onOverflow(std::string_view path) {
    if (underPendingRescan(path)) {
        return;
    }
    dropBelow(path, /*inclusive=*/true);
    m_rescans.insert(std::string(path));
}

void ChangeCoalescer::push(const RawEvent& event) {
    if (!isAtOrBelow(event.path, m_root, m_separator) || m_rescans.contains(m_root)) {
        return; // Outside the watch, or everything is pending as Rescan(root) already.
    }
    const bool pairs = event.kind == RawKind::MovedTo && event.cookie != 0 && m_cookies.contains(event.cookie);
    if (!pairs) {
        resolveUnpaired();
    } else if (m_cookies.size() > 1) {
        // Only this event's own move can still pair.
        const std::string source = m_cookies.at(event.cookie);
        m_cookies.erase(event.cookie);
        resolveUnpaired();
        m_cookies.emplace(event.cookie, source);
    }
    if (event.kind != RawKind::Overflow && underPendingRescan(event.path)) {
        if (pairs) {
            // The source moved somewhere a pending Rescan covers: for its own path that is a removal.
            const std::string source = m_cookies.at(event.cookie);
            m_cookies.erase(event.cookie);
            markBelowGone(source);
        }
        return;
    }
    switch (event.kind) {
    case RawKind::Created: onCreated(event); break;
    case RawKind::Modified: onModified(event); break;
    case RawKind::Removed: onRemoved(event, /*moved=*/false); break;
    case RawKind::MovedFrom: onRemoved(event, /*moved=*/true); break;
    case RawKind::MovedTo: onMovedTo(event); break;
    case RawKind::Overflow: onOverflow(event.path); break;
    }
    if (m_states.size() + m_rescans.size() > m_maxPendingPaths) {
        collapse();
    }
}

std::vector<FileChange> ChangeCoalescer::net(const std::set<std::string, std::less<>>& forcedDegrade) const {
    using Paths = std::set<std::string, std::less<>>;
    std::vector<FileChange> out;
    std::map<std::string, int, std::less<>> originUses;
    for (const auto& [key, state] : m_states) {
        if (state.exists && state.origin && !underPendingRescan(key)) {
            ++originUses[*state.origin];
        }
    }
    Paths rescanKeys;
    Paths removedKeys;
    Paths rereadKeys; // Re-read with their subtree: Created, Renamed destinations, non-directory Modified.
    Paths silentKeys; // Gone sources of reported Renamed entries.
    Paths reportedOrigins;
    const auto ancestorBlocks = [&](std::string_view key) {
        for (std::string_view p = parentOf(key); !p.empty(); p = parentOf(p)) {
            if (rescanKeys.contains(p)) {
                return true;
            }
            const auto it = m_states.find(p);
            if (it != m_states.end() && !it->second.exists) {
                return true;
            }
        }
        return false;
    };
    const auto renamedIsValid = [&](const std::string& key, const State& state) {
        const std::string& origin = *state.origin;
        if (forcedDegrade.contains(key) || origin == key) {
            return false;
        }
        const auto uses = originUses.find(origin);
        if (uses == originUses.end() || uses->second != 1) {
            return false;
        }
        return !underPendingRescan(origin) && !existsAtEnd(origin) && !ancestorBlocks(key);
    };
    const auto classify = [](const State& state) {
        if (state.start == Start::Absent) {
            return FileChangeKind::Created;
        }
        const bool typeChanged = state.startType && state.endType && *state.startType != *state.endType;
        if (!state.identityChanged && state.start == Start::Present && !typeChanged) {
            return FileChangeKind::Modified;
        }
        const bool endNonDirectory = knownNonDirectory(state.endType, state.endNonDirectory);
        const bool noDirectoryAtStart =
            knownNonDirectory(state.startType, state.startNonDirectory) || state.firstWasMovedToNonDirectory;
        return endNonDirectory && noDirectoryAtStart ? FileChangeKind::Modified : FileChangeKind::Rescan;
    };

    // Existing entries in sorted order, so every ancestor is decided before its descendants.
    std::vector<std::pair<const std::string*, const State*>> gone;
    for (const auto& [key, state] : m_states) {
        if (underPendingRescan(key)) {
            continue;
        }
        if (!state.exists) {
            gone.emplace_back(&key, &state);
            continue;
        }
        if (state.origin && renamedIsValid(key, state)) {
            out.push_back(FileChange{FileChangeKind::Renamed, key, *state.origin, state.endType});
            reportedOrigins.insert(*state.origin);
            rereadKeys.insert(key);
            continue;
        }
        const FileChangeKind kind = classify(state);
        if (kind == FileChangeKind::Rescan) {
            rescanKeys.insert(key);
        } else if (kind == FileChangeKind::Created ||
                   (kind == FileChangeKind::Modified && knownNonDirectory(state.endType, state.endNonDirectory))) {
            rereadKeys.insert(key);
        }
        out.push_back(FileChange{kind, key, {}, state.endType});
    }
    for (const auto& [key, state] : gone) {
        // Also when the entry was created in this batch: a consumer that applied the previous batch late
        // may have read it (lagged re-reads), so only the source of a reported Renamed stays silent.
        if (state->preBatchFrame && reportedOrigins.contains(state->startKey)) {
            silentKeys.insert(*key); // The consumer lifts its whole subtree away.
            continue;
        }
        out.push_back(FileChange{FileChangeKind::Removed, *key, {}, state->endType});
        removedKeys.insert(*key);
    }
    for (const std::string& rescan : m_rescans) {
        out.push_back(FileChange{FileChangeKind::Rescan, rescan, {}, std::nullopt});
        rescanKeys.insert(rescan);
    }
    /*
     * Removed and Rescan subsume the entries below them. The consumer re-reads Created entries, Renamed
     * destinations and non-directory Modified entries with their subtree (dropping what it held below a
     * path that turns out to be a file), and lifts a Renamed source's whole subtree away, so a Removed
     * below one of those is redundant.
     */
    std::erase_if(out, [&](const FileChange& change) {
        for (std::string_view p = parentOf(change.path); !p.empty(); p = parentOf(p)) {
            if (rescanKeys.contains(p) || removedKeys.contains(p) ||
                (change.kind == FileChangeKind::Removed && (rereadKeys.contains(p) || silentKeys.contains(p)))) {
                return true;
            }
        }
        return false;
    });
    std::ranges::sort(out, {}, &FileChange::path);
    return out;
}

std::vector<FileChange> ChangeCoalescer::take() {
    resolveUnpaired();
    std::set<std::string, std::less<>> forced;
    std::vector<FileChange> result;
    for (;;) {
        result = net(forced);
        // No path may appear in two entries (a Renamed's source counts): degrade the Renamed involved.
        std::map<std::string_view, int> uses;
        for (const FileChange& change : result) {
            ++uses[change.path];
            if (change.kind == FileChangeKind::Renamed) {
                ++uses[change.oldPath];
            }
        }
        bool degraded = false;
        for (const FileChange& change : result) {
            if (change.kind == FileChangeKind::Renamed && (uses[change.path] > 1 || uses[change.oldPath] > 1)) {
                degraded = forced.insert(change.path).second || degraded;
            }
        }
        if (!degraded) {
            break;
        }
    }
    m_states.clear();
    m_rescans.clear();
    m_cookies.clear();
    return result;
}

} // namespace frost::io::detail
