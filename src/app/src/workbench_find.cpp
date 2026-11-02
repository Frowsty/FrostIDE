// SPDX-License-Identifier: Apache-2.0
// Connect Find and Replace in Files to the live Workbench and its document views.
#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/find/find_feature.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace frost::app {

void Workbench::attachFind() {
    ui::FindFeatureServices services;
    services.scheduler = m_pool.get();
    services.documents = m_documents.get();
    services.solution = [this] { return m_solution.get(); };
    services.currentFile = [this] {
        const DocumentEditorView* view = currentView();
        return view ? view->path() : std::string();
    };
    services.defaultDirectory = [this] { return m_projectRoot; };
    services.selectedText = [this] {
        DocumentEditorView* view = currentView();
        if (!view)
            return QString();
        const auto& editor = view->editor().editor();
        const auto selection = editor.state().primary();
        if (selection.empty())
            return QString();
        const std::string text = editor.text().text(selection.range());
        return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    };
    services.openLocation = [this](const ui::FindLocation& location) {
        const auto limit = static_cast<std::uint32_t>(std::numeric_limits<int>::max() - 1);
        const int line = static_cast<int>(std::min(location.line, limit)) + 1;
        const int column = static_cast<int>(std::min(location.column, limit)) + 1;
        // Results are files even when named .sln or .frostproj; openPath dispatches those as projects.
        if (m_nav) {
            const std::string canonical = m_documentRegistry->canonicalize(location.path);
            (void)m_nav->openFile(canonical, ui::LinePosition{line, column});
        }
    };
    services.documentEdited = [this](document::FileDocument& document, const text::ChangeSet& change) {
        if (DocumentEditorView* view = m_editors->find(document)) {
            if (auto followed = view->editor().followExternalChange(change); !followed) {
                log::warn("workbench", "find replacement display synchronization: {}", followed.error().describe());
                view->reloadFromDocument();
                syncClangdDocument(*view);
                syncLuaDocument(*view);
                syncManagedDocument(*view);
            }
        }
        m_documentRegistry->notifyChanged(document.path());
    };
    m_find = std::make_unique<ui::FindFeature>(*m_window, std::move(services));
    if (auto installed = m_find->install(); !installed) {
        log::warn("workbench", "find actions: {}", installed.error().describe());
    }
}

void Workbench::cancelFind() {
    if (m_find)
        m_find->cancelSearches();
}

} // namespace frost::app
