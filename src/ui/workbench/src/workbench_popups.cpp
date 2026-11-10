// SPDX-License-Identifier: Apache-2.0
//
// Workbench opening and navigation, Find Action / Go to File / Search Everywhere / Recent Files /
// Recent Locations popups, and bookmarks.

#include "frost/input/keymap.hpp"
#include "frost/search/fuzzy_path.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/workbench/file_system_view.hpp"

#include "workbench_internal.hpp"
#include <QLineEdit>

#include <iterator>
#include <set>

namespace frost::ui {

using workbench_detail::fileIcon;
using workbench_detail::fromUtf8;
using workbench_detail::nativePath;
using workbench_detail::text;
using workbench_detail::toUtf8;

// --- Opening and navigation ---

nav::Place Workbench::currentPlace() const {
    const IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr) {
        return {};
    }
    const EditorStatus status = editor->status();
    return nav::Place{editor->path(), status.line, status.column};
}

Result<int> Workbench::openFile(const std::string& path, std::optional<LinePosition> at, bool statusColumn) {
    const nav::Place from = currentPlace();
    m_navigating = true;
    Result<int> opened = m_shell.openFile(path);
    if (opened) {
        IEditorView* editor = m_shell.tabs().editor(*opened);
        if (at) {
            if (statusColumn)
                editor->goToStatusPosition(at->line, at->column);
            else
                editor->goToLine(at->line, at->column);
        }
        const EditorStatus status = editor->status();
        const nav::Place to{editor->path(), status.line, status.column};
        if (!from.path.empty() && !nav::isNear(from, to, nav::NavigationHistory::kNearLines)) {
            m_history.recordJump(from);
            m_recentLocations.touch(from);
        }
        m_recentLocations.touch(to);
        m_recentFiles.touch(editor->path());
        m_lastPlaces[editor->path()] = to;
        m_activePath = editor->path();
        syncEditorBookmarks(editor->path());
        m_shell.focusEditor();
    }
    m_navigating = false;
    return opened;
}

bool Workbench::openPlace(const nav::Place& place) {
    m_navigating = true;
    Result<int> opened = m_shell.openFile(place.path);
    if (opened) {
        IEditorView* editor = m_shell.tabs().editor(*opened);
        editor->goToStatusPosition(place.line, place.column);
        m_lastPlaces[editor->path()] = place;
        m_activePath = editor->path();
        m_recentFiles.touch(editor->path());
        syncEditorBookmarks(editor->path());
        m_shell.focusEditor();
    } else {
        m_history.forget(place.path);
    }
    m_navigating = false;
    return opened.has_value();
}

bool Workbench::goBack() {
    const auto target = m_history.back(currentPlace());
    return target && openPlace(*target);
}

bool Workbench::goForward() {
    const auto target = m_history.forward(currentPlace());
    return target && openPlace(*target);
}

void Workbench::onEditorStatus(IEditorView* editor) {
    if (editor == nullptr || m_restoring) {
        return;
    }
    const EditorStatus status = editor->status();
    const nav::Place now{editor->path(), status.line, status.column};
    auto& last = m_lastPlaces[editor->path()];
    if (!m_navigating && !last.path.empty()) {
        if (m_history.noteCaretMove(last, now)) {
            m_recentLocations.touch(last);
        }
    }
    last = now;
}

void Workbench::onCurrentEditorChanged(IEditorView* editor) {
    const std::string next = editor != nullptr ? editor->path() : std::string();
    if (!m_navigating && !m_restoring && !m_activePath.empty() && next != m_activePath && !next.empty()) {
        // A switch through the tab bar or the switcher: the place left is a jump.
        if (const auto it = m_lastPlaces.find(m_activePath); it != m_lastPlaces.end()) {
            m_history.recordJump(it->second);
        }
    }
    if (!next.empty() && !m_restoring) {
        m_recentFiles.touch(next);
        if (!m_lastPlaces.contains(next)) {
            const EditorStatus status = editor->status();
            m_lastPlaces[next] = nav::Place{next, status.line, status.column};
        }
    }
    m_activePath = next;
}

// --- Popups ---

void Workbench::closeOpenPopup() {
    // The context a popup captures must be the main window's, not the popup's being replaced.
    if (Popup* open = m_shell.activePopup(); open != nullptr) {
        open->closePopup(Popup::CloseReason::Dismissed);
    }
}

std::vector<ListPopupItem> Workbench::fileItems(const std::string& pattern, std::size_t limit) const {
    // The project's candidates, then recent files outside it (opened from elsewhere).
    std::vector<std::string> absolute = m_files->candidates();
    std::vector<std::string> display = m_files->displayCandidates();
    const std::set<std::string, std::less<>> known(absolute.begin(), absolute.end());
    for (const std::string& recent : m_recentFiles.paths()) {
        if (!known.contains(recent)) {
            absolute.push_back(recent);
            display.push_back(m_root.empty() ? recent : nav::relativeTo(m_root, recent));
        }
    }
    std::vector<std::size_t> order;
    if (pattern.empty()) {
        // Rider's empty Go to File: the recent files first.
        std::set<std::size_t> taken;
        for (const std::string& recent : m_recentFiles.paths()) {
            if (const auto it = std::ranges::find(absolute, recent); it != absolute.end()) {
                const auto index = static_cast<std::size_t>(it - absolute.begin());
                if (taken.insert(index).second) {
                    order.push_back(index);
                }
            }
        }
        for (std::size_t i = 0; i < absolute.size() && order.size() < limit; ++i) {
            if (!taken.contains(i)) {
                order.push_back(i);
            }
        }
        if (order.size() > limit) {
            order.resize(limit);
        }
    } else {
        for (const search::RankedPath& ranked : search::rankPaths(pattern, display, limit)) {
            order.push_back(ranked.index);
        }
    }
    std::vector<ListPopupItem> items;
    items.reserve(order.size());
    for (const std::size_t index : order) {
        const std::string& path = absolute[index];
        const std::string_view name = nav::baseName(path);
        const std::string_view folder = nav::parentOf(display[index] == path ? path : display[index]);
        ListPopupItem item;
        item.text = fromUtf8(name);
        item.detail = nativePath(folder);
        item.icon = fileIcon(name);
        item.data = fileData(path);
        items.push_back(std::move(item));
    }
    return items;
}

std::vector<ListPopupItem> Workbench::actionItems(const std::string& query, const input::ActionContext& context,
                                                  std::size_t limit) const {
    std::vector<ListPopupItem> items;
    if (QString::fromUtf8(query).trimmed().isEmpty()) {
        return items;
    }
    const ShellEnvironment& env = m_shell.environment();
    input::FindActionOptions options;
    options.limit = limit;
    options.includeDisabled = true;
    for (const input::FindActionHit& hit : m_findActions.query(query, env.keymap.bindings(), context, options)) {
        const input::ActionInfo& info = env.registry.info(hit.id);
        ListPopupItem item;
        item.text = qtr(kActionsContext, info.name);
        const QString shortcut = shortcutText(info.id);
        const QString group = info.group.empty() ? QString() : qtr(kActionsContext, info.group);
        item.detail =
            shortcut.isEmpty() ? group : (group.isEmpty() ? shortcut : shortcut + QStringLiteral("  ") + group);
        item.enabled = hit.enabled;
        QVariantMap data;
        data.insert(QStringLiteral("kind"), QStringLiteral("action"));
        data.insert(QStringLiteral("id"), fromUtf8(info.id));
        item.data = data;
        items.push_back(std::move(item));
    }
    return items;
}

void Workbench::acceptItem(const QVariant& data, const input::ActionContext& captured) {
    const QVariantMap map = data.toMap();
    const QString kind = map.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("file")) {
        std::optional<LinePosition> at;
        if (map.contains(QStringLiteral("line"))) {
            at = LinePosition{map.value(QStringLiteral("line")).toInt(), map.value(QStringLiteral("column")).toInt()};
        }
        static_cast<void>(openFile(toUtf8(map.value(QStringLiteral("path")).toString()), at));
        return;
    }
    if (kind == QStringLiteral("place")) {
        const nav::Place place{toUtf8(map.value(QStringLiteral("path")).toString()),
                               map.value(QStringLiteral("line")).toInt(), map.value(QStringLiteral("column")).toInt()};
        static_cast<void>(openFile(place.path, LinePosition{place.line, place.column}, true));
        return;
    }
    if (kind == QStringLiteral("mnemonicBookmark")) {
        const int digit = map.value(QStringLiteral("digit")).toInt();
        const std::string path = toUtf8(map.value(QStringLiteral("path")).toString());
        const int line = map.value(QStringLiteral("line")).toInt();
        static_cast<void>(toggleNumberedBookmarkAt(digit, path, line));
        return;
    }
    if (kind == QStringLiteral("action")) {
        input::ActionRegistry& registry = m_shell.environment().registry;
        const std::optional<input::ActionId> id = registry.find(toUtf8(map.value(QStringLiteral("id")).toString()));
        if (!id) {
            return;
        }
        // The focus keys from before the popup opened; level and trust as they are now.
        input::ActionContext context = captured;
        const input::ActionContext live = m_shell.actionContext();
        context.level = live.level;
        context.trusted = live.trusted;
        if (registry.tryInvoke(*id, context, input::InvokeInfo{input::InvokeSource::FindAction, false}) ==
            input::InvokeResult::Ran) {
            m_findActions.noteInvoked(*id);
        }
        m_shell.refreshActions();
    }
}

ListPopup* Workbench::showFindAction(const QString& query) {
    closeOpenPopup();
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Find Action"), true);
    popup->setObjectName(QStringLiteral("findActionPopup"));
    popup->setFilterMode(ListPopup::FilterMode::External);
    connect(popup, &ListPopup::filterTextChanged, popup, [this, popup, captured](const QString& value) {
        popup->setItems(actionItems(toUtf8(value), captured, 100));
    });
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    m_shell.openPopup(popup);
    if (!query.isEmpty()) {
        popup->setFilterText(query);
    }
    return popup;
}

ListPopup* Workbench::showGoToFile(const QString& query) {
    closeOpenPopup();
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Go to File"), true);
    popup->setObjectName(QStringLiteral("goToFilePopup"));
    popup->setFilterMode(ListPopup::FilterMode::External);
    const auto update = [this, popup](const QString& value) {
        const search::LocationQuery location = search::parseLocationQuery(toUtf8(value.trimmed()));
        std::vector<ListPopupItem> items = fileItems(location.pattern, 50);
        if (location.line) {
            for (ListPopupItem& item : items) {
                item.data = fileData(toUtf8(item.data.toMap().value(QStringLiteral("path")).toString()), location.line,
                                     location.column);
            }
        }
        popup->setItems(std::move(items));
    };
    connect(popup, &ListPopup::filterTextChanged, popup, update);
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    update(QString());
    m_shell.openPopup(popup);
    if (!query.isEmpty()) {
        popup->setFilterText(query);
    }
    return popup;
}

ListPopup* Workbench::showSearchEverywhere(const QString& query, SearchEverywhereCategory category) {
    closeOpenPopup();
    const input::ActionContext captured = m_shell.actionContext();
    const auto categoryTitle = [](SearchEverywhereCategory cat) -> QString {
        switch (cat) {
        case SearchEverywhereCategory::All:
            return text("Search Everywhere  [All]  Classes  Files  Symbols  Actions  (Tab)");
        case SearchEverywhereCategory::Classes:
            return text("Search Everywhere  All  [Classes]  Files  Symbols  Actions  (Tab)");
        case SearchEverywhereCategory::Files:
            return text("Search Everywhere  All  Classes  [Files]  Symbols  Actions  (Tab)");
        case SearchEverywhereCategory::Symbols:
            return text("Search Everywhere  All  Classes  Files  [Symbols]  Actions  (Tab)");
        case SearchEverywhereCategory::Actions:
            return text("Search Everywhere  All  Classes  Files  Symbols  [Actions]  (Tab)");
        }
        return text("Search Everywhere");
    };
    auto* popup = new ListPopup(&m_shell, categoryTitle(category), true);
    popup->setObjectName(QStringLiteral("searchEverywherePopup"));
    popup->setFilterMode(ListPopup::FilterMode::External);
    popup->setScopeTabs({text("All"), text("Classes"), text("Files"), text("Symbols"), text("Actions")},
                        static_cast<int>(category));
    auto currentCategory = std::make_shared<SearchEverywhereCategory>(category);
    auto queryGen = std::make_shared<std::uint64_t>(0);
    const auto buildSyncItems = [this, captured](const search::LocationQuery& location, const std::string& utf8,
                                                 SearchEverywhereCategory cat,
                                                 const std::vector<WorkbenchSymbol>* symbols) {
        std::vector<ListPopupItem> items;
        if (symbols != nullptr && !symbols->empty() &&
            (cat == SearchEverywhereCategory::All || cat == SearchEverywhereCategory::Classes ||
             cat == SearchEverywhereCategory::Symbols)) {
            ListPopupItem header;
            header.text = cat == SearchEverywhereCategory::Classes ? text("Classes") : text("Symbols");
            header.header = true;
            items.push_back(std::move(header));
            const std::size_t limit = cat == SearchEverywhereCategory::All ? 8 : 50;
            for (std::size_t i = 0; i < symbols->size() && i < limit; ++i) {
                const WorkbenchSymbol& sym = (*symbols)[i];
                ListPopupItem item;
                item.text = fromUtf8(sym.name);
                const QString fileRel = nativePath(m_root.empty() ? sym.path : nav::relativeTo(m_root, sym.path));
                item.detail =
                    sym.container.empty() ? fileRel : fromUtf8(sym.container) + QStringLiteral("  ") + fileRel;
                item.icon = fileIcon(nav::baseName(sym.path));
                QVariantMap data;
                data.insert(QStringLiteral("kind"), QStringLiteral("place"));
                data.insert(QStringLiteral("path"), fromUtf8(sym.path));
                data.insert(QStringLiteral("line"), sym.line);
                data.insert(QStringLiteral("column"), sym.column);
                item.data = data;
                items.push_back(std::move(item));
            }
        }
        if (cat == SearchEverywhereCategory::All || cat == SearchEverywhereCategory::Files) {
            const std::size_t limit = cat == SearchEverywhereCategory::Files ? 50 : 8;
            std::vector<ListPopupItem> files = fileItems(location.pattern, limit);
            if (!files.empty()) {
                ListPopupItem header;
                header.text = text("Files");
                header.header = true;
                items.push_back(std::move(header));
                for (ListPopupItem& item : files) {
                    if (location.line) {
                        item.data = fileData(toUtf8(item.data.toMap().value(QStringLiteral("path")).toString()),
                                             location.line, location.column);
                    }
                    items.push_back(std::move(item));
                }
            }
        }
        if (cat == SearchEverywhereCategory::All || cat == SearchEverywhereCategory::Actions) {
            const std::size_t limit = cat == SearchEverywhereCategory::Actions ? 50 : 8;
            std::vector<ListPopupItem> actions = actionItems(utf8, captured, limit);
            if (!actions.empty()) {
                ListPopupItem header;
                header.text = text("Actions");
                header.header = true;
                items.push_back(std::move(header));
                std::ranges::move(actions, std::back_inserter(items));
            }
        }
        return items;
    };
    const auto update = [this, popup, currentCategory, queryGen, buildSyncItems](const QString& value) {
        const std::uint64_t gen = ++(*queryGen);
        const std::string utf8 = toUtf8(value.trimmed());
        const search::LocationQuery location = search::parseLocationQuery(utf8);
        const SearchEverywhereCategory cat = *currentCategory;
        popup->setItems(buildSyncItems(location, utf8, cat, nullptr));
        const bool wantsSymbols = (cat == SearchEverywhereCategory::All || cat == SearchEverywhereCategory::Classes ||
                                   cat == SearchEverywhereCategory::Symbols) &&
                                  !location.pattern.empty() && static_cast<bool>(m_services.queryWorkspaceSymbols);
        if (wantsSymbols) {
            const QPointer<ListPopup> safePopup(popup);
            const bool classesOnly = (cat == SearchEverywhereCategory::Classes);
            m_services.queryWorkspaceSymbols(
                location.pattern, classesOnly,
                [safePopup, queryGen, gen, location, utf8, cat, classesOnly,
                 buildSyncItems](std::vector<WorkbenchSymbol> symbols) {
                    if (safePopup == nullptr || *queryGen != gen) {
                        return;
                    }
                    if (classesOnly) {
                        std::erase_if(symbols, [](const WorkbenchSymbol& s) { return !s.isClassLike; });
                    }
                    safePopup->setItems(buildSyncItems(location, utf8, cat, &symbols));
                });
        }
    };
    connect(popup, &ListPopup::filterTextChanged, popup, update);
    const auto switchCategory = [popup, currentCategory, categoryTitle, update](int next) {
        if (next < 0 || next >= 5) {
            return;
        }
        *currentCategory = static_cast<SearchEverywhereCategory>(next);
        popup->setTitle(categoryTitle(*currentCategory));
        popup->setActiveScopeTab(next);
        update(popup->filterField() != nullptr ? popup->filterField()->text() : QString());
    };
    connect(popup, &ListPopup::tabPressed, popup, [currentCategory, switchCategory](bool backward) {
        const int count = 5;
        const int cur = static_cast<int>(*currentCategory);
        const int next = backward ? (cur + count - 1) % count : (cur + 1) % count;
        switchCategory(next);
    });
    connect(popup, &ListPopup::scopeTabClicked, popup, switchCategory);
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    update(QString());
    m_shell.openPopup(popup);
    if (!query.isEmpty()) {
        popup->setFilterText(query);
    }
    return popup;
}

ListPopup* Workbench::showRecentFiles() {
    closeOpenPopup();
    const auto paths = m_recentFiles.paths();
    if (paths.empty()) {
        return nullptr;
    }
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Recent Files"), true);
    popup->setObjectName(QStringLiteral("recentFilesPopup"));
    std::vector<ListPopupItem> items;
    for (const std::string& path : paths) {
        ListPopupItem item;
        item.text = fromUtf8(nav::baseName(path));
        item.detail = nativePath(nav::parentOf(m_root.empty() ? path : nav::relativeTo(m_root, path)));
        item.icon = fileIcon(nav::baseName(path));
        item.data = fileData(path);
        items.push_back(std::move(item));
    }
    popup->setItems(std::move(items));
    // The current file is first; the useful default is the one before it.
    const bool currentFirst = paths.front() == m_activePath;
    popup->setCurrentItem(currentFirst && paths.size() > 1 ? 1 : 0);
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    m_shell.openPopup(popup);
    return popup;
}

ListPopup* Workbench::showRecentLocations() {
    closeOpenPopup();
    const auto places = m_recentLocations.places();
    if (places.empty()) {
        return nullptr;
    }
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Recent Locations"), true);
    popup->setObjectName(QStringLiteral("recentLocationsPopup"));
    std::vector<ListPopupItem> items;
    for (const nav::Place& place : places) {
        ListPopupItem item;
        item.text = QStringLiteral("%1:%2").arg(fromUtf8(nav::baseName(place.path))).arg(place.line);
        item.detail = nativePath(nav::parentOf(m_root.empty() ? place.path : nav::relativeTo(m_root, place.path)));
        item.icon = fileIcon(nav::baseName(place.path));
        QVariantMap data;
        data.insert(QStringLiteral("kind"), QStringLiteral("place"));
        data.insert(QStringLiteral("path"), fromUtf8(place.path));
        data.insert(QStringLiteral("line"), place.line);
        data.insert(QStringLiteral("column"), place.column);
        item.data = data;
        items.push_back(std::move(item));
    }
    popup->setItems(std::move(items));
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    m_shell.openPopup(popup);
    return popup;
}

void Workbench::recordEditLocation(const std::string& path, int line, int column) {
    if (path.empty()) {
        return;
    }
    m_lastEditPlace = nav::Place{m_services.documents.canonicalize(path), std::max(1, line), std::max(1, column)};
    m_shell.refreshActions();
}

bool Workbench::goToLastEditLocation() {
    if (!m_lastEditPlace || m_lastEditPlace->path.empty()) {
        return false;
    }
    return openFile(m_lastEditPlace->path, LinePosition{m_lastEditPlace->line, m_lastEditPlace->column}, true)
        .has_value();
}

void Workbench::syncEditorBookmarks(const std::string& path) {
    std::vector<int> lines;
    if (const auto it = m_bookmarks.find(path); it != m_bookmarks.end()) {
        lines.assign(it->second.begin(), it->second.end());
    }
    m_services.editors.setBookmarks(path, lines);
    for (EditorTabGroup* group : m_shell.tabs().groups()) {
        for (int i = 0; i < group->count(); ++i) {
            if (IEditorView* editor = group->editor(i); editor != nullptr && editor->path() == path) {
                editor->setBookmarks(lines);
            }
        }
    }
}

bool Workbench::toggleBookmark() {
    const IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr || editor->path().empty()) {
        return false;
    }
    return toggleBookmarkAt(editor->path(), editor->status().line);
}

bool Workbench::toggleBookmarkAt(const std::string& path, int line) {
    if (path.empty() || line < 1) {
        return false;
    }
    const std::string canonical = m_services.documents.canonicalize(path);
    auto& lines = m_bookmarks[canonical];
    if (lines.contains(line)) {
        lines.erase(line);
        if (lines.empty()) {
            m_bookmarks.erase(canonical);
        }
        for (auto& slot : m_numberedBookmarks) {
            if (slot && slot->path == canonical && slot->line == line) {
                slot.reset();
            }
        }
    } else {
        lines.insert(line);
    }
    syncEditorBookmarks(canonical);
    m_shell.refreshActions();
    return true;
}

bool Workbench::toggleNumberedBookmark(int digit) {
    const IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr || editor->path().empty()) {
        return false;
    }
    return toggleNumberedBookmarkAt(digit, editor->path(), editor->status().line);
}

bool Workbench::toggleNumberedBookmarkAt(int digit, const std::string& path, int line) {
    if (digit < 1 || digit > 9 || path.empty() || line < 1) {
        return false;
    }
    const std::string canonical = m_services.documents.canonicalize(path);
    auto& slot = m_numberedBookmarks[static_cast<std::size_t>(digit - 1)];
    if (slot && slot->path == canonical && slot->line == line) {
        slot.reset();
        if (const auto it = m_bookmarks.find(canonical); it != m_bookmarks.end()) {
            it->second.erase(line);
            if (it->second.empty()) {
                m_bookmarks.erase(it);
            }
        }
        syncEditorBookmarks(canonical);
        m_shell.refreshActions();
        return true;
    }

    if (slot) {
        const std::string oldPath = slot->path;
        const int oldLine = slot->line;
        slot.reset();
        if (const auto it = m_bookmarks.find(oldPath); it != m_bookmarks.end()) {
            it->second.erase(oldLine);
            if (it->second.empty()) {
                m_bookmarks.erase(it);
            }
        }
        if (oldPath != canonical) {
            syncEditorBookmarks(oldPath);
        }
    }

    for (auto& other : m_numberedBookmarks) {
        if (other && other->path == canonical && other->line == line) {
            other.reset();
        }
    }

    slot = nav::Place{canonical, line, 1};
    m_bookmarks[canonical].insert(line);
    syncEditorBookmarks(canonical);
    m_shell.refreshActions();
    return true;
}

bool Workbench::goToNumberedBookmark(int digit) {
    const std::optional<nav::Place> place = numberedBookmark(digit);
    if (!place || place->path.empty()) {
        return false;
    }
    return openFile(place->path, LinePosition{place->line, place->column}, true).has_value();
}

std::optional<nav::Place> Workbench::numberedBookmark(int digit) const noexcept {
    if (digit < 1 || digit > 9) {
        return std::nullopt;
    }
    return m_numberedBookmarks[static_cast<std::size_t>(digit - 1)];
}

ListPopup* Workbench::showMnemonicBookmarkPopup() {
    const IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr || editor->path().empty()) {
        return nullptr;
    }
    closeOpenPopup();
    const std::string path = m_services.documents.canonicalize(editor->path());
    const int line = editor->status().line;
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Bookmark Mnemonic"), true);
    popup->setObjectName(QStringLiteral("mnemonicBookmarkPopup"));
    std::vector<ListPopupItem> items;
    items.reserve(9);
    for (int d = 1; d <= 9; ++d) {
        ListPopupItem item;
        item.text = QString::number(d);
        if (const auto& existing = m_numberedBookmarks[static_cast<std::size_t>(d - 1)]; existing) {
            item.detail = QStringLiteral("%1:%2").arg(fromUtf8(nav::baseName(existing->path))).arg(existing->line);
        }
        QVariantMap data;
        data.insert(QStringLiteral("kind"), QStringLiteral("mnemonicBookmark"));
        data.insert(QStringLiteral("digit"), d);
        data.insert(QStringLiteral("path"), fromUtf8(path));
        data.insert(QStringLiteral("line"), line);
        item.data = data;
        items.push_back(std::move(item));
    }
    popup->setItems(std::move(items));
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    m_shell.openPopup(popup);
    return popup;
}

bool Workbench::selectInProjectView() {
    const IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr || editor->path().empty() || m_root.empty()) {
        return false;
    }
    m_shell.toolWindows().show(kFileSystemToolWindow);
    if (m_fileSystem != nullptr) {
        m_fileSystem->selectWhenAvailable(editor->path());
    }
    return true;
}

std::vector<nav::Place> Workbench::bookmarks() const {
    std::vector<nav::Place> all;
    for (const auto& [path, lines] : m_bookmarks) {
        for (const int line : lines) {
            all.push_back(nav::Place{path, line, 1});
        }
    }
    return all;
}

bool Workbench::goToNextBookmark() {
    const std::vector<nav::Place> all = bookmarks();
    if (all.empty()) {
        return false;
    }
    const nav::Place cur = currentPlace();
    for (const nav::Place& bm : all) {
        if (bm.path > cur.path || (bm.path == cur.path && bm.line > cur.line)) {
            return openFile(bm.path, LinePosition{bm.line, bm.column}).has_value();
        }
    }
    return openFile(all.front().path, LinePosition{all.front().line, all.front().column}).has_value();
}

bool Workbench::goToPreviousBookmark() {
    const std::vector<nav::Place> all = bookmarks();
    if (all.empty()) {
        return false;
    }
    const nav::Place cur = currentPlace();
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        if (it->path < cur.path || (it->path == cur.path && it->line < cur.line)) {
            return openFile(it->path, LinePosition{it->line, it->column}).has_value();
        }
    }
    return openFile(all.back().path, LinePosition{all.back().line, all.back().column}).has_value();
}

ListPopup* Workbench::showBookmarksPopup() {
    closeOpenPopup();
    const std::vector<nav::Place> all = bookmarks();
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Bookmarks"), true);
    popup->setObjectName(QStringLiteral("bookmarksPopup"));
    std::vector<ListPopupItem> items;
    for (const nav::Place& place : all) {
        ListPopupItem item;
        item.text = QStringLiteral("%1:%2").arg(fromUtf8(nav::baseName(place.path))).arg(place.line);
        item.detail = nativePath(nav::parentOf(m_root.empty() ? place.path : nav::relativeTo(m_root, place.path)));
        item.icon = fileIcon(nav::baseName(place.path));
        QVariantMap data;
        data.insert(QStringLiteral("kind"), QStringLiteral("place"));
        data.insert(QStringLiteral("path"), fromUtf8(place.path));
        data.insert(QStringLiteral("line"), place.line);
        data.insert(QStringLiteral("column"), place.column);
        item.data = data;
        items.push_back(std::move(item));
    }
    popup->setItems(std::move(items));
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    m_shell.openPopup(popup);
    return popup;
}

} // namespace frost::ui
