// SPDX-License-Identifier: Apache-2.0
//
// ChangeCoalescer: turns a stream of raw OS events (RDCW, inotify, polling diffs, MemFs) into the net
// FileChange entries of one batch (frost/io/file_watcher.hpp; ADR 0020). Pure and single-threaded;
// WatchQueue owns one per watch under its lock. Asio-free, so the unit tests include it.
//
/*
 * Per-path state, keyed by the path the entry has now (std::map, so subtrees are contiguous):
 *   start      what was at the key when the batch began: Absent (first event Created), Present
 *              (Modified, Removed, MovedFrom) or Unknown (MovedTo: a rename-over has no removal);
 *   exists     whether something is there after the last event;
 *   types      the start and end types when known, or at least "known not a directory";
 *   identityChanged  the entry there now may not be the start entry (recreated, moved onto, or reported
 *              Created while it existed: a replacement whose removal the backend did not report);
 *   origin     the start-of-batch path whose entry now lives here (set only by a paired move from a
 *              source whose start was Present, in the pre-batch frame);
 *   startKey and preBatchFrame  where the start facts were observed: a directory move re-paths the
 *              pending states below its source to the destination, which keeps their start facts
 *              relative to the source subtree the consumer will place there; a state first seen below a
 *              created, replaced or moved-in directory is relative to that new directory and can never be
 *              an origin. A recreated entry drops the gone states below it (they belong to the old one).
 *
 * Net per path at take():
 *   1. under a pending Rescan subtree: dropped;
 *   2. not existing: the origin of a reported Renamed: nothing; otherwise Removed with the last known
 *      type, also when the entry was created during the batch (start Absent): a consumer that applied the
 *      previous batch late re-read the disk after this batch began and may have seen it;
 *   3. existing, with an origin that does not exist at the end, is used by no other entry, is not under
 *      a pending Rescan and whose destination is not below a Removed or Rescan entry: Renamed(origin -> path);
 *   4. existing, start Absent: Created;
 *   5. existing, identity unchanged: Modified;
 *   6. existing, identity changed (recreated, moved onto, an unpaired MovedTo, a degraded Renamed):
 *      Modified if the end type is known not to be a directory and no directory can have been at the
 *      path at batch start (the start type is known not to be one, or the first event was a MovedTo of a
 *      non-directory: rename(2) and MoveFileExW cannot replace a directory with a non-directory);
 *      otherwise Rescan.
 * Removed and Rescan entries subsume the entries below them; Created entries, Renamed sources and
 * destinations and Modified non-directories subsume the Removed entries below them (the consumer lifts
 * or re-reads their subtree, so such a Removed is redundant). A Renamed whose source path would also be
 * used by another entry degrades (its destination takes rule 6, its source its own net state).
 *
 * Pairing: MovedFrom records cookie -> source key; the matching MovedTo pairs them, re-paths every
 * pending state below the source to the destination (after dropping the destination's old subtree,
 * which a rename replaces), and gives the destination the source's origin, or the source's own start
 * path if its start was Present and its identity unchanged. A MovedFrom that is not followed by its MovedTo (any other
 * event comes first, or take() runs) is a removal. Overflow(p) adds Rescan(p); later events at or below a pending
 * Rescan are dropped; more than maxPendingPaths states collapse everything into Rescan(root).
 *
 * Complexity: O(d log P) per event for path depth d and P pending paths; a directory re-path or removal
 * is O(k log P) for the k pending states below it; take() is O(P d log P).
 */
#pragma once

#include "frost/io/file_system.hpp"
#include "frost/io/file_watcher.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io::detail {

enum class RawKind : std::uint8_t { Created, Modified, Removed, MovedFrom, MovedTo, Overflow };

// One OS event, already converted to an absolute path in the watch root's style.
struct RawEvent {
    RawKind kind = RawKind::Modified;
    std::string path{};
    std::optional<FileType> type{}; // The exact type, when known.
    bool nonDirectory = false;      // Known not to be a directory although the exact type is unknown (inotify).
    std::uint64_t cookie = 0;       // Pairs MovedFrom with MovedTo; 0 for every other kind.

    friend bool operator==(const RawEvent&, const RawEvent&) = default;
};

// Separator-aware prefix tests ("/a/bc" is not below "/a/b"; roots ending in a separator, "C:\" and
// "/", work). Pure; O(length).
[[nodiscard]] bool isStrictlyBelow(std::string_view path, std::string_view ancestor, char separator) noexcept;
[[nodiscard]] bool isAtOrBelow(std::string_view path, std::string_view ancestor, char separator) noexcept;

class ChangeCoalescer {
public:
    // `root`: the normalised watch root; `separator`: '\\' or '/'; `maxPendingPaths` >= 1.
    ChangeCoalescer(std::string root, char separator, std::size_t maxPendingPaths);

    // Folds one event in. Events outside the root are ignored.
    void push(const RawEvent& event);

    // True when nothing is pending (no path state, no Rescan, no unpaired move).
    [[nodiscard]] bool empty() const noexcept;
    // Paths with pending state plus pending Rescan roots.
    [[nodiscard]] std::size_t pendingPaths() const noexcept;

    // The net entries, sorted by path, then clears everything. May be empty when the pending set nets to
    // nothing (created and deleted again, say).
    [[nodiscard]] std::vector<FileChange> take();

private:
    enum class Start : std::uint8_t { Absent, Present, Unknown };
    struct State {
        Start start = Start::Present;
        bool exists = true;
        std::optional<FileType> startType{};
        bool startNonDirectory = false;
        std::optional<FileType> endType{};
        bool endNonDirectory = false;
        bool identityChanged = false;
        bool firstWasMovedToNonDirectory = false;
        bool preBatchFrame = true;
        std::string startKey{};
        std::optional<std::string> origin{};
    };
    using StateMap = std::map<std::string, State, std::less<>>;

    [[nodiscard]] bool underPendingRescan(std::string_view path) const;
    [[nodiscard]] std::string_view parentOf(std::string_view path) const noexcept;
    // The state at `key`, created with `start` if missing (second: whether it was created).
    std::pair<State*, bool> stateFor(const std::string& key, Start start);
    [[nodiscard]] bool replacedFrame(std::string_view key) const;
    [[nodiscard]] bool existsAtEnd(std::string_view path) const;
    void markBelowGone(std::string_view path);
    void dropBelow(std::string_view path, bool inclusive);
    void repath(std::string_view from, std::string_view to);
    void resolveUnpaired();
    void collapse();
    void onCreated(const RawEvent& event);
    void onModified(const RawEvent& event);
    void onRemoved(const RawEvent& event, bool moved);
    void onMovedTo(const RawEvent& event);
    void onOverflow(std::string_view path);
    [[nodiscard]] std::vector<FileChange> net(const std::set<std::string, std::less<>>& forcedDegrade) const;

    std::string m_root;
    char m_separator;
    std::size_t m_maxPendingPaths;
    StateMap m_states;
    std::set<std::string, std::less<>> m_rescans;   // Prefix-free: none lies below another.
    std::map<std::uint64_t, std::string> m_cookies; // MovedFrom cookie -> source key, until paired.
};

} // namespace frost::io::detail
