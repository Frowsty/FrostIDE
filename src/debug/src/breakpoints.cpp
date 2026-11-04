// SPDX-License-Identifier: Apache-2.0
//
// BreakpointManager and DocumentBreakpointTracker (see frost/debug/breakpoints.hpp): the breakpoint
// table, its workspace.json persistence and the anchors that move breakpoints with edits.

#include "frost/debug/breakpoints.hpp"

#include "frost/core/log.hpp"
#include "frost/rpc/json.hpp"
#include "frost/settings/document.hpp"
#include "frost/settings/migration.hpp"
#include "frost/settings/project_layout.hpp"
#include "frost/text/document.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace frost::debug {
namespace {

// The workspace.json member (header comment, "Persistence").
constexpr std::string_view kMember = "breakpoints";

char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// JSON string escaping for the paths written to workspace.json.
std::string jsonString(std::string_view text) { return rpc::Json(text).dump(); }

// `path` relative to `root` with '/' separators, or nullopt when it is not under it.
std::optional<std::string> relativeTo(std::string_view root, std::string_view path, io::PathStyle style) {
    const bool windows = style == io::PathStyle::Windows;
    const auto same = [&](char a, char b) { return windows ? lowerAscii(a) == lowerAscii(b) : a == b; };
    const auto separator = [&](char c) { return c == '/' || (windows && c == '\\'); };
    std::size_t length = root.size();
    while (length > 0 && separator(root[length - 1])) {
        --length;
    }
    if (path.size() <= length + 1 || !separator(path[length]) ||
        !std::equal(root.begin(), root.begin() + static_cast<std::ptrdiff_t>(length), path.begin(), same)) {
        return std::nullopt;
    }
    std::string relative(path.substr(length + 1));
    if (windows) {
        std::ranges::replace(relative, '\\', '/');
    }
    return relative;
}

} // namespace

std::string BreakpointManager::key(std::string_view path) const {
    std::string out(path);
    if (m_style == io::PathStyle::Windows) {
        std::ranges::transform(out, out.begin(), lowerAscii);
        std::ranges::replace(out, '/', '\\');
    }
    return out;
}

bool BreakpointManager::samePath(std::string_view a, std::string_view b) const { return key(a) == key(b); }

void BreakpointManager::notify(std::string_view path, Change change) {
    // Copied: a listener may unsubscribe while it runs.
    const auto listeners = m_listeners;
    for (const auto& [token, listener] : listeners) {
        listener(path, change);
    }
}

Result<std::optional<BreakpointId>> BreakpointManager::toggle(std::string_view path, std::int64_t line) {
    if (const auto existing = at(path, line)) {
        remove(existing->id);
        return std::optional<BreakpointId>();
    }
    auto id = add(path, line);
    if (!id) {
        return std::unexpected(std::move(id.error()));
    }
    return std::optional<BreakpointId>(*id);
}

Result<BreakpointId> BreakpointManager::add(std::string_view path, std::int64_t line) {
    if (line < 1) {
        return fail(ErrorCode::InvalidArgument, std::format("breakpoint line {} is not 1-based", line));
    }
    auto normal = io::normalizePath(path, m_style);
    if (!normal) {
        return std::unexpected(std::move(normal.error()));
    }
    if (const auto existing = at(*normal, line)) {
        return existing->id;
    }
    const std::string fileKey = key(*normal);
    const auto display = m_displayPath.try_emplace(fileKey, *normal).first->second;
    const BreakpointId id = m_nextId++;
    m_byId.emplace(
        id,
        LineBreakpoint{.id = id, .path = display, .line = line, .status = BreakpointStatus::Unknown, .message = {}});
    notify(display, Change::Lines);
    return id;
}

bool BreakpointManager::remove(BreakpointId id) {
    const auto it = m_byId.find(id);
    if (it == m_byId.end()) {
        return false;
    }
    const std::string path = it->second.path;
    m_byId.erase(it);
    notify(path, Change::Lines);
    return true;
}

void BreakpointManager::clear() {
    const std::vector<std::string> paths = files();
    m_byId.clear();
    for (const std::string& path : paths) {
        notify(path, Change::Lines);
    }
}

bool BreakpointManager::setEnabled(BreakpointId id, bool enabled) {
    const auto it = m_byId.find(id);
    if (it == m_byId.end()) {
        return false;
    }
    if (it->second.enabled == enabled) {
        return true;
    }
    it->second.enabled = enabled;
    if (!enabled) {
        it->second.status = BreakpointStatus::Unknown;
        it->second.message.clear();
    }
    notify(it->second.path, Change::Lines);
    return true;
}

bool BreakpointManager::setProperties(BreakpointId id, std::string condition, std::string hitCondition,
                                      std::string logMessage) {
    const auto it = m_byId.find(id);
    if (it == m_byId.end()) {
        return false;
    }
    if (it->second.condition == condition && it->second.hitCondition == hitCondition &&
        it->second.logMessage == logMessage) {
        return true;
    }
    it->second.condition = std::move(condition);
    it->second.hitCondition = std::move(hitCondition);
    it->second.logMessage = std::move(logMessage);
    notify(it->second.path, Change::Lines);
    return true;
}

void BreakpointManager::setMuted(bool muted) {
    if (m_muted == muted) {
        return;
    }
    m_muted = muted;
    for (const std::string& path : files()) {
        notify(path, Change::Lines);
    }
}

std::optional<LineBreakpoint> BreakpointManager::find(BreakpointId id) const {
    const auto it = m_byId.find(id);
    return it == m_byId.end() ? std::nullopt : std::optional<LineBreakpoint>(it->second);
}

std::optional<LineBreakpoint> BreakpointManager::at(std::string_view path, std::int64_t line) const {
    const std::string fileKey = key(path);
    for (const auto& [id, breakpoint] : m_byId) {
        if (breakpoint.line == line && key(breakpoint.path) == fileKey) {
            return breakpoint;
        }
    }
    return std::nullopt;
}

std::vector<LineBreakpoint> BreakpointManager::inFile(std::string_view path) const {
    const std::string fileKey = key(path);
    std::vector<LineBreakpoint> out;
    for (const auto& [id, breakpoint] : m_byId) {
        if (key(breakpoint.path) == fileKey) {
            out.push_back(breakpoint);
        }
    }
    std::ranges::sort(out, {}, &LineBreakpoint::line);
    return out;
}

std::vector<LineBreakpoint> BreakpointManager::all() const {
    std::vector<LineBreakpoint> out;
    for (const std::string& path : files()) {
        for (LineBreakpoint& bp : inFile(path)) {
            out.push_back(std::move(bp));
        }
    }
    return out;
}

std::vector<std::string> BreakpointManager::files() const {
    std::set<std::string> keys;
    std::vector<std::string> out;
    for (const auto& [id, breakpoint] : m_byId) {
        if (keys.insert(key(breakpoint.path)).second) {
            out.push_back(breakpoint.path);
        }
    }
    std::ranges::sort(out);
    return out;
}

std::map<std::string, std::vector<BreakpointLine>> BreakpointManager::sessionLines() const {
    std::map<std::string, std::vector<BreakpointLine>> out;
    for (const std::string& path : files()) {
        out.emplace(path, sessionLines(path));
    }
    return out;
}

std::vector<BreakpointLine> BreakpointManager::sessionLines(std::string_view path) const {
    std::vector<BreakpointLine> out;
    if (m_muted) {
        return out;
    }
    for (const LineBreakpoint& breakpoint : inFile(path)) {
        if (!breakpoint.enabled) {
            continue;
        }
        out.push_back(BreakpointLine{
            .id = breakpoint.id,
            .line = breakpoint.line,
            .enabled = breakpoint.enabled,
            .condition = breakpoint.condition,
            .hitCondition = breakpoint.hitCondition,
            .logMessage = breakpoint.logMessage,
        });
    }
    return out;
}

void BreakpointManager::move(std::string_view path, const std::vector<std::pair<BreakpointId, std::int64_t>>& lines) {
    bool changed = false;
    for (const auto& [id, line] : lines) {
        const auto it = m_byId.find(id);
        if (it != m_byId.end() && line >= 1 && it->second.line != line) {
            it->second.line = line;
            changed = true;
        }
    }
    // Merges breakpoints that share a line: the lowest id stays.
    const std::string fileKey = key(path);
    std::map<std::int64_t, BreakpointId> seen;
    std::vector<BreakpointId> duplicates;
    for (const auto& [id, breakpoint] : m_byId) {
        if (key(breakpoint.path) != fileKey) {
            continue;
        }
        if (!seen.try_emplace(breakpoint.line, id).second) {
            duplicates.push_back(id);
        }
    }
    for (const BreakpointId id : duplicates) {
        m_byId.erase(id);
        changed = true;
    }
    if (changed) {
        notify(path, Change::Lines);
    }
}

void BreakpointManager::setStatus(BreakpointId id, bool verified, std::string message) {
    const auto it = m_byId.find(id);
    if (it == m_byId.end()) {
        return;
    }
    const BreakpointStatus status = verified ? BreakpointStatus::Verified : BreakpointStatus::Unverified;
    if (verified) {
        message.clear();
    }
    if (it->second.status == status && it->second.message == message) {
        return;
    }
    it->second.status = status;
    it->second.message = std::move(message);
    notify(it->second.path, Change::Status);
}

void BreakpointManager::resetStatus(BreakpointStatus status) {
    std::set<std::string> changed;
    for (auto& [id, breakpoint] : m_byId) {
        if (breakpoint.status != status || !breakpoint.message.empty()) {
            breakpoint.status = status;
            breakpoint.message.clear();
            changed.insert(breakpoint.path);
        }
    }
    for (const std::string& path : changed) {
        notify(path, Change::Status);
    }
}

std::uint64_t BreakpointManager::subscribe(Listener listener) {
    const std::uint64_t token = m_nextToken++;
    m_listeners.emplace(token, std::move(listener));
    return token;
}

void BreakpointManager::unsubscribe(std::uint64_t token) { m_listeners.erase(token); }

Result<void> BreakpointManager::load(io::IFileSystem& fs, std::string_view root) {
    auto layout = settings::ProjectLayout::forRoot(root, m_style);
    if (!layout) {
        return std::unexpected(std::move(layout.error()));
    }
    struct StoredEntry {
        std::string path;
        std::int64_t line = 0;
        bool enabled = true;
        std::string condition;
        std::string hitCondition;
        std::string logMessage;
    };
    std::vector<StoredEntry> entries;
    auto exists = fs.exists(layout->personalSettings);
    if (!exists) {
        return std::unexpected(std::move(exists.error()));
    }
    if (*exists) {
        auto bytes = fs.readFile(layout->personalSettings);
        if (!bytes) {
            return std::unexpected(std::move(bytes.error()));
        }
        auto document = settings::SettingsDocument::parse(std::move(*bytes));
        if (!document) {
            return std::unexpected(std::move(document.error()).withContext(layout->personalSettings));
        }
        if (const settings::SettingsDocument::Member* member = document->find(kMember); member != nullptr) {
            auto list = rpc::parseJson(document->rawValue(*member));
            if (!list || !list->isArray()) {
                log::warn("debug", "{}: \"breakpoints\" is not an array; ignored", layout->personalSettings);
            } else {
                for (const rpc::Json& entry : list->items()) {
                    const std::string_view file = entry.stringAt("file");
                    const std::optional<std::int64_t> line = entry.integerAt("line");
                    if (file.empty() || !line || *line < 1) {
                        continue;
                    }
                    std::string path(file);
                    if (!io::isAbsolutePath(path, m_style)) {
                        auto joined = io::joinPath(layout->root, path, m_style);
                        if (!joined) {
                            continue;
                        }
                        path = std::move(*joined);
                    }
                    StoredEntry stored;
                    stored.path = std::move(path);
                    stored.line = *line;
                    stored.enabled = entry.boolAt("enabled").value_or(true);
                    stored.condition = std::string(entry.stringAt("condition"));
                    stored.hitCondition = std::string(entry.stringAt("hitCondition"));
                    stored.logMessage = std::string(entry.stringAt("logMessage"));
                    entries.push_back(std::move(stored));
                }
            }
        }
    }
    clear();
    for (const StoredEntry& item : entries) {
        auto added = add(item.path, item.line);
        if (!added) {
            log::warn("debug", "a stored breakpoint at {}:{} was skipped: {}", item.path, item.line,
                      added.error().message());
            continue;
        }
        if (!item.enabled) {
            setEnabled(*added, false);
        }
        if (!item.condition.empty() || !item.hitCondition.empty() || !item.logMessage.empty()) {
            setProperties(*added, item.condition, item.hitCondition, item.logMessage);
        }
    }
    return {};
}

Result<void> BreakpointManager::save(io::IFileSystem& fs, std::string_view root) const {
    auto layout = settings::ProjectLayout::forRoot(root, m_style);
    if (!layout) {
        return std::unexpected(std::move(layout.error()));
    }
    settings::SettingsDocument document = settings::SettingsDocument::create(settings::kCurrentSchemaVersion);
    auto exists = fs.exists(layout->personalSettings);
    if (!exists) {
        return std::unexpected(std::move(exists.error()));
    }
    if (*exists) {
        auto bytes = fs.readFile(layout->personalSettings);
        if (!bytes) {
            return std::unexpected(std::move(bytes.error()));
        }
        auto parsed = settings::SettingsDocument::parse(std::move(*bytes));
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()).withContext(layout->personalSettings));
        }
        document = std::move(*parsed);
    } else if (m_byId.empty()) {
        return {}; // Nothing to remember and no file to touch.
    }

    const std::vector<LineBreakpoint> allBreakpoints = all();
    if (allBreakpoints.empty()) {
        auto removed = document.remove(kMember);
        if (!removed) {
            return std::unexpected(std::move(removed.error()));
        }
        if (!*removed) {
            return {};
        }
    } else {
        std::string json = "[";
        for (std::size_t i = 0; i < allBreakpoints.size(); ++i) {
            const LineBreakpoint& bp = allBreakpoints[i];
            const std::string file = relativeTo(layout->root, bp.path, m_style).value_or(bp.path);
            std::string extras;
            if (!bp.enabled) {
                extras += ", \"enabled\": false";
            }
            if (!bp.condition.empty()) {
                extras += std::format(", \"condition\": {}", jsonString(bp.condition));
            }
            if (!bp.hitCondition.empty()) {
                extras += std::format(", \"hitCondition\": {}", jsonString(bp.hitCondition));
            }
            if (!bp.logMessage.empty()) {
                extras += std::format(", \"logMessage\": {}", jsonString(bp.logMessage));
            }
            json += std::format("{}\n    {{\"file\": {}, \"line\": {}{}}}", i == 0 ? "" : ",", jsonString(file),
                                bp.line, extras);
        }
        json += "\n  ]";
        if (auto set = document.setRaw(kMember, json); !set) {
            return std::unexpected(std::move(set.error()));
        }
    }
    if (auto ensured = settings::ensureProjectLayout(fs, *layout); !ensured) {
        return std::unexpected(std::move(ensured.error()));
    }
    return fs.writeFileAtomic(layout->personalSettings, document.bytes());
}

// --- DocumentBreakpointTracker ---

DocumentBreakpointTracker::DocumentBreakpointTracker(BreakpointManager& manager, text::Document& document,
                                                     std::string path)
    : m_manager(manager), m_document(document), m_path(std::move(path)) {
    m_token = m_manager.subscribe([this](std::string_view changed, BreakpointManager::Change change) {
        if (change == BreakpointManager::Change::Lines && !m_syncing && m_manager.samePath(changed, m_path)) {
            reanchor();
        }
    });
    reanchor();
}

DocumentBreakpointTracker::~DocumentBreakpointTracker() {
    m_manager.unsubscribe(m_token);
    release();
}

void DocumentBreakpointTracker::release() {
    for (const auto& [id, anchor] : m_anchors) {
        static_cast<void>(m_document.releaseAnchor(anchor));
    }
    m_anchors.clear();
}

void DocumentBreakpointTracker::reanchor() {
    sync(); // Lines the document moved first, so a toggle on an edited document lands right.
    release();
    const text::Snapshot& text = m_document.text();
    for (const LineBreakpoint& breakpoint : m_manager.inFile(m_path)) {
        const auto line = static_cast<std::uint32_t>(breakpoint.line - 1);
        if (line >= text.lineCount()) {
            continue; // Past the end: kept as it is, not tracked.
        }
        auto info = text.lineInfo(LineIndex(line));
        if (!info) {
            continue;
        }
        if (auto anchor = m_document.createAnchor(info->content.begin, text::Bias::Right)) {
            m_anchors.emplace_back(breakpoint.id, *anchor);
        }
    }
    m_version = m_document.version();
}

void DocumentBreakpointTracker::sync() {
    if (m_document.version() == m_version || m_syncing) {
        return;
    }
    m_version = m_document.version();
    const text::Snapshot& text = m_document.text();
    std::vector<std::pair<BreakpointId, std::int64_t>> lines;
    for (const auto& [id, anchor] : m_anchors) {
        if (const std::optional<ByteOffset> offset = m_document.anchors().find(anchor)) {
            lines.emplace_back(id, static_cast<std::int64_t>(text.lineAt(*offset).value()) + 1);
        }
    }
    m_syncing = true;
    m_manager.move(m_path, lines);
    m_syncing = false;
    // Merged breakpoints lost their entries; drop their anchors.
    std::erase_if(m_anchors, [&](const auto& entry) {
        if (m_manager.find(entry.first)) {
            return false;
        }
        static_cast<void>(m_document.releaseAnchor(entry.second));
        return true;
    });
}

} // namespace frost::debug
