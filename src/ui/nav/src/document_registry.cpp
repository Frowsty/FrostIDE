// SPDX-License-Identifier: Apache-2.0
//
// DocumentRegistry (frost/ui/nav/document_registry.hpp).

#include "frost/ui/nav/document_registry.hpp"

#include "frost/io/path.hpp"
#include "frost/ui/nav/paths.hpp"

#include <algorithm>
#include <utility>

namespace frost::ui::nav {

DocumentRegistry::DocumentRegistry(document::SaveManager& manager, io::IFileSystem& fs) noexcept
    : m_manager(&manager), m_fs(&fs) {}

std::string DocumentRegistry::canonicalize(std::string_view path) const {
    if (auto canonical = m_fs->canonicalPath(path); canonical) {
        return std::move(*canonical);
    }
    if (auto normalized = io::normalizePath(path, styleOf(path)); normalized) {
        return std::move(*normalized);
    }
    return std::string(path);
}

Result<OpenedDocument> DocumentRegistry::open(std::string_view path) {
    std::string canonical = canonicalize(path);
    auto opened = m_manager->open(canonical);
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    return OpenedDocument{*opened, std::move(canonical)};
}

document::FileDocument* DocumentRegistry::find(std::string_view path) const {
    return m_manager->find(canonicalize(path));
}

void DocumentRegistry::acquire(const std::string& path) { ++m_views[path]; }

void DocumentRegistry::release(const std::string& path) {
    const auto it = m_views.find(path);
    if (it == m_views.end()) {
        return;
    }
    if (--it->second > 0) {
        return;
    }
    m_views.erase(it);
    if (document::FileDocument* document = m_manager->find(path);
        document != nullptr && !document->dirty() && document->diskState() != document::DiskState::Changed) {
        // A failed journal removal leaves a stale entry the next restore offers; nothing to report here.
        static_cast<void>(m_manager->close(*document));
    }
}

int DocumentRegistry::viewCount(std::string_view path) const {
    const auto it = m_views.find(path);
    return it == m_views.end() ? 0 : it->second;
}

std::uint64_t DocumentRegistry::subscribe(Listener listener) {
    const std::uint64_t id = m_nextListener++;
    m_listeners.emplace_back(id, std::move(listener));
    return id;
}

void DocumentRegistry::unsubscribe(std::uint64_t id) {
    std::erase_if(m_listeners, [id](const auto& entry) { return entry.first == id; });
}

void DocumentRegistry::notifyChanged(const std::string& path) {
    // A copy: a listener may unsubscribe while it runs.
    const auto listeners = m_listeners;
    for (const auto& [id, listener] : listeners) {
        if (listener) {
            listener(path);
        }
    }
}

} // namespace frost::ui::nav
