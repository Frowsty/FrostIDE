// SPDX-License-Identifier: Apache-2.0
//
// The menu bar built from frost::input's action registry (PLAN.md row 239, ADR 0016 Consequences,
// ADR 0035): a data-driven definition names action ids; each item shows the action's (translated)
// name and its primary shortcut from the keymap as text, never as a QAction shortcut, so the
// shortcut dispatcher stays the only key path. Unregistered actions are hidden, placeholders and
// otherwise disabled actions are shown disabled, and a menu with nothing visible is hidden.
#pragma once

#include "frost/input/action.hpp"
#include "frost/input/keymap_service.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class QAction;
class QMenu;
class QMenuBar;

namespace frost::ui {

// One entry of a menu definition.
struct MenuEntry {
    enum class Kind : std::uint8_t { Action, Separator, Submenu, EndSubmenu };
    Kind kind = Kind::Action;
    // The action id (Action) or the English submenu title (Submenu).
    std::string_view value;
    // The QAction's object name; empty: the action id.
    std::string_view objectName;
};

// A top-level menu: its English title with its mnemonic ("&File") and its entries.
struct MenuDefinition {
    std::string_view title;
    std::span<const MenuEntry> entries;
};

// The shell's menus: File, Edit, View, Navigate, Code, Build, Run, Window, Help.
[[nodiscard]] std::span<const MenuDefinition> defaultMenus() noexcept;

/*
 * Builds and refreshes the menus in `bar`. `context` gives the ActionContext for enabled states,
 * `invoke` runs an action chosen from a menu. Refreshing re-reads the registry and the keymap: a
 * menu refreshes itself whenever it is about to show, and refreshIfStale() catches registrations
 * and keymap or layout changes for the menu bar's own visibility. The registry, the service and
 * the bar must outlive this object. GUI thread only. O(items) per refresh.
 */
class ShellMenus final : public QObject {
    Q_OBJECT

public:
    using ContextProvider = std::function<input::ActionContext()>;
    using Invoker = std::function<void(std::string_view actionId)>;
    using CheckedProvider = std::function<bool()>;

    ShellMenus(QMenuBar& bar, input::ActionRegistry& registry, input::KeymapService& keymap, ContextProvider context,
               Invoker invoke, QObject* parent = nullptr);

    // Replaces the menu bar's contents with `menus`.
    void build(std::span<const MenuDefinition> menus);
    // Makes the action's item checkable; `checked` is read on each refresh.
    void setChecked(std::string_view actionId, CheckedProvider checked);
    // Re-reads everything now.
    void refresh();
    // refresh() when the registry or the keymap's bindings changed since the last refresh.
    void refreshIfStale();

    // The item for an action id (nullptr when no menu names it).
    [[nodiscard]] QAction* action(std::string_view actionId) const;
    // The top-level menu with the English title `title` ("&File"), or nullptr.
    [[nodiscard]] QMenu* menu(std::string_view title) const;

    // "Name\tShortcut" as a menu item shows it: the translated name (ampersands escaped) and the
    // primary shortcut's display text, or the name alone.
    [[nodiscard]] static QString itemText(input::ActionRegistry& registry, input::KeymapService& keymap,
                                          input::ActionId id);

private:
    struct Item {
        std::string actionId;
        QAction* action = nullptr;
    };
    struct Menu {
        std::string title;
        QMenu* menu = nullptr;
        std::vector<Item> items;
        // Submenus are refreshed with their parent; a submenu's QAction in its parent menu.
        std::vector<QAction*> submenuActions;
    };

    void addEntries(QMenu& target, std::span<const MenuEntry>& entries, Menu& owner);
    void refreshItem(Item& item, const input::ActionContext& context);
    static void tidySeparators(QMenu& menu);

    QMenuBar& m_bar;
    input::ActionRegistry& m_registry;
    input::KeymapService& m_keymap;
    ContextProvider m_context;
    Invoker m_invoke;
    std::vector<Menu> m_menus;
    std::map<std::string, CheckedProvider, std::less<>> m_checked;
    std::uint64_t m_registryGeneration = UINT64_MAX;
    std::uint64_t m_bindingsGeneration = UINT64_MAX;
};

} // namespace frost::ui
