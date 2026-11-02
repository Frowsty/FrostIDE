// SPDX-License-Identifier: Apache-2.0
//
// FindPopup (ADR 0045): the Find in Files (Ctrl+Shift+F) and Replace in Files (Ctrl+Shift+R)
// popup, laid out as Rider's Find in Path: the query field with Match Case, Words and Regex
// toggles, the replacement field in replace mode, the file mask, the scope buttons (In Solution,
// In Project, Directory, Open Files), the live result list with the matches highlighted (check
// boxes in replace mode: an unchecked match is not replaced), the preview of the selected match
// with its context lines, and the Open in Find Window, Replace and Replace All buttons.
/*
 * Keys: typing searches again after a short pause; Up, Down, Page Up and Page Down in the query
 * field move the selection; Enter (or a double-click) opens the selected match through
 * locationActivated() and closes the popup; Ctrl+Enter asks for the Find tool window; Escape
 * closes. The popup owns its FindSession until takeSession() hands it over. GUI thread only.
 */
#pragma once

#include "frost/ui/find/find_session.hpp"
#include "frost/ui/shell/popup.hpp"

#include <QString>

#include <array>
#include <optional>
#include <utility>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QToolButton;

namespace frost::ui {

class FindPopup final : public Popup {
    Q_OBJECT

public:
    // How long typing pauses before the search runs again.
    static constexpr int kDebounceMs = 150;

    // `settings` fills the widgets (FindSettings::replaceMode picks the mode); `solutionOpen`
    // enables the Solution and Project scopes. The first search starts when the popup opens or
    // searchNow() runs.
    FindPopup(QWidget* owner, FindSessionServices services, const FindSettings& settings, bool solutionOpen);
    ~FindPopup() override;
    FindPopup(const FindPopup&) = delete ("QObject instances have identity and cannot be copied");
    FindPopup& operator=(const FindPopup&) = delete ("QObject instances have identity and cannot be copied");

    // The settings as the widgets show them.
    [[nodiscard]] FindSettings settings() const;
    [[nodiscard]] bool replaceMode() const noexcept { return m_replaceMode; }
    // Searches now with settings() (skipping the pause).
    void searchNow();
    // False immediately after search criteria change, until a search for them starts.
    [[nodiscard]] bool resultsCurrent() const noexcept { return !m_searchPending; }
    // The session; nullptr after takeSession().
    [[nodiscard]] FindSession* session() const noexcept { return m_session; }
    // Gives the session (and its results) to the caller, which becomes its parent's owner; the
    // popup shows no results afterwards and should be closed.
    [[nodiscard]] FindSession* takeSession();

    // The selected match as (file, match) indices of the session, or nullopt.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> currentMatch() const;
    // Opens the selected match (locationActivated) and closes the popup. False without one.
    bool activateCurrent();

    // The widgets, for the feature and tests.
    [[nodiscard]] QLineEdit* queryField() const noexcept { return m_query; }
    [[nodiscard]] QLineEdit* replacementField() const noexcept { return m_replacement; }
    [[nodiscard]] QLineEdit* fileMaskField() const noexcept { return m_mask; }
    [[nodiscard]] QCheckBox* fileMaskToggle() const noexcept { return m_maskEnabled; }
    [[nodiscard]] QLineEdit* directoryField() const noexcept { return m_directory; }
    [[nodiscard]] QToolButton* caseToggle() const noexcept { return m_case; }
    [[nodiscard]] QToolButton* wordsToggle() const noexcept { return m_words; }
    [[nodiscard]] QToolButton* regexToggle() const noexcept { return m_regex; }
    [[nodiscard]] QToolButton* scopeButton(search::ScopeKind kind) const noexcept;
    [[nodiscard]] QListWidget* resultList() const noexcept { return m_results; }
    [[nodiscard]] QPlainTextEdit* preview() const noexcept { return m_preview; }
    [[nodiscard]] QLabel* previewTitle() const noexcept { return m_previewTitle; }
    [[nodiscard]] QLabel* statusLabel() const noexcept { return m_status; }
    [[nodiscard]] QPushButton* openInWindowButton() const noexcept { return m_openInWindow; }
    [[nodiscard]] QPushButton* replaceButton() const noexcept { return m_replaceOne; }
    [[nodiscard]] QPushButton* replaceAllButton() const noexcept { return m_replaceAll; }

signals:
    // Enter or a double-click on a match.
    void locationActivated(const frost::ui::FindLocation& location);
    // Ctrl+Enter or the Open in Find Window button.
    void openInFindWindowRequested();
    // Replace (only the selected match) or Replace All (every checked match).
    void replaceRequested(bool all);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildUi(const FindSettings& settings, bool solutionOpen);
    void connectSession();
    void scheduleSearch();
    void onCleared();
    void onFilesAdded(int first, int count);
    void onFinished();
    void onItemChanged(QListWidgetItem* item);
    void updatePreview();
    void updateButtons();
    void moveSelection(int delta);

    FindSessionServices m_services;
    // The settings the popup opened with: settings() keeps their fields no widget shows (the
    // current file, the directory excludes, multiline).
    FindSettings m_base;
    FindSession* m_session = nullptr;
    bool m_replaceMode = false;
    bool m_searchedOnce = false;
    bool m_searchPending = true;
    bool m_filling = false;
    QTimer* m_debounce = nullptr;
    QLabel* m_title = nullptr;
    QLineEdit* m_query = nullptr;
    QLineEdit* m_replacement = nullptr;
    QToolButton* m_case = nullptr;
    QToolButton* m_words = nullptr;
    QToolButton* m_regex = nullptr;
    QCheckBox* m_maskEnabled = nullptr;
    QLineEdit* m_mask = nullptr;
    std::array<QToolButton*, 4> m_scopes{};
    QLineEdit* m_directory = nullptr;
    QListWidget* m_results = nullptr;
    QLabel* m_previewTitle = nullptr;
    QPlainTextEdit* m_preview = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_openInWindow = nullptr;
    QPushButton* m_replaceOne = nullptr;
    QPushButton* m_replaceAll = nullptr;
};

} // namespace frost::ui
