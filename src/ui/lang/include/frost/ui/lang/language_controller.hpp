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
#include "frost/ui/lang/language_capabilities.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <chrono>
#include <cstdint>
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

// Why a language server may have no answer for a document (navigation messages, ADR 0078).
enum class ServerReadiness : std::uint8_t {
    Unknown,          // Ready as far as the client knows.
    NotRunning,       // No server for the document.
    NoCompileCommand, // clangd parses the file with fallback flags.
    Indexing,         // The background index is still being built.
};

// What the controller talks to (see the lifetime rules in the header comment).
struct LanguageServices {
    ::frost::lang::ILspRequester& server; // ClangdService.
    sched::IScheduler& scheduler;
    IEditorLanguageHost& host;
    ::frost::lang::IWorkspaceEditApplier& applier; // Rename and quick fixes go through it.
    io::IFileSystem& fs;                           // Line previews of files that are not open.
    // The live diagnostics of a file (ClangdService::diagnostics()), for quick fixes.
    std::function<std::vector<::frost::lang::LiveDiagnostic>(std::string_view path)> diagnostics;
    // Known project/workspace files for header/source switching when LSP cannot resolve it.
    std::function<std::vector<std::string>()> projectFiles{};
    /*
     * Where real metadata-as-source from the language server (csharp/metadata) is written to be
     * shown: absolute, <profile>/caches/metadata, never a workspace directory. Empty: such
     * sources are not shown and navigation reports "source not available" (LTR-6).
     */
    std::string metadataCacheDir{};
    // Why the server may have answered nothing for `path`, for navigation messages. Unset: only a
    // server that is not running is told apart (ServerReadiness::Unknown otherwise).
    std::function<ServerReadiness(std::string_view path)> readiness{};
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
    void smartCompleteCode();
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
    void gotoClass();
    void gotoSymbol();
    void rename();
    void showIntentions();
    void reformat();
    void reformatFile();

    /*
     * Refactorings and code generation (FND-3, ADR 0078: every edit comes from the language server).
     * Refactor This lists Rename, then the refactor actions and tweaks the server offers at the caret
     * (clangd_actions.hpp order). For C++, Extract Variable and Extract Method run clangd's
     * ExtractVariable and ExtractFunction tweaks, Generate lists clangd's MemberwiseConstructor,
     * SpecialMembers and OverridePureVirtuals, Generate Constructor and Implement Methods run the
     * first and the last, and Optimize Includes applies clangd's include-cleaner fix (C#: the
     * server's source.organizeImports action). Commands without a correct engine (Extract
     * Constant/Field/Parameter: REF-7; Inline, Change Signature: REF-6; Move, Safe Delete: REF-8;
     * Override Methods, getters/setters, equality and formatting members: REF-9; and every clangd
     * command outside C++) stay registered but disabled, with the epic in their description, and
     * report it on the status bar when called. Postfix templates, live templates, Surround With
     * and Complete Current Statement run only where language_capabilities.hpp allows them.
     */
    // --- Refactorings ---
    void refactorThis();
    void extractVariable();
    void extractConstant();
    void extractField();
    void extractParameter();
    void extractMethod();
    void inlineSymbol();
    void changeSignature();
    void safeDelete();
    void moveSymbol();
    void copySymbol();

    // --- Code generation and editing assistance ---
    void generateCode();
    void generateConstructor();
    void generateGettersSetters();
    void overrideMethods();
    void implementMethods();
    void generateEqualityMembers();
    void generateFormattingMembers();
    void surroundWith();
    void completeStatement();
    void optimizeImports();
    void insertLiveTemplate();

    // --- Navigation, Hierarchies & Peek (Milestone 2) ---
    void quickDefinition();
    void showUsagesPopup();
    void callHierarchy();
    void typeHierarchy();
    void gotoSuperMethod();
    void gotoTypeDeclaration();
    // Shows real metadata-as-source from the language server for the symbol at the caret, written
    // under metadataCacheDir; never writes into the workspace and never synthesises a source.
    void decompileExternalSymbol();

    // --- Editor hooks ---
    // Cancel requests and dismiss document-specific UI when the active editor changes.
    void editorChanged();
    /*
     * Invalidate locations immediately on every active-document edit or server replacement and request
     * the structure again. After an edit of the same document on the same server session the old tree
     * stays shown (inert: activating it does nothing) until the reply replaces it in place, so typing
     * does not flicker or lose collapsed nodes; otherwise the panel shows "Loading structure...".
     * GUI thread only.
     */
    void structureChanged();
    /*
     * Like structureChanged(), for an active document whose whole text was replaced (a reload from
     * disk): the old tree describes text that is gone, so it is always cleared at once ("Loading
     * structure...") rather than kept until the reply. GUI thread only.
     */
    void structureReplaced();
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
    [[nodiscard]] QuickDefinitionPopup& quickDefinitionPopup();
    [[nodiscard]] HierarchyPopup& hierarchyPopup();

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
    void executeServerCommand(std::string commandName, std::vector<::frost::lang::Json> arguments,
                              const QString& label);
    void initServiceHooks();
    void onRenameAccepted(const QString& name);
    void findUsagesAt(std::string path, ::frost::lang::LspPosition position);
    void gotoDeclarationAtPosition(::frost::lang::LspPosition position);
    void requestStructure();
    // structureChanged() and structureReplaced(): `keepSameIdentityTree` keeps the shown tree for the
    // same document and server session until the reply arrives.
    void invalidateStructure(bool keepSameIdentityTree);
    bool nextTabStop();
    bool tryExpandPostfixCompletion();
    void insertSnippetAt(text::ByteRange replaceRange, std::string_view rawSnippet, std::string_view undoLabel);
    // The capability row of the active document (language_capabilities.hpp).
    [[nodiscard]] const LanguageCapabilities& capabilities() const;
    // Runs clangd tweak `tweakId` (clangd_actions.hpp) at the caret or selection; `none` goes to
    // the status bar when clangd does not offer it there. C++ only (capabilities().clangdTweaks).
    void runClangdTweak(std::string_view tweakId, const QString& label, const QString& none);
    // A server code action: its edit through the applier, then its command on the server.
    void runCodeAction(::frost::lang::CodeActionItem action);
    // A command without a correct engine for the active document: `reason` names the owning epic.
    void unavailable(const QString& reason);
    // `none` plus why the server may have found nothing (ServerReadiness).
    [[nodiscard]] QString emptyResultMessage(const QString& none) const;
    // Opens real metadata-as-source the server returned (written under metadataCacheDir).
    void openMetadataLocation(const ::frost::lang::Location& location, const QString& symbol);
    void refreshCallHierarchy(bool outgoing);
    void refreshTypeHierarchy(bool subtypes);

    LanguageServices m_services;
    std::unique_ptr<CompletionPopup> m_completion;
    std::unique_ptr<ParameterInfoPopup> m_parameterInfo;
    std::unique_ptr<QuickDocPopup> m_quickDoc;
    std::unique_ptr<FindUsagesPanel> m_usages;
    std::unique_ptr<SymbolPopup> m_symbols;
    std::unique_ptr<StructurePanel> m_structure;
    std::unique_ptr<RenameInput> m_rename;
    std::unique_ptr<ChoicePopup> m_choice;
    std::unique_ptr<QuickDefinitionPopup> m_quickDef;
    std::unique_ptr<HierarchyPopup> m_hierarchy;
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
    sched::LatestWins m_hierarchySlot;

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
    std::string m_renameOldName;
    ::frost::lang::LspPosition m_renamePosition;
    // Active hierarchy root.
    std::string m_hierarchyPath;
    ::frost::lang::LspPosition m_hierarchyPosition{};
    bool m_hierarchyIsType = false;
    // The document and version a reformat was requested for.
    std::int64_t m_formatVersion = 0;
    bool m_symbolClassesOnly = false;
};

} // namespace frost::ui::lang
