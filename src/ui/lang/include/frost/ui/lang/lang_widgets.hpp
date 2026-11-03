// SPDX-License-Identifier: Apache-2.0
//
// The language-feature widgets (ADR 0041), standalone so the workbench and tests can drive them
// without an editor: the completion popup (kind badges, detail, a documentation side panel),
// the parameter-info and quick-documentation popups, the find-usages panel, the symbol popup
// (file structure tree or go-to-symbol list, both with speed search), the rename field and the
// choice list behind quick fixes and multi-target navigation. All are UI-thread only.
//
/*
 * Focus model, as in Rider: the completion, parameter-info and quick-doc popups never take focus
 * (the editor keeps typing; LanguageController forwards Up/Down/Enter/Tab/Esc to the lookup with
 * handleKey()). The symbol, rename and choice popups are Qt::Popup windows that take focus,
 * close on Esc or a click outside, and return focus to the widget that had it.
 */
#pragma once

#include "frost/lang/code_assist.hpp"
#include "frost/lang/completion.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/symbols.hpp"

#include <QFrame>
#include <QIcon>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <string>
#include <vector>

class QKeyEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace frost::ui::lang {

// A round badge with the kind's letter in a colour of its kind group ("C" class, "f" function,
// "m" method, "v" variable, ...), sized for list rows. The icon atlas has no symbol-kind icons
// yet (ADR 0041 defers them).
[[nodiscard]] QIcon kindIcon(::frost::lang::CompletionItemKind kind);
[[nodiscard]] QIcon kindIcon(::frost::lang::SymbolKind kind);

// LSP markup as Qt rich text: Markdown through QTextDocument's reader, plain text escaped.
[[nodiscard]] QString markupToHtml(const ::frost::lang::Markup& markup);

/*
 * The non-activating popup base: a frameless tool window that shows below `anchor` (a screen
 * rect, normally the caret's line box), or above it when there is no room below.
 */
class HintPopup : public QFrame {
    Q_OBJECT

public:
    explicit HintPopup(QWidget* parent = nullptr);
    // Shows (or moves) the popup next to `anchor`; `above` prefers the space above it.
    void showAt(const QRect& anchor, bool above = false);
};

/*
 * The completion lookup over a CompletionModel it owns. setList()/setPrefix() refilter; the
 * rows follow the model's order and selection. handleKey() takes Up, Down, PageUp, PageDown,
 * Enter (commit), Tab (commit replacing the rest of the word) and Esc (cancel) while the popup
 * is visible, and returns false for every other key so the editor handles it.
 */
class CompletionPopup final : public HintPopup {
    Q_OBJECT

public:
    explicit CompletionPopup(QWidget* parent = nullptr);

    [[nodiscard]] ::frost::lang::CompletionModel& model() noexcept { return m_model; }
    void setList(::frost::lang::CompletionList list);
    void setPrefix(std::string_view prefix);
    // Replaces item `index` (resolved) and refreshes its row and the documentation panel.
    void replaceItem(std::size_t index, ::frost::lang::CompletionItem item);
    // Rebuilds the rows from the model (after model() was changed directly).
    void refresh();
    [[nodiscard]] int rowCount() const;
    // The documentation panel's text (empty when hidden), for tests.
    [[nodiscard]] QString documentationText() const;

    // See the class comment.
    bool handleKey(int key, Qt::KeyboardModifiers modifiers);

signals:
    // The user committed the item at `itemIndex` (into model().items()); `replaceWord` for Tab.
    void commitRequested(std::size_t itemIndex, bool replaceWord);
    void cancelled();
    // The selection moved to item `itemIndex`; the controller resolves its documentation.
    void selectionChanged(std::size_t itemIndex);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void syncSelection();
    void updateDocumentation();

    ::frost::lang::CompletionModel m_model;
    QListWidget* m_list = nullptr;
    QTextBrowser* m_documentation = nullptr;
};

// Signature help: every signature on its own line, the active one's active parameter in bold.
class ParameterInfoPopup final : public HintPopup {
    Q_OBJECT

public:
    explicit ParameterInfoPopup(QWidget* parent = nullptr);
    void setHelp(const ::frost::lang::SignatureHelp& help);
    // The rich text shown, for tests.
    [[nodiscard]] QString html() const;

private:
    QLabel* m_label = nullptr;
};

// Quick documentation (hover): Markdown rendered as rich text, closed with Esc by the controller.
class QuickDocPopup final : public HintPopup {
    Q_OBJECT

public:
    explicit QuickDocPopup(QWidget* parent = nullptr);
    void setMarkup(const ::frost::lang::Markup& markup);
    [[nodiscard]] QString plainText() const;

private:
    QTextBrowser* m_browser = nullptr;
};

/*
 * The find-usages results: one node per file ("main.cpp  2 usages" with the path as tooltip)
 * and one row per usage ("12  int total = value();"). Double-click or Enter on a usage emits
 * openRequested.
 */
class FindUsagesPanel final : public QWidget {
    Q_OBJECT

public:
    explicit FindUsagesPanel(QWidget* parent = nullptr);
    void setUsages(const QString& title, std::vector<::frost::lang::UsageGroup> groups);
    [[nodiscard]] QString title() const;
    [[nodiscard]] QTreeWidget* tree() const noexcept { return m_tree; }
    [[nodiscard]] std::size_t usageCount() const noexcept { return m_count; }
    // Opens the usage at `index` of `group` (the same as double-clicking it).
    void activate(std::size_t group, std::size_t index);

signals:
    void openRequested(QString path, ::frost::lang::LspPosition position);

private:
    void onActivated(QTreeWidgetItem* item);

    QLabel* m_title = nullptr;
    QTreeWidget* m_tree = nullptr;
    std::vector<::frost::lang::UsageGroup> m_groups;
    std::size_t m_count = 0;
};

/*
 * A focus-taking popup with a speed-search field above a tree. Structure mode shows a
 * document's symbol tree, filtered locally (filterSymbols) as the user types; list mode shows
 * workspace symbols the controller fetches for each query (queryChanged). Enter or a
 * double-click emits activated with the target; Esc closes.
 */
class StructurePanel final : public QWidget {
    Q_OBJECT
public:
    explicit StructurePanel(QWidget* parent = nullptr);
    void setStatus(const QString& status);
    void setSymbols(std::string path, std::vector<::frost::lang::DocumentSymbol> symbols);
signals:
    void activated(QString path, ::frost::lang::LspPosition position);

private:
    void rebuild();
    QLabel* m_status = nullptr;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_tree = nullptr;
    std::string m_path;
    std::vector<::frost::lang::DocumentSymbol> m_symbols;
};

class SymbolPopup final : public QFrame {
    Q_OBJECT

public:
    explicit SymbolPopup(QWidget* parent = nullptr);

    // File structure of `path`.
    void setStructure(QString title, std::string path, std::vector<::frost::lang::DocumentSymbol> symbols);
    // Go to symbol: the results for the current query.
    void setWorkspaceSymbols(QString title, std::vector<::frost::lang::WorkspaceSymbol> symbols);
    // Shows centred over `over` (the editor) and focuses the search field.
    void popup(QWidget* over);
    [[nodiscard]] QString query() const;
    void setQuery(const QString& query);
    // True while it shows a file structure, false for go to symbol.
    [[nodiscard]] bool structureMode() const noexcept { return m_structureMode; }
    [[nodiscard]] int visibleRowCount() const;
    // Activates the current row (Enter).
    void activateCurrent();

signals:
    void activated(QString path, ::frost::lang::LspPosition position);
    void queryChanged(QString query);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuild();

    bool m_structureMode = true;
    std::string m_path;
    std::vector<::frost::lang::DocumentSymbol> m_symbols;
    std::vector<::frost::lang::WorkspaceSymbol> m_workspaceSymbols;
    QLabel* m_title = nullptr;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_tree = nullptr;
};

// The rename field: the current name selected; Enter accepts, Esc cancels.
class RenameInput final : public QFrame {
    Q_OBJECT

public:
    explicit RenameInput(QWidget* parent = nullptr);
    void start(const QString& currentName, const QRect& anchor);
    [[nodiscard]] QLineEdit* field() const noexcept { return m_field; }

signals:
    void accepted(QString newName);
    void cancelled();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QLineEdit* m_field = nullptr;
    bool m_done = false;
};

// A short list to choose from (quick fixes, several declarations): Enter or a click chooses.
class ChoicePopup final : public QFrame {
    Q_OBJECT

public:
    explicit ChoicePopup(QWidget* parent = nullptr);
    // Shows `choices` below `anchor`; `preferred` rows get a marker.
    void start(const QString& title, const std::vector<QString>& choices, const QRect& anchor,
               std::vector<bool> preferred = {});
    [[nodiscard]] QListWidget* list() const noexcept { return m_list; }

signals:
    void chosen(std::size_t index);
    void cancelled();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QLabel* m_title = nullptr;
    QListWidget* m_list = nullptr;
    bool m_done = false;
};

} // namespace frost::ui::lang
