// SPDX-License-Identifier: Apache-2.0
//
// Live diagnostics (diagnostics.hpp): decoding publishDiagnostics, mapping ranges to snapshots and
// the DiagnosticsStore.

#include "frost/lang/diagnostics.hpp"

#include "frost/lang/uri.hpp"

#include <algorithm>
#include <tuple>

namespace frost::lang {
namespace {

[[nodiscard]] std::string codeOf(const Json* code) {
    if (code == nullptr) {
        return {};
    }
    if (const std::string* text = code->asString()) {
        return *text;
    }
    if (const std::optional<std::int64_t> number = code->asInteger()) {
        return std::to_string(*number);
    }
    return {};
}

[[nodiscard]] DiagnosticSeverity severityOf(const Json& diagnostic) noexcept {
    const std::optional<std::int64_t> value = diagnostic.integerAt("severity");
    if (!value || *value < 1 || *value > 4) {
        return DiagnosticSeverity::Error; // LSP: "If omitted it is up to the client"; clangd always sends it.
    }
    return static_cast<DiagnosticSeverity>(*value);
}

} // namespace

std::string_view toString(DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::Error: return "error";
    case DiagnosticSeverity::Warning: return "warning";
    case DiagnosticSeverity::Information: return "information";
    case DiagnosticSeverity::Hint: return "hint";
    }
    return "error";
}

Result<FileDiagnostics> decodePublishDiagnostics(const Json& params) {
    const std::string* uri = params.find("uri") != nullptr ? params.find("uri")->asString() : nullptr;
    const Json* list = params.find("diagnostics");
    if (uri == nullptr || list == nullptr || !list->isArray()) {
        return fail(ErrorCode::InvalidArgument, "publishDiagnostics without uri or diagnostics");
    }
    auto path = uriToPath(*uri);
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    FileDiagnostics out;
    out.uri = *uri;
    out.file = std::move(*path);
    out.lspVersion = params.integerAt("version");
    for (const Json& item : list->items()) {
        const Json* rangeJson = item.find("range");
        const std::optional<LspRange> range = rangeJson != nullptr ? parseRange(*rangeJson) : std::nullopt;
        if (!range) {
            continue;
        }
        LiveDiagnostic diagnostic;
        diagnostic.severity = severityOf(item);
        diagnostic.code = codeOf(item.find("code"));
        diagnostic.message = std::string(item.stringAt("message"));
        diagnostic.source = std::string(item.stringAt("source"));
        diagnostic.file = out.file;
        diagnostic.lspRange = *range;
        diagnostic.line = static_cast<int>(range->start.line) + 1;
        diagnostic.column = static_cast<int>(range->start.character) + 1;
        if (const Json* related = item.find("relatedInformation"); related != nullptr && related->isArray()) {
            for (const Json& entry : related->items()) {
                const Json* location = entry.find("location");
                if (location == nullptr) {
                    continue;
                }
                const Json* relatedRange = location->find("range");
                const std::optional<LspRange> where =
                    relatedRange != nullptr ? parseRange(*relatedRange) : std::nullopt;
                if (!where) {
                    continue;
                }
                auto relatedPath = uriToPath(location->stringAt("uri"));
                diagnostic.related.push_back(RelatedInformation{
                    .file = relatedPath ? std::move(*relatedPath) : std::string(),
                    .range = *where,
                    .line = static_cast<int>(where->start.line) + 1,
                    .column = static_cast<int>(where->start.character) + 1,
                    .message = std::string(entry.stringAt("message")),
                });
            }
        }
        out.diagnostics.push_back(std::move(diagnostic));
    }
    return out;
}

void mapToSnapshot(FileDiagnostics& diagnostics, const text::Snapshot& snapshot, PositionEncoding encoding) {
    diagnostics.textVersion = snapshot.version();
    for (LiveDiagnostic& diagnostic : diagnostics.diagnostics) {
        diagnostic.textVersion = snapshot.version();
        diagnostic.range.reset();
        auto begin = fromLspPosition(snapshot, diagnostic.lspRange.start, encoding);
        auto end = fromLspPosition(snapshot, diagnostic.lspRange.end, encoding);
        if (begin && end && *begin <= *end) {
            diagnostic.range = text::ByteRange{.begin = *begin, .end = *end};
        }
    }
}

void DiagnosticsStore::publish(FileDiagnostics diagnostics) {
    const std::string key = documentKey(diagnostics.file);
    {
        LockGuard lock(m_mutex);
        ++m_generation;
        if (diagnostics.diagnostics.empty()) {
            m_files.erase(key);
        } else {
            m_files[key] = diagnostics;
        }
    }
    notify(diagnostics);
}

void DiagnosticsStore::clear() {
    std::vector<FileDiagnostics> cleared;
    {
        LockGuard lock(m_mutex);
        ++m_generation;
        for (auto& [key, set] : m_files) {
            FileDiagnostics empty;
            empty.file = set.file;
            empty.uri = set.uri;
            cleared.push_back(std::move(empty));
        }
        m_files.clear();
    }
    for (const FileDiagnostics& set : cleared) {
        notify(set);
    }
}

std::optional<FileDiagnostics> DiagnosticsStore::forFile(std::string_view path) const {
    const std::string key = documentKey(path);
    LockGuard lock(m_mutex);
    auto it = m_files.find(key);
    if (it == m_files.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<FileDiagnostics> DiagnosticsStore::files() const {
    LockGuard lock(m_mutex);
    std::vector<FileDiagnostics> out;
    out.reserve(m_files.size());
    for (const auto& [key, set] : m_files) {
        out.push_back(set);
    }
    return out;
}

std::vector<LiveDiagnostic> DiagnosticsStore::problems() const {
    std::vector<LiveDiagnostic> out;
    {
        LockGuard lock(m_mutex);
        for (const auto& [key, set] : m_files) {
            out.insert(out.end(), set.diagnostics.begin(), set.diagnostics.end());
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const LiveDiagnostic& a, const LiveDiagnostic& b) {
        return std::tie(a.file, a.line, a.column) < std::tie(b.file, b.line, b.column);
    });
    return out;
}

std::size_t DiagnosticsStore::count(DiagnosticSeverity severity) const {
    LockGuard lock(m_mutex);
    std::size_t total = 0;
    for (const auto& [key, set] : m_files) {
        total +=
            static_cast<std::size_t>(std::count_if(set.diagnostics.begin(), set.diagnostics.end(),
                                                   [&](const LiveDiagnostic& d) { return d.severity == severity; }));
    }
    return total;
}

std::uint64_t DiagnosticsStore::generation() const {
    LockGuard lock(m_mutex);
    return m_generation;
}

std::uint64_t DiagnosticsStore::addListener(Listener listener) {
    LockGuard lock(m_mutex);
    const std::uint64_t id = m_nextListener++;
    m_listeners.emplace(id, std::move(listener));
    return id;
}

void DiagnosticsStore::removeListener(std::uint64_t id) {
    LockGuard lock(m_mutex);
    m_listeners.erase(id);
}

void DiagnosticsStore::notify(const FileDiagnostics& diagnostics) const {
    std::vector<Listener> listeners;
    {
        LockGuard lock(m_mutex);
        for (const auto& [id, listener] : m_listeners) {
            listeners.push_back(listener);
        }
    }
    for (const Listener& listener : listeners) {
        listener(diagnostics);
    }
}

} // namespace frost::lang
