// SPDX-License-Identifier: Apache-2.0
//
// TrustStore (trust_store.hpp): trusted roots in a small text file, and the build gate.
#include "frost/project/trust_store.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/solution_model.hpp"

#include "paths.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#endif

namespace frost::project {
namespace {

constexpr std::string_view kHeaderPrefix = "# FrostIDE trusted roots v";
constexpr int kFormatVersion = 1;

// Validates and normalises a root or a queried path (trust_store.hpp, "Semantics").
Result<std::string> normalizeTrustPath(std::string_view path) {
    if (path.find_first_of("\r\n") != std::string_view::npos) {
        return fail(ErrorCode::InvalidArgument, "a trusted path cannot contain line breaks");
    }
    auto normalized = io::normalizePath(path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    if constexpr (io::kNativePathStyle == io::PathStyle::Posix) {
        const std::string& p = *normalized;
        if (p.ends_with("/..") || p.find("/../") != std::string::npos) {
            return fail(ErrorCode::InvalidArgument, std::format("{}: '..' is not allowed in a trusted path", p));
        }
    }
    return normalized;
}

#ifdef _WIN32
Result<std::string> environmentPath(const wchar_t* name) {
    std::wstring value(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size()) {
        return fail(ErrorCode::NotFound, "environment variable not set");
    }
    value.resize(length);
    auto utf8 = io::fromWide(value);
    if (!utf8) {
        return std::unexpected(std::move(utf8.error()));
    }
    return io::normalizePath(*utf8);
}
#else
Result<std::string> environmentPath(const char* name) {
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read once, never set.
    if (value == nullptr || *value == '\0') {
        return fail(ErrorCode::NotFound, std::format("{} is not set", name));
    }
    return io::normalizePath(value);
}
#endif

} // namespace

Result<std::unique_ptr<TrustStore>> TrustStore::open(io::IFileSystem& fs, std::string_view storePath) {
    auto path = io::normalizePath(storePath);
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    std::unique_ptr<TrustStore> store(new TrustStore(fs, std::move(*path)));
    {
        LockGuard lock(store->m_mutex);
        if (auto loaded = store->reload(); !loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
    }
    return store;
}

Result<std::string> TrustStore::defaultStorePath() {
#ifdef _WIN32
    auto base = environmentPath(L"LOCALAPPDATA");
    if (!base) {
        return fail(ErrorCode::NotFound, "LOCALAPPDATA is not set to an absolute path");
    }
    return io::joinPath(*base, "FrostIDE\\data\\trusted-roots.txt");
#else
    if (auto xdg = environmentPath("XDG_DATA_HOME")) {
        return io::joinPath(*xdg, "FrostIDE/trusted-roots.txt");
    }
    auto home = environmentPath("HOME");
    if (!home) {
        return fail(ErrorCode::NotFound, "neither XDG_DATA_HOME nor HOME is set to an absolute path");
    }
    return io::joinPath(*home, ".local/share/FrostIDE/trusted-roots.txt");
#endif
}

Result<void> TrustStore::reload() {
    m_warnings.clear();
    auto content = m_fs.readFile(m_storePath);
    if (!content) {
        if (content.error().code() == ErrorCode::NotFound) {
            if (!m_roots.empty())
                ++m_revision;
            m_roots.clear();
            return {};
        }
        return std::unexpected(std::move(content.error()));
    }
    auto text = detail::decodeText(*content, m_storePath);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    std::vector<std::string> roots;
    bool header = false;
    for (std::string_view line : detail::splitTrimmed(*text, '\n')) {
        if (!header) {
            if (!line.starts_with(kHeaderPrefix)) {
                return fail(ErrorCode::Corrupt, std::format("{}: not a FrostIDE trusted-roots file", m_storePath));
            }
            const std::string_view number = line.substr(kHeaderPrefix.size());
            int version = 0;
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), version);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || version < 1) {
                return fail(ErrorCode::Corrupt, std::format("{}: unreadable format version", m_storePath));
            }
            if (version > kFormatVersion) {
                return fail(ErrorCode::Unsupported,
                            std::format("{}: written by a newer FrostIDE (format v{})", m_storePath, version));
            }
            header = true;
            continue;
        }
        if (line.front() == '#') {
            continue;
        }
        auto root = normalizeTrustPath(line);
        if (!root) {
            m_warnings.push_back(std::format("skipped '{}': {}", line, root.error().message()));
            continue;
        }
        const std::string key = detail::pathKey(*root);
        bool duplicate = false;
        for (const std::string& existing : roots) {
            duplicate = duplicate || detail::pathKey(existing) == key;
        }
        if (!duplicate) {
            roots.push_back(std::move(*root));
        }
    }
    if (m_roots != roots)
        ++m_revision;
    m_roots = std::move(roots);
    return {};
}

Result<void> TrustStore::save(const std::vector<std::string>& roots) {
    const std::string_view directory = io::parentPath(m_storePath);
    if (!directory.empty()) {
        if (auto created = m_fs.createDirectories(directory); !created) {
            return std::unexpected(std::move(created.error()));
        }
    }
    std::string content = std::format("{}{}\n", kHeaderPrefix, kFormatVersion);
    for (const std::string& root : roots) {
        content += root;
        content += '\n';
    }
    return m_fs.writeFileAtomic(m_storePath, content);
}

bool TrustStore::isTrusted(std::string_view path) const { return snapshot(path).trusted; }

TrustSnapshot TrustStore::snapshot(std::string_view path) const {
    auto normalized = normalizeTrustPath(path);
    LockGuard lock(m_mutex);
    if (!normalized)
        return {.trusted = false, .revision = m_revision};
    for (const auto* list : {&m_roots, &m_sessionRoots}) {
        for (const std::string& root : *list) {
            if (detail::isSameOrUnder(*normalized, root)) {
                return {.trusted = true, .revision = m_revision};
            }
        }
    }
    return {.trusted = false, .revision = m_revision};
}

Result<void> TrustStore::trustForSession(std::string_view root) {
    auto normalized = normalizeTrustPath(root);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    LockGuard lock(m_mutex);
    const std::string key = detail::pathKey(*normalized);
    if (std::ranges::any_of(m_sessionRoots,
                            [&](const std::string& existing) { return detail::pathKey(existing) == key; }))
        return {};
    m_sessionRoots.push_back(std::move(*normalized));
    ++m_revision;
    return {};
}

void TrustStore::endSessionTrust(std::string_view root) {
    auto normalized = normalizeTrustPath(root);
    if (!normalized) {
        return;
    }
    LockGuard lock(m_mutex);
    const std::string key = detail::pathKey(*normalized);
    if (std::erase_if(m_sessionRoots, [&](const std::string& existing) { return detail::pathKey(existing) == key; }) >
        0)
        ++m_revision;
}

Result<void> TrustStore::requireTrusted(std::string_view path) const {
    if (isTrusted(path)) {
        return {};
    }
    return fail(ErrorCode::PermissionDenied,
                std::format("{} is not in a trusted folder; trust it before building or running project code", path));
}

Result<void> TrustStore::trust(std::string_view root) {
    auto normalized = normalizeTrustPath(root);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    LockGuard lock(m_mutex);
    if (auto loaded = reload(); !loaded) {
        return loaded;
    }
    const std::string key = detail::pathKey(*normalized);
    for (const std::string& existing : m_roots) {
        if (detail::pathKey(existing) == key) {
            return {};
        }
    }
    std::vector<std::string> roots = m_roots;
    roots.push_back(std::move(*normalized));
    if (auto saved = save(roots); !saved) {
        return saved;
    }
    m_roots = std::move(roots);
    ++m_revision;
    return {};
}

Result<void> TrustStore::untrust(std::string_view root) {
    auto normalized = normalizeTrustPath(root);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    LockGuard lock(m_mutex);
    if (auto loaded = reload(); !loaded) {
        return loaded;
    }
    const std::string key = detail::pathKey(*normalized);
    std::vector<std::string> roots;
    for (const std::string& existing : m_roots) {
        if (detail::pathKey(existing) != key) {
            roots.push_back(existing);
        }
    }
    if (roots.size() == m_roots.size()) {
        return {};
    }
    if (auto saved = save(roots); !saved) {
        return saved;
    }
    m_roots = std::move(roots);
    ++m_revision;
    return {};
}

std::vector<std::string> TrustStore::roots() const {
    LockGuard lock(m_mutex);
    return m_roots;
}

std::vector<std::string> TrustStore::warnings() const {
    LockGuard lock(m_mutex);
    return m_warnings;
}

std::vector<std::string> buildTrustRoots(const SolutionModel& model) {
    std::vector<std::string> roots{model.directory()};
    for (const ProjectInfo& project : model.projects()) {
        if (project.path.empty()) {
            continue;
        }
        const std::string_view directory = io::parentPath(project.path);
        bool covered = false;
        for (const std::string& root : roots) {
            covered = covered || detail::isSameOrUnder(directory, root);
        }
        if (!covered) {
            roots.emplace_back(directory);
        }
    }
    return roots;
}

Result<void> checkBuildTrust(const TrustStore& store, const SolutionModel& model) {
    for (const std::string& root : buildTrustRoots(model)) {
        if (!store.isTrusted(root)) {
            return fail(ErrorCode::PermissionDenied,
                        std::format("{} is not trusted; building {} would run its project code", root, model.name()));
        }
    }
    return {};
}

} // namespace frost::project
