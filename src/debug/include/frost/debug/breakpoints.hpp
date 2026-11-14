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
 * Function breakpoints and exception filters (DBG-2a). Function breakpoints (by function name, with
 * a condition and a hit condition) share the id space, enabling, muting and statuses of line
 * breakpoints. Exception filters are the adapter's: setAvailableExceptionFilters records the list a
 * session's capabilities report (lldb-dap: cpp_throw, cpp_catch, ...) with each filter's
 * availability, keeping what the user enabled; until a session has reported them the list holds only
 * what was persisted (the labels came from an earlier session's capabilities), never a filter
 * FrostIDE made up. The C++ throw filter (cpp_throw) can carry a list of exception types (DBG-2b):
 * an MSVC session then stops only for throws of those types (DebugSession, "Thrown types").
 *
 * Persistence. The personal scope file's "breakpoints", "functionBreakpoints" and
 * "exceptionBreakpoints" members:
 *     "breakpoints": [
 *       {"file": "src/main.cpp", "line": 12, "condition": "i == 4", "hitCondition": "3",
 *        "logMessage": "i={i}", "enabled": false}
 *     ],
 *     "functionBreakpoints": [{"name": "frost_function_target", "condition": "value > 1"}],
 *     "exceptionBreakpoints": [{"filter": "cpp_throw", "label": "C++ Throw", "enabled": true,
                              "types": ["std::runtime_error"]}]
 * (optional members only when set). Paths under the root are stored relative to it with '/', others
 * absolute. Every other byte of the file is kept (SettingsDocument); an empty list removes its
 * member.
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
    bool enabled = true;
    std::string condition{};
    std::string hitCondition{};
    std::string logMessage{};

    friend bool operator==(const LineBreakpoint&, const LineBreakpoint&) = default;
};

struct FunctionBreakpointEntry {
    BreakpointId id = 0;
    std::string name;
    BreakpointStatus status = BreakpointStatus::Unknown;
    std::string message;
    bool enabled = true;
    std::string condition{};
    std::string hitCondition{};

    friend bool operator==(const FunctionBreakpointEntry&, const FunctionBreakpointEntry&) = default;
};

// One exception filter as the Breakpoints window lists it (header comment).
struct ExceptionFilterEntry {
    std::string filter; // "cpp_throw".
    std::string label;  // "C++ Throw", the adapter's.
    bool enabled = false;
    std::string unavailable{};        // Why it cannot stop in this session's programs; "" when it can.
    std::vector<std::string> types{}; // cpp_throw only: the thrown types it stops for; empty: all.

    friend bool operator==(const ExceptionFilterEntry&, const ExceptionFilterEntry&) = default;
};

class BreakpointManager {
public:
    // What changed: a file's lines (added, removed, moved), only statuses (a function breakpoint's
    // with an empty path), the function breakpoints or the exception filters (these two with an
    // empty path).
    enum class Change : std::uint8_t { Lines, Status, Functions, Exceptions };
    using Listener = std::function<void(std::string_view path, Change change)>;

    explicit BreakpointManager(io::PathStyle style = io::kNativePathStyle) noexcept : m_style(style) {}

    // Adds a breakpoint at `line` of `path`, or removes the one there. Returns the new id, or
    // nullopt when one was removed. Errors: InvalidArgument (relative path, line < 1).
    [[nodiscard]] Result<std::optional<BreakpointId>> toggle(std::string_view path, std::int64_t line);
    // Adds a breakpoint (the existing one's id when the line has one). Errors: as toggle().
    [[nodiscard]] Result<BreakpointId> add(std::string_view path, std::int64_t line);
    // Removes a line or function breakpoint; false when there is none with `id`.
    bool remove(BreakpointId id);
    // Removes every line and function breakpoint (exception filters stay).
    void clear();

    // A line or function breakpoint.
    bool setEnabled(BreakpointId id, bool enabled);
    // A line breakpoint's properties, or a function breakpoint's (its logMessage must be empty: DAP
    // function breakpoints have none).
    bool setProperties(BreakpointId id, std::string condition, std::string hitCondition, std::string logMessage);

    // Adds a function breakpoint (the existing one's id for a name that has one). Errors:
    // InvalidArgument (an empty name or one with a line break).
    [[nodiscard]] Result<BreakpointId> addFunction(std::string name);
    [[nodiscard]] std::optional<FunctionBreakpointEntry> findFunction(BreakpointId id) const;
    // By id.
    [[nodiscard]] std::vector<FunctionBreakpointEntry> functionBreakpoints() const;
    // The enabled function breakpoints for a session; none while muted.
    [[nodiscard]] std::vector<FunctionBreakpointLine> sessionFunctions() const;

    /*
     * Records the filters a session's capabilities report (header comment). `unavailable(filter)`
     * says why one cannot work in this session ("" when it can). Filters the adapter no longer
     * reports are dropped; enabled flags of the others are kept.
     */
    void setAvailableExceptionFilters(const std::vector<dap::ExceptionFilter>& filters,
                                      const std::function<std::string(std::string_view)>& unavailable = {});
    // Enables or disables a known filter; false when `filter` is unknown or unavailable (enabling).
    bool setExceptionFilterEnabled(std::string_view filter, bool enabled);
    [[nodiscard]] std::vector<ExceptionFilterEntry> exceptionFilters() const { return m_exceptionFilters; }
    // The enabled, available filter ids for a session; none while muted.
    [[nodiscard]] std::vector<std::string> sessionExceptionFilters() const;
    // Sets the thrown types cpp_throw stops for (header comment; msvc::splitTypeList's list); false
    // when `filter` is unknown or is not cpp_throw.
    bool setExceptionFilterTypes(std::string_view filter, std::vector<std::string> types);
    // The types of the enabled, available cpp_throw filter for a session (DebugSession::setThrowTypes);
    // empty when it has none, is off, or while muted.
    [[nodiscard]] std::vector<std::string> sessionThrowTypes() const;
    [[nodiscard]] bool muted() const noexcept { return m_muted; }
    void setMuted(bool muted);

    [[nodiscard]] std::optional<LineBreakpoint> find(BreakpointId id) const;
    [[nodiscard]] std::optional<LineBreakpoint> at(std::string_view path, std::int64_t line) const;
    // The breakpoints of `path`, by line.
    [[nodiscard]] std::vector<LineBreakpoint> inFile(std::string_view path) const;
    // All breakpoints across all files, ordered by file and line.
    [[nodiscard]] std::vector<LineBreakpoint> all() const;
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

    // Copies shared logical line/function definitions and mute state, retaining this manager's
    // verification for unchanged definitions. Exception capabilities/settings stay session-local.
    // Stable ids and the next-id watermark come from source. GUI thread; O(breakpoints log n).
    // Only changed logical files/functions are notified; adapter status is never copied.
    void replaceLogicalFrom(const BreakpointManager& source);

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
    std::map<BreakpointId, FunctionBreakpointEntry> m_functions;
    std::vector<ExceptionFilterEntry> m_exceptionFilters;
    std::map<std::string, std::string, std::less<>> m_displayPath; // key -> path as first given.
    std::uint64_t m_nextToken = 1;
    std::map<std::uint64_t, Listener> m_listeners;
    bool m_muted = false;
};

using BreakpointStore = BreakpointManager;

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
