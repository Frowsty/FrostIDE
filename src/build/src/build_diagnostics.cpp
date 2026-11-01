// SPDX-License-Identifier: Apache-2.0
//
// Diagnostic resolution, the diagnostic index and the project output line reader (see
// frost/build/build_diagnostics.hpp).

#include "frost/build/build_diagnostics.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include <array>
#include <utility>

namespace frost::build {

namespace {

constexpr char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

bool endsWithIgnoringCase(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && equalsIgnoringCase(text.substr(text.size() - suffix.size()), suffix);
}

// The extension of the last component, with its dot ("" when there is none).
std::string_view extensionOf(std::string_view path) noexcept {
    const std::size_t separator = path.find_last_of("\\/");
    const std::string_view name = separator == std::string_view::npos ? path : path.substr(separator + 1);
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos || dot == 0 ? std::string_view() : name.substr(dot);
}

std::string_view trimLeft(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    return text;
}

constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

void count(DiagnosticCounts& counts, Severity severity) noexcept {
    switch (severity) {
    case Severity::Error: ++counts.errors; break;
    case Severity::Warning: ++counts.warnings; break;
    case Severity::Message: ++counts.messages; break;
    }
}

} // namespace

bool isToolOrigin(std::string_view origin) noexcept {
    if (origin.empty() || origin.find_first_of("\\/") != std::string_view::npos) {
        return false;
    }
    const std::string_view extension = extensionOf(origin);
    return extension.empty() || equalsIgnoringCase(extension, ".exe");
}

bool isBuildProduct(std::string_view path) noexcept {
    static constexpr std::array<std::string_view, 16> kProducts{".obj", ".o",    ".lib",  ".a",    ".exe", ".dll",
                                                                ".so",  ".pdb",  ".res",  ".exp",  ".ilk", ".idb",
                                                                ".pch", ".ipch", ".tlog", ".dylib"};
    const std::string_view extension = extensionOf(path);
    for (const std::string_view product : kProducts) {
        if (equalsIgnoringCase(extension, product)) {
            return true;
        }
    }
    return false;
}

std::string documentKey(std::string_view path) {
    auto normalized = io::normalizePath(path);
    if (!normalized) {
        return {};
    }
    if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
        for (char& c : *normalized) {
            c = lowerAscii(c);
        }
    }
    return std::move(*normalized);
}

DiagnosticResolver::DiagnosticResolver(io::IFileSystem& fs, std::string solutionDirectory)
    : m_fs(fs), m_solutionDirectory(std::move(solutionDirectory)) {}

BuildDiagnostic DiagnosticResolver::resolve(const Diagnostic& diagnostic) {
    BuildDiagnostic out{.diagnostic = diagnostic, .path = {}, .jumpable = false};
    const std::string_view origin = diagnostic.file;
    if (origin.empty() || isToolOrigin(origin)) {
        return out;
    }
    if (io::isAbsolutePath(origin)) {
        out.path = io::normalizePath(origin).value_or("");
    } else {
        // Relative: against the reporting project's directory, else the solution's.
        std::string base;
        if (auto project = io::normalizePath(diagnostic.project)) {
            base = std::string(io::parentPath(*project));
        }
        if (base.empty()) {
            base = m_solutionDirectory;
        }
        if (!base.empty()) {
            if (auto joined = io::joinPath(base, origin)) {
                out.path = io::normalizePath(*joined).value_or("");
            }
        }
    }
    if (out.path.empty() || diagnostic.line <= 0 || isBuildProduct(out.path)) {
        return out;
    }
    const std::string key = documentKey(out.path);
    auto known = m_isFile.find(key);
    if (known == m_isFile.end()) {
        auto status = m_fs.status(out.path);
        known = m_isFile.emplace(key, status && status->type == io::FileType::Regular).first;
    }
    out.jumpable = known->second;
    return out;
}

void DiagnosticIndex::add(BuildDiagnostic diagnostic) {
    count(m_counts, diagnostic.diagnostic.severity);
    if (!diagnostic.path.empty()) {
        m_byDocument[documentKey(diagnostic.path)].push_back(m_items.size());
    }
    m_items.push_back(std::move(diagnostic));
}

void DiagnosticIndex::clear() noexcept {
    m_items.clear();
    m_counts = {};
    m_byDocument.clear();
}

std::vector<DiagnosticFileGroup> DiagnosticIndex::byFile() const {
    std::vector<DiagnosticFileGroup> groups;
    // Group key -> index into groups. Paths and origins get distinct prefixes so "LINK" the tool
    // and a file named LINK never share a group.
    std::unordered_map<std::string, std::size_t> slots;
    for (const BuildDiagnostic& item : m_items) {
        const bool isPath = !item.path.empty();
        std::string key = isPath ? "p" + documentKey(item.path) : "o" + item.diagnostic.file;
        auto [slot, inserted] = slots.emplace(std::move(key), groups.size());
        if (inserted) {
            groups.push_back(
                {.file = isPath ? item.path : item.diagnostic.file, .isPath = isPath, .diagnostics = {}, .counts = {}});
        }
        DiagnosticFileGroup& group = groups[slot->second];
        group.diagnostics.push_back(item);
        count(group.counts, item.diagnostic.severity);
    }
    return groups;
}

std::vector<BuildDiagnostic> DiagnosticIndex::forDocument(std::string_view path) const {
    const std::string key = documentKey(path);
    if (key.empty()) {
        return {};
    }
    const auto it = m_byDocument.find(key);
    if (it == m_byDocument.end()) {
        return {};
    }
    std::vector<BuildDiagnostic> out;
    out.reserve(it->second.size());
    for (const std::size_t index : it->second) {
        out.push_back(m_items[index]);
    }
    return out;
}

std::optional<ProjectOutputLine> parseProjectOutputLine(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    line = trimLeft(line);
    // MSBuild's node prefix ("12>"), which -m adds from normal verbosity up.
    std::size_t digits = 0;
    while (digits < line.size() && isDigit(line[digits])) {
        ++digits;
    }
    if (digits > 0 && digits < line.size() && line[digits] == '>') {
        line = trimLeft(line.substr(digits + 1));
    }
    constexpr std::string_view kArrow = " -> ";
    const std::size_t arrow = line.find(kArrow);
    if (arrow == std::string_view::npos || arrow == 0) {
        return std::nullopt;
    }
    const std::string_view project = line.substr(0, arrow);
    const std::string_view output = line.substr(arrow + kArrow.size());
    const std::string_view extension = extensionOf(project);
    if (project.find_first_of("\\/:") != std::string_view::npos || extension.size() <= 4 ||
        !endsWithIgnoringCase(extension, "proj") || output.empty()) {
        return std::nullopt;
    }
    return ProjectOutputLine{.projectFile = std::string(project), .output = std::string(output)};
}

} // namespace frost::build
