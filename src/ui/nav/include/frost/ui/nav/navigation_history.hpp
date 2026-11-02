// SPDX-License-Identifier: Apache-2.0
//
// The navigation models of the shell (PLAN.md section 7 M1 "Ctrl+E, recent locations, back/forward";
// ADR 0042): Place, a (document, caret) position; NavigationHistory, the back/forward stacks of
// places recorded on jumps and big caret moves; RecentFiles (Ctrl+E) and RecentLocations
// (Ctrl+Shift+E). Qt-free plain values; the UI thread owns them.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::ui::nav {

// A caret position in a document: the canonical path and a 1-based line and column.
struct Place {
    std::string path;
    int line = 1;
    int column = 1;
    friend bool operator==(const Place&, const Place&) = default;
};

// True when `a` and `b` are in one file within `lines` lines of each other (one history entry).
[[nodiscard]] bool near(const Place& a, const Place& b, int lines) noexcept;

/*
 * Back/forward over places, as IntelliJ's Back and Forward (Ctrl+Alt+Left/Right). A jump records
 * the place it left (recordJump): the back stack gains it (a place near the top replaces the top)
 * and the forward stack is cleared. back(current) moves `current` to the forward stack and returns
 * the place to go to; forward(current) is the reverse. Big caret moves count as jumps
 * (noteCaretMove). Both stacks keep at most kMaxEntries places (the oldest go first). O(1) per call
 * except the cap's O(n) erase.
 */
class NavigationHistory {
public:
    static constexpr std::size_t kMaxEntries = 100;
    // Places closer than this in one file are one entry.
    static constexpr int kNearLines = 3;
    // A caret move of at least this many lines in one file is recorded like a jump.
    static constexpr int kBigMoveLines = 10;

    // Records the place a jump left.
    void recordJump(Place from);
    // A caret moved from `from` to `to` without a jump: recorded when it is a big move in one file.
    // Returns whether it was recorded.
    bool noteCaretMove(const Place& from, const Place& to);
    // The place to go back to (nullopt when there is none); `current` goes on the forward stack.
    [[nodiscard]] std::optional<Place> back(const Place& current);
    // The place to go forward to (nullopt when there is none); `current` goes on the back stack.
    [[nodiscard]] std::optional<Place> forward(const Place& current);

    [[nodiscard]] bool canGoBack() const noexcept { return !m_back.empty(); }
    [[nodiscard]] bool canGoForward() const noexcept { return !m_forward.empty(); }
    // Oldest first.
    [[nodiscard]] std::span<const Place> backStack() const noexcept { return m_back; }
    [[nodiscard]] std::span<const Place> forwardStack() const noexcept { return m_forward; }
    // Drops every place in `path` (the file was deleted or the project closed).
    void forget(std::string_view path);
    void clear() noexcept;

private:
    static void push(std::vector<Place>& stack, Place place);

    std::vector<Place> m_back;
    std::vector<Place> m_forward;
};

/*
 * The most recently used files, most recent first, unique, at most kMaxEntries. touch() moves a
 * path to the front. O(n) per call.
 */
class RecentFiles {
public:
    static constexpr std::size_t kMaxEntries = 50;

    void touch(std::string path);
    void remove(std::string_view path);
    void assign(std::vector<std::string> paths);
    [[nodiscard]] std::span<const std::string> paths() const noexcept { return m_paths; }
    void clear() noexcept { m_paths.clear(); }

private:
    std::vector<std::string> m_paths;
};

/*
 * Recently visited places, most recent first: a place near an existing entry (within
 * NavigationHistory::kNearLines in the same file) replaces it. At most kMaxEntries. O(n) per call.
 */
class RecentLocations {
public:
    static constexpr std::size_t kMaxEntries = 50;

    void touch(Place place);
    void forget(std::string_view path);
    void assign(std::vector<Place> places);
    [[nodiscard]] std::span<const Place> places() const noexcept { return m_places; }
    void clear() noexcept { m_places.clear(); }

private:
    std::vector<Place> m_places;
};

} // namespace frost::ui::nav
