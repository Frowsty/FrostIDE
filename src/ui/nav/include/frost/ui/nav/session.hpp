// SPDX-License-Identifier: Apache-2.0
//
// Session restore (PLAN.md section 7 M1 "session restore"; ADR 0042): SessionState, what the shell
// remembers per project (open tabs in order with pinned state, caret and scroll; the active tab; the
// tool-window layout; recent files and locations), its JSON form, and SessionStore, which keeps one
// file per project plus an index naming the last project in the user's data directory.
/*
 * Files (version 1). <directory>/index.json: {"version": 1, "lastProject": "<root>",
 * "recentProjects": ["<root>", ...]}. <directory>/project-<16 hex>.json, the hex being the FNV-1a
 * 64-bit hash of the canonical project root: {"version": 1, "root": ..., "tabs": [{"path", "pinned",
 * "line", "column", "top"}], "active": <index or -1>, "toolWindows": [{"id", "visible", "size"}],
 * "recentFiles": [...], "recentLocations": [{"path", "line", "column"}], and optional
 * "cmakeTargets": [{"profile", "target"}]}. Unknown keys are ignored;
 * a newer version or malformed JSON is Corrupt or Unsupported, and the shell then starts empty.
 * Writes go through IFileSystem::writeFileAtomic. Thread safety: none (the UI thread).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/ui/nav/navigation_history.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::ui::nav {

// One open tab.
struct SessionTab {
    std::string path;
    bool pinned = false;
    int line = 1;
    int column = 1;
    int top = 0; // The first visible line, 0-based.
    std::uint64_t foldTextHash = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> folds{};
    int group = 0;
    bool groupActive = false;
    friend bool operator==(const SessionTab&, const SessionTab&) = default;
};

// One tool window's remembered state (mirrors ui::ToolWindowLayout without depending on Qt).
struct SessionToolWindow {
    std::string id;
    bool visible = false;
    int size = 0;
    friend bool operator==(const SessionToolWindow&, const SessionToolWindow&) = default;
};

struct SessionState {
    std::string root; // The canonical project root the state belongs to.
    std::vector<SessionTab> tabs;
    int active = -1;          // Index into tabs, or -1.
    std::string editorLayout; // Splitter tree; empty for legacy flat sessions.
    int activeGroup = 0;
    std::vector<SessionToolWindow> toolWindows;
    std::vector<std::string> recentFiles;
    std::vector<Place> recentLocations;
    // CMake configure profile name to executable target name, scoped to this root.
    std::vector<std::pair<std::string, std::string>> cmakeTargets;
    friend bool operator==(const SessionState&, const SessionState&) = default;
};

inline constexpr int kSessionVersion = 1;

// The JSON form described in the header comment. O(size).
[[nodiscard]] std::string encodeSession(const SessionState& state);
// Errors: Corrupt (not JSON, wrong types), Unsupported (a newer version). O(size).
[[nodiscard]] Result<SessionState> decodeSession(std::string_view json);

// The FNV-1a 64-bit hash of `root`, as 16 lower-case hex digits (the session file's name). Pure.
[[nodiscard]] std::string sessionKey(std::string_view root);

class SessionStore {
public:
    // `directory` is absolute (created on the first save); `fs` outlives the store.
    SessionStore(io::IFileSystem& fs, std::string directory);

    // The saved session of `root`. A stored different root, or unbound CMake preferences, is Corrupt.
    // Errors: NotFound (none saved), decodeSession's, the file system's.
    [[nodiscard]] Result<SessionState> load(std::string_view root) const;
    // Saves `state` as the session of state.root and makes it the last project. Errors: the file
    // system's (createDirectories, writeFileAtomic).
    [[nodiscard]] Result<void> save(const SessionState& state);
    // The last project saved or opened, if any (a missing or broken index, or one cleared by
    // clearLastProject(), gives nullopt): the project the next start reopens.
    [[nodiscard]] std::optional<std::string> lastProject() const;
    // Recently opened project roots, most recent first.
    [[nodiscard]] std::vector<std::string> recentProjects() const;
    // Records `root` as the last (and most recent) project. Errors: as save().
    [[nodiscard]] Result<void> setLastProject(std::string_view root);
    // Forgets the last project (the recent list stays), so the next start shows the Welcome screen:
    // the user closed the project (ADR 0044). Errors: as save().
    [[nodiscard]] Result<void> clearLastProject();

    [[nodiscard]] const std::string& directory() const noexcept { return m_directory; }
    // The session file of `root`.
    [[nodiscard]] std::string fileOf(std::string_view root) const;

private:
    [[nodiscard]] Result<void> writeIndex(const std::vector<std::string>& recent, std::string_view last);

    io::IFileSystem* m_fs;
    std::string m_directory;
};

} // namespace frost::ui::nav
