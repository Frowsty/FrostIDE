// SPDX-License-Identifier: Apache-2.0
//
// Line breakpoints (PLAN.md M4, ADR 0050): BreakpointManager keeps every line breakpoint of the open
// root with the debug adapter's verdict on it, persists them in .frost/workspace.json, and
// DocumentBreakpointTracker moves the breakpoints of an open document with its edits.
/*
 * Identity. A breakpoint is a source path (absolute, normalised; compared case-insensitively on
 * Windows, as the file system does) and a 1-based line; one breakpoint per line. Ids are unique
 * for the manager's life and never reused, so the adapter's answers for a breakpoint that was
 * removed meanwhile are dropped.
 *
 * Status. Unknown (no session), Pending (sent, no answer yet), Verified (the adapter bound it) or
 * Unverified (it did not, with the adapter's message, such as "No code is generated for this
 * line"). The gutter draws Verified as a filled red dot and the others as a hollow grey one.
 *
 * Persistence. The personal scope file's "breakpoints" member, one breakpoint per line:
 *     "breakpoints": [
 *       {"file": "src/main.cpp", "line": 12}
 *     ]
 * Paths under the root are stored relative to it with '/', others absolute. Every other byte of
 * the file is kept (SettingsDocument); an empty list removes the member.
 *
 * Moving with edits. DocumentBreakpointTracker holds one text anchor (Bias::Right) at the start of
 * each breakpoint's line, so inserting lines above moves it down, typing at the line start keeps it,
 * and deleting its line leaves it on the line that takes the place; breakpoints that land on the
 * same line merge. sync() reads the anchors after an edit and moves the breakpoints.
 *
 * Threads: thread-compatible; the UI uses it from its thread only. Listeners run synchronously
 * inside the call that changed something and may call const members.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/debug/debug_session.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/text/anchor_set.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::text {
class Document;
}

namespace frost::debug {

using BreakpointId = std::uint64_t;

enum class BreakpointStatus : std::uint8_t { Unknown, Pending, Verified, Unverified };

struct LineBreakpoint {
    BreakpointId id = 0;
    std::string path;      // Absolute and normalised, as first given.
    std::int64_t line = 0; // 1-based.
    BreakpointStatus status = BreakpointStatus::Unknown;
    std::string message; // The adapter's reason when Unverified.

    friend bool operator==(const LineBreakpoint&, const LineBreakpoint&) = default;
};

class BreakpointManager {
public:
    // What changed in a file: its lines (added, removed, moved) or only statuses.
    enum class Change : std::uint8_t { Lines, Status };
    using Listener = std::function<void(std::string_view path, Change change)>;

    explicit BreakpointManager(io::PathStyle style = io::kNativePathStyle) noexcept : m_style(style) {}

    // Adds a breakpoint at `line` of `path`, or removes the one there. Returns the new id, or
    // nullopt when one was removed. Errors: InvalidArgument (relative path, line < 1).
    [[nodiscard]] Result<std::optional<BreakpointId>> toggle(std::string_view path, std::int64_t line);
    // Adds a breakpoint (the existing one's id when the line has one). Errors: as toggle().
    [[nodiscard]] Result<BreakpointId> add(std::string_view path, std::int64_t line);
    // Removes a breakpoint; false when there is none with `id`.
    bool remove(BreakpointId id);
    // Removes every breakpoint.
    void clear();

    [[nodiscard]] std::optional<LineBreakpoint> find(BreakpointId id) const;
    [[nodiscard]] std::optional<LineBreakpoint> at(std::string_view path, std::int64_t line) const;
    // The breakpoints of `path`, by line.
    [[nodiscard]] std::vector<LineBreakpoint> inFile(std::string_view path) const;
    // Every file with breakpoints (each as first given), sorted.
    [[nodiscard]] std::vector<std::string> files() const;
    [[nodiscard]] std::size_t size() const noexcept { return m_byId.size(); }
    // Whether two normalised paths name the same file under the manager's rules.
    [[nodiscard]] bool samePath(std::string_view a, std::string_view b) const;

    // The lines to configure a debug session with, per source (DebugSession::start).
    [[nodiscard]] std::map<std::string, std::vector<BreakpointLine>> sessionLines() const;
    // The lines of one source (DebugSession::setBreakpoints).
    [[nodiscard]] std::vector<BreakpointLine> sessionLines(std::string_view path) const;

    // Moves breakpoints of `path` to new 1-based lines; when two land on one line the one with the
    // lower id stays and the other is removed. Unknown ids are skipped.
    void move(std::string_view path, const std::vector<std::pair<BreakpointId, std::int64_t>>& lines);

    // Applies the adapter's answer for one breakpoint (unknown ids are ignored).
    void setStatus(BreakpointId id, bool verified, std::string message);
    // Every breakpoint to `status` with no message (Pending when a session starts, Unknown when it ends).
    void resetStatus(BreakpointStatus status = BreakpointStatus::Unknown);

    // Registers a listener; returns its token for unsubscribe().
    std::uint64_t subscribe(Listener listener);
    void unsubscribe(std::uint64_t token);

    /*
     * Replaces the breakpoints with those in `root`'s .frost/workspace.json. A missing file or
     * member is an empty list; entries that are malformed are skipped. Errors: Corrupt (the file
     * does not parse), InvalidArgument (root), the file system's errors.
     */
    [[nodiscard]] Result<void> load(io::IFileSystem& fs, std::string_view root);
    // Writes them to `root`'s .frost/workspace.json (creating .frost/ first). Errors: as load().
    [[nodiscard]] Result<void> save(io::IFileSystem& fs, std::string_view root) const;

private:
    [[nodiscard]] std::string key(std::string_view path) const;
    void notify(std::string_view path, Change change);

    io::PathStyle m_style;
    BreakpointId m_nextId = 1;
    std::map<BreakpointId, LineBreakpoint> m_byId;
    std::map<std::string, std::string, std::less<>> m_displayPath; // key -> path as first given.
    std::uint64_t m_nextToken = 1;
    std::map<std::uint64_t, Listener> m_listeners;
};

// Keeps the breakpoints of one open document on their lines through its edits (header comment).
// Borrows both; destroy it before either. Not thread-safe.
class DocumentBreakpointTracker {
public:
    DocumentBreakpointTracker(BreakpointManager& manager, text::Document& document, std::string path);
    ~DocumentBreakpointTracker();
    DocumentBreakpointTracker(const DocumentBreakpointTracker&) = delete ("owns the document's anchors");
    DocumentBreakpointTracker& operator=(const DocumentBreakpointTracker&) = delete ("owns the document's anchors");

    // After an edit: moves the breakpoints to where their anchors went. Cheap when nothing changed.
    void sync();
    // Recreates the anchors from the manager's lines (the manager's listener does it on its own).
    void reanchor();

    [[nodiscard]] const std::string& path() const noexcept { return m_path; }

private:
    void release();

    BreakpointManager& m_manager;
    text::Document& m_document;
    std::string m_path;
    std::uint64_t m_token = 0;
    std::uint64_t m_version = 0;
    bool m_syncing = false;
    std::vector<std::pair<BreakpointId, text::AnchorId>> m_anchors;
};

} // namespace frost::debug
