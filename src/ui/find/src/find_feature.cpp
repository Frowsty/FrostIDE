// SPDX-License-Identifier: Apache-2.0
//
// FindFeature (frost/ui/find/find_feature.hpp): the find actions, the Find tool window and the
// Replace in Files summary.

#include "frost/ui/find/find_feature.hpp"

#include "frost/core/assert.hpp"
#include "frost/input/action.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/keymap_service.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/find/find_popup.hpp"
#include "frost/ui/find/find_tool_window.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/editor_host.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "find_util.hpp"

#include <algorithm>
#include <utility>

namespace frost::ui {

using find_detail::fromUtf8;
using find_detail::label;
using find_detail::toUtf8;

namespace {

// Like the shell's window actions: anywhere except in a popup or a modal dialog.
constexpr input::ContextRule kFindRule{{}, {input::ContextKey::PopupFocus, input::ContextKey::DialogFocus}};

// The most skipped files the summary lists.
constexpr std::size_t kListedSkips = 10;

// `text` with RE2's metacharacters escaped, so a selection is searched literally in regex mode.
std::string escapeRegex(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (std::string_view("\\^$.|?*+()[]{}").find(c) != std::string_view::npos) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

QString reasonText(search::ReplaceSkipReason reason) { return label(search::toString(reason)); }

} // namespace

FindFeature::FindFeature(ShellWindow& window, FindFeatureServices services)
    : QObject(&window), m_window(window), m_services(std::move(services)) {
    FROST_PRE(m_services.scheduler != nullptr && m_services.documents != nullptr);
}

FindFeature::~FindFeature() { cancelSearches(); }

FindPopup* FindFeature::popup() const noexcept { return m_popup.data(); }
FindToolWindow* FindFeature::toolWindow() const noexcept { return m_toolWindow.data(); }

void FindFeature::cancelSearches() {
    if (m_popup) {
        m_last = m_popup->settings();
        m_popup->closePopup(Popup::CloseReason::Cancelled);
        delete m_popup.data();
    }
    if (m_toolWindow)
        m_toolWindow->setSession(nullptr);
}

FindSessionServices FindFeature::sessionServices() const {
    return FindSessionServices{m_services.scheduler, m_services.documents, m_services.solution, m_services.limits};
}

Result<void> FindFeature::install() {
    if (m_installed) {
        return {};
    }
    m_installed = true;
    const ShellEnvironment& env = m_window.environment();
    const QPointer<FindFeature> self(this);

    const auto add = [&](std::string_view id, std::string_view name, std::string_view group, std::function<void()> run,
                         std::function<bool()> enabled) -> Result<void> {
        input::ActionSpec action;
        action.id = std::string(id);
        action.name = std::string(name);
        action.group = std::string(group);
        action.context = kFindRule;
        action.handler = [self, run = std::move(run)](const input::ActionContext&, const input::InvokeInfo&) {
            if (self != nullptr) {
                run();
            }
        };
        if (enabled) {
            action.enabled = [self, enabled = std::move(enabled)](const input::ActionContext&) {
                return self != nullptr && enabled();
            };
        }
        if (Result<input::ActionId> registered = env.registry.add(std::move(action)); !registered) {
            return std::unexpected(std::move(registered.error()));
        }
        return {};
    };
    const auto hasOccurrences = [this] { return m_toolWindow != nullptr && m_toolWindow->hasOccurrences(); };
    Result<void> result = add(find_actions::kFindInFiles, "Find in Files...", "Find",
                              [this] { static_cast<void>(showPopup(false)); }, {});
    if (result) {
        result = add(find_actions::kReplaceInFiles, "Replace in Files...", "Find",
                     [this] { static_cast<void>(showPopup(true)); }, {});
    }
    if (result) {
        result = add(
            find_actions::kNextOccurrence, "Next Occurrence", "Find", [this] { static_cast<void>(stepOccurrence(1)); },
            hasOccurrences);
    }
    if (result) {
        result = add(
            find_actions::kPreviousOccurrence, "Previous Occurrence", "Find",
            [this] { static_cast<void>(stepOccurrence(-1)); }, hasOccurrences);
    }
    if (result) {
        result = add(find_actions::kFindWindow, "Find", "Tool Windows",
                     [this] { m_window.toolWindows().activate(find_actions::kFindToolWindow); }, {});
    }
    // The tool window after its action, so its stripe tooltip shows the action's shortcut.
    ToolWindowSpec spec;
    spec.id = std::string(find_actions::kFindToolWindow);
    spec.title = label("Find");
    spec.side = ToolWindowSide::Bottom;
    spec.iconId = std::string(icons::kActionsSearch);
    if (const std::optional<input::ActionId> id = env.registry.find(find_actions::kFindWindow); id) {
        if (const std::optional<input::Sequence> shortcut = env.keymap.primaryShortcut(*id); shortcut) {
            spec.shortcutText = fromUtf8(input::toDisplayText(*shortcut, env.keymap.layout(), env.keymap.platform()));
        }
    }
    spec.createContent = [self] {
        auto* window = new FindToolWindow();
        if (self != nullptr) {
            connect(window, &FindToolWindow::locationActivated, self.data(), &FindFeature::openLocation);
            self->m_toolWindow = window;
        }
        return window;
    };
    if (Result<void> added = m_window.toolWindows().add(std::move(spec)); !added && result) {
        result = std::move(added);
    }

    m_window.refreshActions();
    return result;
}

FindSettings FindFeature::initialSettings(bool replace) const {
    FindSettings settings = m_last.value_or(FindSettings{});
    settings.replaceMode = replace;
    settings.scope.currentFile = m_services.currentFile ? m_services.currentFile() : std::string();
    const bool solutionOpen = m_services.solution && m_services.solution() != nullptr;
    settings.scope.ignoreRoot = m_services.defaultDirectory ? m_services.defaultDirectory() : std::string();
    if (!settings.directoryExplicit) {
        settings.scope.directory = settings.scope.ignoreRoot;
    }
    settings.scope.roles = m_services.directoryRoles ? m_services.directoryRoles() : nullptr;
    if (settings.scopeExplicit && settings.preferredScope) {
        settings.scope.kind = *settings.preferredScope;
    }
    if (!settings.scopeExplicit) {
        // Solution by default with a solution; otherwise the folder, or the open files.
        settings.scope.kind = solutionOpen                        ? search::ScopeKind::Solution
                              : !settings.scope.directory.empty() ? search::ScopeKind::Directory
                                                                  : search::ScopeKind::OpenFiles;
    }
    if (m_services.selectedText) {
        const QString selected = m_services.selectedText();
        if (!selected.isEmpty() && !selected.contains(QLatin1Char('\n')) &&
            !selected.contains(QChar::ParagraphSeparator)) {
            const std::string text = toUtf8(selected);
            settings.query.pattern = settings.query.options.regex ? escapeRegex(text) : text;
        }
    }
    return settings;
}

FindPopup* FindFeature::showPopup(bool replace) {
    if (m_popup != nullptr) {
        m_popup->closePopup(Popup::CloseReason::Cancelled);
        // A closed popup is only scheduled for deletion; join its session before replacing it.
        delete m_popup.data();
    }
    const bool solutionOpen = m_services.solution && m_services.solution() != nullptr;
    auto* popup = new FindPopup(&m_window, sessionServices(), initialSettings(replace), solutionOpen);
    m_popup = popup;
    connect(popup, &FindPopup::locationActivated, this, &FindFeature::openLocation);
    connect(popup, &FindPopup::openInFindWindowRequested, this, [this, popup] { openInFindWindow(*popup); });
    connect(popup, &FindPopup::replaceRequested, this,
            [this, popup](bool all) { static_cast<void>(this->replace(*popup, all)); });
    connect(popup, &Popup::closed, this, [this, popup] { m_last = popup->settings(); });
    popup->showCentered(&m_window, 0.12);
    return popup;
}

FindToolWindow* FindFeature::openInFindWindow(FindPopup& popup) {
    m_last = popup.settings();
    if (!popup.resultsCurrent())
        popup.searchNow();
    FindSession* session = popup.takeSession();
    popup.closePopup(Popup::CloseReason::Accepted);
    m_window.toolWindows().show(find_actions::kFindToolWindow, true);
    if (m_toolWindow == nullptr) {
        // The manager has not created the content (the window is not registered): keep nothing.
        delete session;
        return nullptr;
    }
    m_toolWindow->setSession(session);
    return m_toolWindow.data();
}

Result<search::ReplaceSummary> FindFeature::replace(FindPopup& popup, bool all) {
    FindSession* session = popup.session();
    if (!popup.resultsCurrent()) {
        return fail(ErrorCode::Busy, "search criteria changed; wait for the new results");
    }
    if (session == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the popup has no results to replace");
    }
    std::optional<std::pair<std::size_t, std::size_t>> only;
    if (!all) {
        only = popup.currentMatch();
        if (!only) {
            return fail(ErrorCode::InvalidArgument, "no match is selected");
        }
    }
    session->wait();
    search::ReplaceRequest request = session->replaceRequest(only);
    request.replacement = popup.settings().replacement;
    // TODO(M2): run large replaces on a background lane with progress (closed files only; open
    // documents stay on the GUI thread).
    Result<search::ReplaceSummary> summary =
        search::replaceInFiles(*m_services.documents, request, m_services.documentEdited);
    NotificationCenter& notifications = m_window.notifications();
    if (!summary) {
        notifications.notify(Notification{label("Replace in Files failed"),
                                          fromUtf8(summary.error().message()),
                                          NotificationKind::Error,
                                          std::nullopt,
                                          {}});
        return summary;
    }
    notifications.notify(Notification{label("Replace in Files"),
                                      summaryText(*summary),
                                      summary->skipped.empty() && summary->warnings.empty() ? NotificationKind::Info
                                                                                            : NotificationKind::Warning,
                                      std::nullopt,
                                      {}});
    for (const Error& warning : summary->warnings) {
        notifications.notify(Notification{label("Replacement saved with a warning"),
                                          fromUtf8(warning.describe()),
                                          NotificationKind::Warning,
                                          std::nullopt,
                                          {}});
    }
    m_last = popup.settings();
    if (all) {
        popup.closePopup(Popup::CloseReason::Accepted);
    } else {
        popup.searchNow();
    }
    return summary;
}

bool FindFeature::stepOccurrence(int delta) { return m_toolWindow != nullptr && m_toolWindow->stepOccurrence(delta); }

QString FindFeature::summaryText(const search::ReplaceSummary& summary) {
    QString text = label("%1 replacements in %2 files, %3 skipped")
                       .arg(summary.replacements)
                       .arg(summary.replaced.size())
                       .arg(summary.skipped.size());
    const std::size_t listed = std::min(summary.skipped.size(), kListedSkips);
    for (std::size_t i = 0; i < listed; ++i) {
        const search::SkippedReplaceFile& skipped = summary.skipped[i];
        text += QStringLiteral("\n%1: %2").arg(fromUtf8(io::fileName(skipped.path)), reasonText(skipped.reason));
    }
    if (summary.skipped.size() > listed) {
        text += QLatin1Char('\n') + label("and %1 more").arg(summary.skipped.size() - listed);
    }
    return text;
}

void FindFeature::openLocation(const FindLocation& location) {
    if (m_services.openLocation) {
        m_services.openLocation(location);
        return;
    }
    // Until the composition root wires the editor: open the file in a tab and go to the line.
    if (Result<int> opened = m_window.openFile(location.path); opened) {
        if (IEditorView* editor = m_window.tabs().currentEditor(); editor != nullptr) {
            editor->goToLine(static_cast<int>(location.line) + 1, static_cast<int>(location.column) + 1);
        }
    }
}

} // namespace frost::ui
