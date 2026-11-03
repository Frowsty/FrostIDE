// SPDX-License-Identifier: Apache-2.0
//
// LanguageController: the Rider-like language features of the active editor (ADR 0041), wired
// from frost::lang's models to the widgets of lang_widgets.hpp through IEditorLanguageHost, and
// registered as frost::input actions under the IntelliJ keymap ids.
//
/*
 * Actions (id = default IntelliJ shortcut): code.completion = Ctrl+Space, code.parameterInfo =
 * Ctrl+P, code.quickDocumentation = Ctrl+Q, navigate.declaration = Ctrl+B (also Ctrl+click via
 * gotoDeclarationAt), navigate.implementation = Ctrl+Alt+B, navigate.relatedFiles =
 * Ctrl+Alt+Home (clangd's switchSourceHeader), navigate.findUsages = Alt+F7,
 * navigate.highlightUsages = Ctrl+Shift+F7, navigate.fileStructure = Ctrl+F12,
 * navigate.gotoSymbol = Ctrl+Alt+Shift+N, refactor.rename = Shift+F6, code.showIntentions =
 * Alt+Enter (clangd fix-its) and code.reformat = Ctrl+Alt+L. Every one but go to symbol needs
 * EditorFocus.
 *
 * Typing (textTyped) opens completion on an identifier start, '.', "->" and "::" (and
 * include-path characters), refilters an open lookup locally, re-requests only when the server
 * said the list is incomplete, and opens or updates parameter info on '(' and ','. Caret moves
 * (caretMoved) close a lookup the caret left and schedule highlight usages after a debounce.
 *
 * Threads: UI thread only. Requests run as coroutines on the scheduler (Lane::Interactive); each
 * feature has a LatestWins slot, so a newer request cancels the older one, and results are
 * posted back to the UI thread and dropped when stale or after the controller is gone. The
 * server, scheduler and file system must outlive every request started (the workbench stops the
 * scheduler before destroying the server); host and applier must outlive the controller.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/input/action.hpp"
#include "frost/lang/completion.hpp"
#include "frost/lang/diagnostics.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/refactoring.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/sched/latest_wins.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"
#include "frost/text/positions.hpp"
#include "frost/ui/lang/editor_language_host.hpp"
#include "frost/ui/lang/lang_widgets.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
}

namespace frost::ui::lang {

// What the controller talks to (see the lifetime rules in the header comment).
struct LanguageServices {
    ::frost::lang::ILspRequester& server; // ClangdService.
    sched::IScheduler& scheduler;
    IEditorLanguageHost& host;
    ::frost::lang::IWorkspaceEditApplier& applier; // Rename and quick fixes go through it.
    io::IFileSystem& fs;                           // Line previews of files that are not open.
    // The live diagnostics of a file (ClangdService::diagnostics()), for quick fixes.
    std::function<std::vector<::frost::lang::LiveDiagnostic>(std::string_view path)> diagnostics;
};

class LanguageController final : public QObject {
    Q_OBJECT

public:
    explicit LanguageController(LanguageServices services, QObject* parent = nullptr);
    ~LanguageController() override;
    LanguageController(const LanguageController&) = delete ("owns popups and request slots");
    LanguageController& operator=(const LanguageController&) = delete ("owns popups and request slots");

    // Registers the actions of the header comment. Errors: the registry's (AlreadyExists when
    // another slice registered one of the ids).
    [[nodiscard]] Result<void> registerActions(input::ActionRegistry& registry);

    // --- The features (the action handlers) ---
    void completeCode();
    void showParameterInfo();
    void showQuickDocumentation();
    void gotoDeclaration();
    // Ctrl+click: go to declaration of the symbol at `offset`.
    void gotoDeclarationAt(ByteOffset offset);
    void gotoImplementation();
    void gotoRelatedFile();
    void findUsages();
    void highlightUsages();
    void showFileStructure();
    void gotoSymbol();
    void rename();
    void showIntentions();
    void reformat();

    // --- Editor hooks ---
    // Cancel requests and dismiss document-specific UI when the active editor changes.
    void editorChanged();
    // Invalidate locations immediately on every active-document edit or server replacement.
    void structureChanged();
    void setStructureVisible(bool visible);
    [[nodiscard]] StructurePanel& structurePanel();
    // A key press in the editor; true when the language features consumed it (lookup
    // navigation, commit, Esc, snippet Tab). Call before the editor handles the key.
    bool keyPress(int key, Qt::KeyboardModifiers modifiers);
    // The user typed `text`, which is already in the document (the caret is after it). Not for
    // edits the controller applied itself (commits, rename, reformat).
    void textTyped(std::string_view text);
    // The caret moved (typing, clicks, navigation).
    void caretMoved();
    // The delay before highlight usages follows the caret (default 300 ms).
    void setHighlightDelay(std::chrono::milliseconds delay);

    // --- The widgets (owned; the workbench docks the usages panel) ---
    [[nodiscard]] CompletionPopup& completionPopup();
    [[nodiscard]] ParameterInfoPopup& parameterInfoPopup();
    [[nodiscard]] QuickDocPopup& quickDocPopup();
    [[nodiscard]] FindUsagesPanel& usagesPanel();
    [[nodiscard]] SymbolPopup& symbolPopup();
    [[nodiscard]] RenameInput& renameInput();
    [[nodiscard]] ChoicePopup& choicePopup();

signals:
    // A message for the status bar ("No implementations found", a rename refusal, ...).
    void statusMessage(QString message);
    // The find-usages panel has new results: show its tool window.
    void usagesReady();
    // A request's result reached the UI thread (tests wait on it).
    void requestFinished();

private:
    // Runs `make(token)` on the scheduler and `done(result)` on the UI thread when `slot` still
    // holds the ticket and the controller is alive.
    template<class T>
    void start(sched::LatestWins& slot, std::function<sched::Task<Result<T>>(std::stop_token)> make,
               std::function<void(Result<T>)> done);

    [[nodiscard]] bool hasDocument() const;
    [[nodiscard]] ::frost::lang::LspPosition positionAt(ByteOffset offset) const;
    [[nodiscard]] ::frost::lang::LspPosition caretPosition() const;
    [[nodiscard]] std::string lineBeforeCaret() const;
    void report(const Error& error, std::string_view what);
    void openTargets(std::vector<::frost::lang::Location> locations, const QString& title, const QString& none);
    void requestCompletion(::frost::lang::CompletionTrigger trigger);
    void updateLookupPrefix();
    void closeLookup();
    void commitCompletion(std::size_t itemIndex, bool replaceWord);
    void resolveSelected(std::size_t itemIndex);
    void requestSignatureHelp(std::optional<char> trigger);
    void requestWorkspaceSymbols(const QString& query);
    void applyWorkspaceEdit(const ::frost::lang::WorkspaceEdit& edit, const QString& label);
    void findUsagesAt(std::string path, ::frost::lang::LspPosition position);
    void gotoDeclarationAtPosition(::frost::lang::LspPosition position);
    void requestStructure();
    bool nextTabStop();

    LanguageServices m_services;
    std::unique_ptr<CompletionPopup> m_completion;
    std::unique_ptr<ParameterInfoPopup> m_parameterInfo;
    std::unique_ptr<QuickDocPopup> m_quickDoc;
    std::unique_ptr<FindUsagesPanel> m_usages;
    std::unique_ptr<SymbolPopup> m_symbols;
    std::unique_ptr<StructurePanel> m_structure;
    std::unique_ptr<RenameInput> m_rename;
    std::unique_ptr<ChoicePopup> m_choice;
    QTimer m_highlightTimer;
    QTimer m_symbolQueryTimer;
    QTimer m_structureTimer;
    sched::LatestWins m_structureSlot;
    std::uint64_t m_structureGeneration = 0;
    std::string m_structurePath;
    std::string m_structureIdentity;
    std::int64_t m_structureVersion = 0;
    std::uint64_t m_structureRevision = 0;
    ::frost::lang::PositionEncoding m_structureEncoding = ::frost::lang::PositionEncoding::Utf16;
    bool m_structureVisible = false;

    sched::LatestWins m_completionSlot;
    sched::LatestWins m_resolveSlot;
    sched::LatestWins m_signatureSlot;
    sched::LatestWins m_hoverSlot;
    sched::LatestWins m_navigationSlot;
    sched::LatestWins m_usagesSlot;
    sched::LatestWins m_highlightSlot;
    sched::LatestWins m_symbolSlot;
    sched::LatestWins m_renameSlot;
    sched::LatestWins m_actionSlot;
    sched::LatestWins m_formatSlot;

    // The open lookup: the document and where its identifier starts.
    std::string m_lookupPath;
    std::optional<ByteOffset> m_lookupWordStart;
    // What the choice popup's rows do when chosen.
    std::vector<std::function<void()>> m_choiceActions;
    // Snippet tab stops still to visit after a commit, and the document size they assume.
    std::vector<text::ByteRange> m_tabStops;
    std::uint64_t m_tabStopsDocumentSize = 0;
    // The rename in progress.
    std::string m_renamePath;
    ::frost::lang::LspPosition m_renamePosition;
    // The document and version a reformat was requested for.
    std::int64_t m_formatVersion = 0;
};

} // namespace frost::ui::lang
