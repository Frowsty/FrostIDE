// SPDX-License-Identifier: Apache-2.0
//
// The shell's menu definitions and ShellMenus (frost/ui/shell/shell_menus.hpp). The definitions
// follow the IntelliJ platform's menu layout as its public documentation describes it; ids come
// from the built-in keymap (src/input/src/keymap_intellij.cpp) or from frost/ui/shell/shell_actions.hpp.

#include "frost/ui/shell/shell_menus.hpp"

#include "frost/input/keymap.hpp"
#include "frost/ui/shell/shell_actions.hpp"

#include "shell_util.hpp"
#include <QAction>
#include <QMenu>
#include <QMenuBar>

#include <array>

namespace frost::ui {

namespace {

using Kind = MenuEntry::Kind;

constexpr MenuEntry act(std::string_view id, std::string_view objectName = {}) {
    return MenuEntry{Kind::Action, id, objectName};
}
constexpr MenuEntry sep() { return MenuEntry{Kind::Separator, {}, {}}; }
constexpr MenuEntry sub(std::string_view title) { return MenuEntry{Kind::Submenu, title, {}}; }
constexpr MenuEntry endSub() { return MenuEntry{Kind::EndSubmenu, {}, {}}; }

namespace a = frost::ui::actions;

constexpr std::array kFileMenu{
    act("file.newProject"),
    act("file.new"),
    act("file.newFolder"),
    act("file.rename"),
    act("file.delete"),
    act(a::kOpenFile),
    act("file.openFolder"),
    act("file.openRecent"),
    act("file.closeProject"),
    sep(),
    act(a::kCloseTab),
    act(a::kCloseOtherTabs),
    act(a::kCloseAllTabs),
    act(a::kCloseAllButPinned),
    sep(),
    act("file.saveDocument"),
    act("file.saveAs"),
    act("file.saveAll"),
    act("file.reloadFromDisk"),
    sep(),
    act("file.settings"),
    act("file.projectStructure"),
    act("file.solutionProperties"),
    act("vcs.showLocalHistory"),
    sep(),
    // The M0 window's Exit item kept its object name (tests/app/main_window_test.cpp).
    act(a::kExit, "actionExit"),
};

constexpr std::array kEditMenu{
    act("editor.undo"),
    act("editor.redo"),
    sep(),
    act("editor.cut"),
    act("editor.copy"),
    act("editor.paste"),
    act("editor.pasteFromHistory"),
    sep(),
    act("editor.selectAll"),
    act("editor.extendSelection"),
    act("editor.shrinkSelection"),
    act("editor.selectNextOccurrence"),
    act("editor.unselectOccurrence"),
    act("editor.selectAllOccurrences"),
    sep(),
    act("editor.find"),
    act("editor.replace"),
    act("editor.findNext"),
    act("editor.findPrevious"),
    act("search.findInFiles"),
    act("search.replaceInFiles"),
    act("search.nextOccurrence"),
    act("search.previousOccurrence"),
    sep(),
    act("editor.duplicate"),
    act("editor.deleteLine"),
    act("editor.joinLines"),
    act("editor.toggleCase"),
    act("editor.toggleColumnMode"),
};

constexpr std::array kViewMenu{
    sub("Tool Windows"),
    act(a::kProjectWindow),
    act(a::kCommitWindow),
    act("window.find"),
    act(a::kRunWindow),
    act("window.debug"),
    act(a::kProblemsWindow),
    act("window.todo"),
    act("window.structure"),
    act("window.unitTests"),
    act("window.nuget"),
    act("window.asmViewer"),
    act("window.terminal"),
    act("vcs.log"),
    act(a::kNotificationsWindow),
    endSub(),
    sub("Appearance"),
    act(a::kToggleToolbar),
    act(a::kToggleStatusBar),
    act(a::kCompactMode),
    sep(),
    act(a::kZoomIn),
    act(a::kZoomOut),
    act(a::kResetZoom),
    endSub(),
    act(a::kQuickSwitchTheme),
    sep(),
    act("navigate.quickDefinition"),
    act("navigate.quickDocumentation"),
    act("code.parameterInfo"),
    sep(),
    act("navigate.recentFiles"),
    act("navigate.recentLocations"),
};

constexpr std::array kNavigateMenu{
    act("navigate.back"),
    act("navigate.forward"),
    act("navigate.lastEditLocation"),
    act(a::kFocusNavigationBar),
    sep(),
    act("search.everywhere"),
    act("navigate.gotoClass"),
    act("navigate.gotoFile"),
    act("navigate.gotoSymbol"),
    act(a::kGoToLine),
    sep(),
    act("navigate.declaration"),
    act("navigate.implementation"),
    act("navigate.typeDeclaration"),
    act("navigate.superMethod"),
    act("navigate.relatedFiles"),
    sep(),
    act("navigate.fileStructure"),
    act("navigate.findUsages"),
    act("navigate.showUsages"),
    act("navigate.callHierarchy"),
    act("navigate.typeHierarchy"),
    act("navigate.nextError"),
    act("navigate.previousError"),
    act("navigate.nextMember"),
    act("navigate.previousMember"),
    act("navigate.matchingBrace"),
    sep(),
    act("bookmarks.toggle"),
    act("bookmarks.show"),
    act("bookmarks.next"),
    act("bookmarks.previous"),
};

constexpr std::array kCodeMenu{
    act("code.completion"),
    act("code.showIntentions"),
    act("code.generate"),
    act("code.overrideMethods"),
    act("code.implementMethods"),
    act("code.surroundWith"),
    act("code.insertLiveTemplate"),
    act("code.completeStatement"),
    sep(),
    act("editor.commentLine"),
    act("editor.commentBlock"),
    sep(),
    act("code.reformat"),
    act("code.reformatFile"),
    act("code.optimizeImports"),
    sep(),
    act("editor.moveLineUp"),
    act("editor.moveLineDown"),
    act("editor.moveStatementUp"),
    act("editor.moveStatementDown"),
    sep(),
    act("editor.foldExpand"),
    act("editor.foldCollapse"),
    act("editor.foldExpandAll"),
    act("editor.foldCollapseAll"),
};

constexpr std::array kRefactorMenu{
    act("refactor.this"),
    sep(),
    act("refactor.rename"),
    act("refactor.changeSignature"),
    sep(),
    act("refactor.extractMethod"),
    act("refactor.extractVariable"),
    act("refactor.extractConstant"),
    act("refactor.extractField"),
    act("refactor.extractParameter"),
    act("refactor.inline"),
    sep(),
    act("refactor.move"),
    act("refactor.safeDelete"),
};

// The build tool windows' actions (frost/ui/toolwindows/build_actions.hpp, ADR 0039).
constexpr std::array kBuildMenu{
    act("build.buildSolution"),         act("build.rebuildSolution"),
    act("build.cleanSolution"),         sep(),
    act("build.cancelBuild"),           sep(),
    act("build.configurationManager"),  act("project.manageNuGetPackages"),
    act("project.addProjectReference"),
};

constexpr std::array kRunMenu{
    act("run.run"),
    act("run.debug"),
    act("run.stop"),
    sep(),
    act("run.editConfigurations"),
    act("run.runCompound"),
    act("run.selectConfiguration"),
    act("run.selectDebugConfiguration"),
    act("debug.attachToProcess"),
    sep(),
    act("debug.resume"),
    act("debug.pause"),
    act("debug.stepOver"),
    act("debug.stepInto"),
    act("debug.stepOut"),
    act("debug.runToCursor"),
    act("debug.setNextStatement"),
    act("debug.evaluate"),
    sep(),
    act("debug.toggleBreakpoint"),
};

constexpr std::array kGitMenu{
    act("vcs.commit"),
    act("vcs.log"),
    act("vcs.branches"),
    act("vcs.trustRepository"),
    sep(),
    act("vcs.compareWithHead"),
    act("vcs.compareWithIndex"),
    act("vcs.annotate"),
    act("vcs.showLocalHistory"),
    act("vcs.rollback"),
    sep(),
    act("vcs.stash"),
    act("vcs.unstash"),
    sep(),
    act("vcs.fetch"),
    act("vcs.pull"),
    act("vcs.pullRebase"),
    act("vcs.push"),
    act("vcs.forcePush"),
};

constexpr std::array kWindowMenu{
    act(a::kNextTab),        act(a::kPreviousTab),      act(a::kPinTab),      sep(), act(a::kHideActiveWindow),
    act(a::kHideAllWindows), act(a::kJumpToLastWindow), act(a::kFocusEditor), sep(), act(a::kFocusToolbar),
    act(a::kFocusStatusBar),
};

constexpr std::array kHelpMenu{
    act("search.findAction"),
    sep(),
    act("help.checkForUpdates"),
    act(a::kAbout),
};

constexpr std::array kDefaultMenus{
    MenuDefinition{"&File", kFileMenu},   MenuDefinition{"&Edit", kEditMenu},
    MenuDefinition{"&View", kViewMenu},   MenuDefinition{"&Navigate", kNavigateMenu},
    MenuDefinition{"&Code", kCodeMenu},   MenuDefinition{"&Refactor", kRefactorMenu},
    MenuDefinition{"&Build", kBuildMenu}, MenuDefinition{"&Run", kRunMenu},
    MenuDefinition{"&Git", kGitMenu},     MenuDefinition{"&Window", kWindowMenu},
    MenuDefinition{"&Help", kHelpMenu},
};

// True when `menu` shows any item that is not a separator.
bool hasVisibleItem(const QMenu& menu) {
    for (const QAction* action : menu.actions()) {
        if (action->isVisible() && !action->isSeparator()) {
            return true;
        }
    }
    return false;
}

} // namespace

std::span<const MenuDefinition> defaultMenus() noexcept { return kDefaultMenus; }

ShellMenus::ShellMenus(QMenuBar& bar, input::ActionRegistry& registry, input::KeymapService& keymap,
                       ContextProvider context, Invoker invoke, QObject* parent)
    : QObject(parent), m_bar(bar), m_registry(registry), m_keymap(keymap), m_context(std::move(context)),
      m_invoke(std::move(invoke)) {
    setObjectName(QStringLiteral("shellMenus"));
}

void ShellMenus::build(std::span<const MenuDefinition> menus) {
    m_bar.clear();
    m_menus.clear();
    for (const MenuDefinition& definition : menus) {
        Menu& entry = m_menus.emplace_back();
        entry.title = std::string(definition.title);
        entry.menu = m_bar.addMenu(qtr(shell_detail::kContext, definition.title));
        entry.menu->setObjectName(QStringLiteral("menu.") + shell_detail::fromUtf8(definition.title).remove(u'&'));
        std::span<const MenuEntry> entries = definition.entries;
        addEntries(*entry.menu, entries, entry);
        connect(entry.menu, &QMenu::aboutToShow, this, [this] { refresh(); });
    }
    refresh();
}

void ShellMenus::addEntries(QMenu& target, std::span<const MenuEntry>& entries, Menu& owner) {
    while (!entries.empty()) {
        const MenuEntry entry = entries.front();
        entries = entries.subspan(1);
        switch (entry.kind) {
        case Kind::Separator: target.addSeparator(); break;
        case Kind::EndSubmenu: return;
        case Kind::Submenu: {
            QMenu* submenu = target.addMenu(qtr(shell_detail::kContext, entry.value));
            submenu->setObjectName(QStringLiteral("menu.") + shell_detail::fromUtf8(entry.value));
            owner.submenuActions.push_back(submenu->menuAction());
            addEntries(*submenu, entries, owner);
            break;
        }
        case Kind::Action: {
            QAction* action = target.addAction(QString());
            const std::string_view name = entry.objectName.empty() ? entry.value : entry.objectName;
            action->setObjectName(shell_detail::fromUtf8(name));
            action->setData(shell_detail::fromUtf8(entry.value));
            const std::string id(entry.value);
            connect(action, &QAction::triggered, this, [this, id] {
                if (m_invoke) {
                    m_invoke(id);
                }
            });
            owner.items.push_back(Item{id, action});
            break;
        }
        }
    }
}

void ShellMenus::setChecked(std::string_view actionId, CheckedProvider checked) {
    m_checked.insert_or_assign(std::string(actionId), std::move(checked));
    if (QAction* item = action(actionId); item != nullptr) {
        item->setCheckable(true);
    }
}

void ShellMenus::refresh() {
    const input::ActionContext context = m_context ? m_context() : input::ActionContext{};
    for (Menu& menu : m_menus) {
        for (Item& item : menu.items) {
            refreshItem(item, context);
        }
        // Submenus first (innermost last added), then the menu itself.
        for (auto it = menu.submenuActions.rbegin(); it != menu.submenuActions.rend(); ++it) {
            QMenu* submenu = (*it)->menu();
            tidySeparators(*submenu);
            (*it)->setVisible(hasVisibleItem(*submenu));
        }
        tidySeparators(*menu.menu);
        menu.menu->menuAction()->setVisible(hasVisibleItem(*menu.menu));
    }
    // Read after the refresh: the binding queries may rebuild the index (ADR 0016 decision 8).
    m_registryGeneration = m_registry.generation();
    m_bindingsGeneration = m_keymap.bindingsGeneration();
}

void ShellMenus::refreshIfStale() {
    if (m_registry.generation() != m_registryGeneration || m_keymap.bindingsGeneration() != m_bindingsGeneration) {
        refresh();
    }
}

QAction* ShellMenus::action(std::string_view actionId) const {
    for (const Menu& menu : m_menus) {
        for (const Item& item : menu.items) {
            if (item.actionId == actionId) {
                return item.action;
            }
        }
    }
    return nullptr;
}

QMenu* ShellMenus::menu(std::string_view title) const {
    for (const Menu& menu : m_menus) {
        if (menu.title == title) {
            return menu.menu;
        }
    }
    return nullptr;
}

QString ShellMenus::itemText(input::ActionRegistry& registry, input::KeymapService& keymap, input::ActionId id) {
    QString text = escapeMnemonics(qtr(shell_detail::kActionsContext, registry.info(id).name));
    if (const std::optional<input::Sequence> shortcut = keymap.primaryShortcut(id); shortcut) {
        text += QLatin1Char('\t') +
                shell_detail::fromUtf8(input::toDisplayText(*shortcut, keymap.layout(), keymap.platform()));
    }
    return text;
}

void ShellMenus::refreshItem(Item& item, const input::ActionContext& context) {
    const std::optional<input::ActionId> id = m_registry.find(item.actionId);
    if (!id) {
        item.action->setVisible(false);
        return;
    }
    const input::ActionInfo& info = m_registry.info(*id);
    item.action->setVisible(true);
    item.action->setText(itemText(m_registry, m_keymap, *id));
    item.action->setStatusTip(qtr(shell_detail::kActionsContext, info.description));
    item.action->setEnabled(m_registry.isEnabled(*id, context));
    if (const auto checked = m_checked.find(item.actionId); checked != m_checked.end() && checked->second) {
        item.action->setCheckable(true);
        item.action->setChecked(checked->second());
    }
}

void ShellMenus::tidySeparators(QMenu& menu) {
    // Hide leading, trailing and doubled separators among the visible items.
    QAction* pending = nullptr;
    bool seenItem = false;
    for (QAction* action : menu.actions()) {
        if (action->isSeparator()) {
            action->setVisible(false);
            if (seenItem) {
                pending = action;
            }
            continue;
        }
        if (!action->isVisible()) {
            continue;
        }
        if (pending != nullptr) {
            pending->setVisible(true);
            pending = nullptr;
        }
        seenItem = true;
    }
}

} // namespace frost::ui
