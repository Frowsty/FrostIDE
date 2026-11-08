// SPDX-License-Identifier: Apache-2.0
//
// FindFeature (ADR 0045): Find and Replace in Files wired into the shell. install() registers the
// actions with their IntelliJ ids (search.findInFiles Ctrl+Shift+F, search.replaceInFiles
// Ctrl+Shift+R, search.nextOccurrence Ctrl+Alt+Down, search.previousOccurrence Ctrl+Alt+Up and
// window.find Alt+3; the keymap holds the shortcuts) and docks the Find tool window ("find", at
// the bottom). The actions open FindPopup with the editor's selection as the query, Open in Find
// Window moves its results to the tool window, and Replace runs Replace in Files and posts the
// summary ("N replacements in M files, K skipped").
/*
 * The composition root (W1) supplies the services: the scheduler searches run on, the SaveManager
 * (open documents and closed-file rewrites), the open solution, the active editor's file and
 * selection, and openLocation, which opens a match in the editor. documentEdited lets the editor
 * follow the edits Replace in Files makes to an open document (Editor::follow). Every pointer
 * outlives the feature; the feature is a child of the window. GUI thread only.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/search/find_in_files.hpp"
#include "frost/search/replace_in_files.hpp"
#include "frost/ui/find/find_session.hpp"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace frost::ui {

class FindPopup;
class FindToolWindow;
class ShellWindow;

namespace find_actions {

inline constexpr std::string_view kFindInFiles = "search.findInFiles";
inline constexpr std::string_view kReplaceInFiles = "search.replaceInFiles";
inline constexpr std::string_view kNextOccurrence = "search.nextOccurrence";
inline constexpr std::string_view kPreviousOccurrence = "search.previousOccurrence";
inline constexpr std::string_view kFindWindow = "window.find";
// The tool window's id.
inline constexpr std::string_view kFindToolWindow = "find";

} // namespace find_actions

struct FindFeatureServices {
    sched::IScheduler* scheduler = nullptr;
    document::SaveManager* documents = nullptr;
    // The open solution, or null.
    std::function<const project::SolutionModel*()> solution;
    // The active editor's file ("" without one): the Project scope's file.
    std::function<std::string()> currentFile;
    // The active editor's selection ("" without one): a single-line selection pre-fills the query.
    std::function<QString()> selectedText;
    // Where the Directory scope starts when the user has not picked a folder (the solution's or
    // the opened folder's directory).
    std::function<std::string()> defaultDirectory;
    // The open project's directory roles (UX-23): the Directory scope walks the project as they say.
    // Empty or null: the scope's name excludes only.
    std::function<std::shared_ptr<const project::DirectoryRoles>()> directoryRoles;
    // Opens a match in the editor. Empty: activating a match only closes the popup.
    std::function<void(const FindLocation& location)> openLocation;
    // Called after Replace in Files edited an open document (one undo step).
    search::DocumentEdited documentEdited;
    search::FindLimits limits{};
};

class FindFeature final : public QObject {
    Q_OBJECT

public:
    // Precondition: services.scheduler and services.documents are set.
    FindFeature(ShellWindow& window, FindFeatureServices services);
    ~FindFeature() override;
    FindFeature(const FindFeature&) = delete ("QObject instances have identity and cannot be copied");
    FindFeature& operator=(const FindFeature&) = delete ("QObject instances have identity and cannot be copied");

    // Cancels popup and tool-window sessions before a project switch or owner teardown.
    // GUI thread; waits for running searches to finish their cancellation.
    void cancelSearches();
    // Registers the actions and the tool window (once). Errors: the registry's or the tool window
    // manager's (AlreadyExists when another slice registered one of the ids); what registered
    // before the error stays registered.
    [[nodiscard]] Result<void> install();

    // Opens the Find (or with `replace`, the Replace) in Files popup, closing an open one.
    FindPopup* showPopup(bool replace);
    // The open popup, or nullptr.
    [[nodiscard]] FindPopup* popup() const noexcept;
    // The Find tool window's content, or nullptr before the window was first shown.
    [[nodiscard]] FindToolWindow* toolWindow() const noexcept;

    // Moves `popup`'s search (running or done) to the Find tool window, shows it and closes the
    // popup.
    FindToolWindow* openInFindWindow(FindPopup& popup);
    // Replaces the popup's checked matches (`all`) or its selected match, posts the summary and
    // then closes the popup (all) or searches again. Errors: replaceInFiles' (also posted).
    Result<search::ReplaceSummary> replace(FindPopup& popup, bool all);
    // The Find tool window's next or previous occurrence. False without results.
    bool stepOccurrence(int delta);

    // The settings the next popup opens with (the last popup's).
    [[nodiscard]] const std::optional<FindSettings>& lastSettings() const noexcept { return m_last; }
    // "N replacements in M files, K skipped", with one line per skipped file (at most ten).
    [[nodiscard]] static QString summaryText(const search::ReplaceSummary& summary);

private:
    [[nodiscard]] FindSessionServices sessionServices() const;
    [[nodiscard]] FindSettings initialSettings(bool replace) const;
    void openLocation(const FindLocation& location);

    ShellWindow& m_window;
    FindFeatureServices m_services;
    QPointer<FindPopup> m_popup;
    QPointer<FindToolWindow> m_toolWindow;
    std::optional<FindSettings> m_last;
    bool m_installed = false;
};

} // namespace frost::ui
