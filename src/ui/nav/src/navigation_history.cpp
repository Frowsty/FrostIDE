// SPDX-License-Identifier: Apache-2.0
//
// NavigationHistory, RecentFiles and RecentLocations (frost/ui/nav/navigation_history.hpp).

#include "frost/ui/nav/navigation_history.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace frost::ui::nav {

bool isNear(const Place& a, const Place& b, int lines) noexcept {
    return a.path == b.path && std::abs(a.line - b.line) < lines;
}

void NavigationHistory::push(std::vector<Place>& stack, Place place) {
    if (!stack.empty() && isNear(stack.back(), place, kNearLines)) {
        stack.back() = std::move(place);
        return;
    }
    stack.push_back(std::move(place));
    if (stack.size() > kMaxEntries) {
        stack.erase(stack.begin());
    }
}

void NavigationHistory::recordJump(Place from) {
    if (from.path.empty()) {
        return;
    }
    push(m_back, std::move(from));
    m_forward.clear();
}

bool NavigationHistory::noteCaretMove(const Place& from, const Place& to) {
    if (from.path.empty() || from.path != to.path || std::abs(from.line - to.line) < kBigMoveLines) {
        return false;
    }
    recordJump(from);
    return true;
}

std::optional<Place> NavigationHistory::back(const Place& current) {
    if (m_back.empty()) {
        return std::nullopt;
    }
    Place target = std::move(m_back.back());
    m_back.pop_back();
    // Going back from a place that is the target itself (no move since) skips to the one before.
    if (isNear(target, current, kNearLines) && !m_back.empty()) {
        target = std::move(m_back.back());
        m_back.pop_back();
    }
    if (!current.path.empty()) {
        push(m_forward, current);
    }
    return target;
}

std::optional<Place> NavigationHistory::forward(const Place& current) {
    if (m_forward.empty()) {
        return std::nullopt;
    }
    Place target = std::move(m_forward.back());
    m_forward.pop_back();
    if (!current.path.empty()) {
        push(m_back, current);
    }
    return target;
}

void NavigationHistory::forget(std::string_view path) {
    std::erase_if(m_back, [path](const Place& place) { return place.path == path; });
    std::erase_if(m_forward, [path](const Place& place) { return place.path == path; });
}

void NavigationHistory::clear() noexcept {
    m_back.clear();
    m_forward.clear();
}

void RecentFiles::touch(std::string path) {
    if (path.empty()) {
        return;
    }
    std::erase(m_paths, path);
    m_paths.insert(m_paths.begin(), std::move(path));
    if (m_paths.size() > kMaxEntries) {
        m_paths.resize(kMaxEntries);
    }
}

void RecentFiles::remove(std::string_view path) {
    std::erase_if(m_paths, [path](const std::string& entry) { return entry == path; });
}

void RecentFiles::assign(std::vector<std::string> paths) {
    m_paths.clear();
    // Keep the given order (most recent first) while dropping duplicates and empties.
    for (std::string& path : paths) {
        if (!path.empty() && std::ranges::find(m_paths, path) == m_paths.end() && m_paths.size() < kMaxEntries) {
            m_paths.push_back(std::move(path));
        }
    }
}

void RecentLocations::touch(Place place) {
    if (place.path.empty()) {
        return;
    }
    std::erase_if(m_places,
                  [&place](const Place& entry) { return isNear(entry, place, NavigationHistory::kNearLines); });
    m_places.insert(m_places.begin(), std::move(place));
    if (m_places.size() > kMaxEntries) {
        m_places.resize(kMaxEntries);
    }
}

void RecentLocations::forget(std::string_view path) {
    std::erase_if(m_places, [path](const Place& place) { return place.path == path; });
}

void RecentLocations::assign(std::vector<Place> places) {
    m_places.clear();
    for (Place& place : places) {
        if (!place.path.empty() && m_places.size() < kMaxEntries) {
            m_places.push_back(std::move(place));
        }
    }
}

} // namespace frost::ui::nav
