// SPDX-License-Identifier: Apache-2.0
//
// LanguageBridge (language_bridge.hpp): the language features' server, editor host and open
// documents over the Workbench's clangd and editor views; and Workbench::languageReadiness, the
// reason the language features give for an empty answer (ServerReadiness, R0 follow-up 4).

#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/symbols.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"

#include "language_bridge.hpp"
#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
#include <format>
#include <memory>
#include <utility>

namespace frost::app {

// What a compile_commands.json holds, as far as clangd's fallback is concerned
// (Workbench::clangdUsesFallbackFlags).
struct CompileDatabaseState {
    enum class Kind {
        Unreadable,  // The read failed: nothing is known.
        Unparseable, // Not a JSON array: clangd cannot load it either.
        Empty,       // "[]": clangd loads it and has no file to infer a command from.
        Entries,     // At least one entry: clangd infers a command for any file from them.
    };
    std::string database;  // The compile_commands.json that was read.
    io::FileStatus status; // Its size and modification time then; a change means a new read.
    Kind kind = Kind::Unreadable;
};

namespace {

// Where `offset` (before the batch) lands after the sorted, non-overlapping `edits`: shifted by the
// edits before it; inside a replaced range, at the end of the replacement.
ByteOffset mapThrough(ByteOffset offset, std::span<const lang::ByteEdit> edits) {
    std::int64_t delta = 0;
    for (const lang::ByteEdit& edit : edits) {
        const auto begin = static_cast<std::int64_t>(edit.range.begin.value());
        const auto end = static_cast<std::int64_t>(edit.range.end.value());
        const auto inserted = static_cast<std::int64_t>(edit.text.size());
        const auto at = static_cast<std::int64_t>(offset.value());
        if (end <= at && !(begin == end && begin == at)) {
            delta += inserted - (end - begin);
        } else if (begin < at && at < end) {
            return ByteOffset{static_cast<std::uint64_t>(begin + inserted + delta)};
        } else {
            break;
        }
    }
    return ByteOffset{static_cast<std::uint64_t>(static_cast<std::int64_t>(offset.value()) + delta)};
}

} // namespace

LanguageBridge::LanguageBridge(Workbench& workbench)
    : m_workbench(workbench), m_applier(static_cast<lang::IWorkspaceDocuments&>(*this), workbench.m_services.fs,
                                        lang::PositionEncoding::Utf16) {}

LanguageBridge::~LanguageBridge() = default;

lang::IWorkspaceEditApplier& LanguageBridge::applier() noexcept { return *this; }

Result<void> LanguageBridge::apply(const lang::WorkspaceEdit& edit, std::string_view label) {
    // Applying edits is on the GUI thread; encoding may change on a clangd restart.
    m_applier.setEncoding(encoding());
    return m_applier.apply(edit, label);
}

// --- ILspRequester ---

sched::Task<Result<lang::Json>> LanguageBridge::request(std::string method, lang::Json params, std::stop_token token) {
    return m_server.request(std::move(method), std::move(params), std::move(token));
}

lang::PositionEncoding LanguageBridge::encoding() const { return m_server.encoding(); }

std::optional<lang::ServerCapabilities> LanguageBridge::serverCapabilities() const {
    return m_server.serverCapabilities();
}

// --- IEditorLanguageHost ---

ui::EditorView* LanguageBridge::activeEditor() const {
    DocumentEditorView* view = m_workbench.currentView();
    return view != nullptr ? &view->editor() : nullptr;
}

std::string LanguageBridge::documentPath() const {
    const DocumentEditorView* view = m_workbench.currentView();
    return view != nullptr ? view->path() : std::string();
}

std::optional<text::Snapshot> LanguageBridge::snapshot() const {
    const ui::EditorView* editor = activeEditor();
    if (editor == nullptr) {
        return std::nullopt;
    }
    return editor->editor().text();
}

std::int64_t LanguageBridge::documentVersion() const {
    const lang::ClangdService* clangd = m_workbench.m_clangd.get();
    const std::string path = documentPath();
    if (isManagedSource(path))
        return m_workbench.m_managed ? m_workbench.m_managed->documentVersion(path).value_or(0) : 0;
    if (isLuaSource(path))
        return m_workbench.m_lua ? m_workbench.m_lua->documentVersion(path).value_or(0) : 0;
    if (clangd == nullptr || path.empty()) {
        return 0;
    }
    return clangd->documentVersion(path).value_or(0);
}

std::string LanguageBridge::structureIdentity() const {
    const auto* view = m_workbench.currentView();
    const std::string path = documentPath();
    const auto key = reinterpret_cast<std::uintptr_t>(view);
    if (isManagedSource(path) && m_workbench.m_managed)
        return std::format("{}:{}:{}:{}", key, reinterpret_cast<std::uintptr_t>(m_workbench.m_managed.get()),
                           m_workbench.m_managed->restarts(), static_cast<int>(m_workbench.m_managed->state()));
    if (isLuaSource(path) && m_workbench.m_lua)
        return std::format("{}:{}:{}:{}", key, reinterpret_cast<std::uintptr_t>(m_workbench.m_lua.get()),
                           m_workbench.m_lua->restarts(), static_cast<int>(m_workbench.m_lua->state()));
    if (isCFamilySource(path) && m_workbench.m_clangd)
        return std::format("{}:{}:{}:{}", key, reinterpret_cast<std::uintptr_t>(m_workbench.m_clangd.get()),
                           m_workbench.m_clangd->restarts(), static_cast<int>(m_workbench.m_clangd->state()));
    return std::format("{}:none", key);
}

ByteOffset LanguageBridge::caretOffset() const {
    const ui::EditorView* editor = activeEditor();
    return editor != nullptr ? editor->editor().state().primary().head : ByteOffset{0};
}

std::optional<text::ByteRange> LanguageBridge::selection() const {
    const ui::EditorView* editor = activeEditor();
    if (editor == nullptr || editor->editor().state().primary().empty()) {
        return std::nullopt;
    }
    return editor->editor().state().primary().range();
}

ui::lang::FormattingSettings LanguageBridge::formattingOptions() const {
    const ui::EditorView* editor = activeEditor();
    if (!editor)
        return {};
    const auto& settings = editor->settings();
    return {.tabSize = settings.tabSize, .indentSize = settings.indentSize, .insertSpaces = settings.insertSpaces};
}

QRect LanguageBridge::caretScreenRect() const {
    const ui::EditorView* editor = activeEditor();
    return editor != nullptr ? editor->globalCaretRect(editor->editor().state().primary().head) : QRect();
}

QRect LanguageBridge::screenRectAt(ByteOffset offset) const {
    const ui::EditorView* editor = activeEditor();
    return editor != nullptr ? editor->globalCaretRect(offset) : QRect();
}

Result<void> LanguageBridge::applyTo(ui::EditorView& editor, std::span<const lang::ByteEdit> edits,
                                     std::optional<ByteOffset> caret) {
    if (editor.isReadOnly()) {
        return fail(ErrorCode::InvalidArgument, editor.path() + " is read-only");
    }
    text::CommandResult result;
    result.edits.reserve(edits.size());
    for (const lang::ByteEdit& edit : edits) {
        result.edits.push_back(text::TextEdit{edit.range, edit.text});
    }
    const ByteOffset at = caret.value_or(mapThrough(editor.editor().state().primary().head, edits));
    result.state = text::EditorState::caretAt(at);
    result.breakCoalescing = true; // One undo step of its own.
    if (!editor.apply(result)) {
        return editor.lastError() ? std::unexpected(*editor.lastError())
                                  : fail(ErrorCode::InvalidArgument, "the editor refused the edits");
    }
    return {};
}

Result<void> LanguageBridge::applyEdits(std::span<const lang::ByteEdit> edits, std::string_view,
                                        std::optional<ByteOffset> caret) {
    ui::EditorView* editor = activeEditor();
    if (editor == nullptr) {
        return fail(ErrorCode::NotFound, "no editor is active");
    }
    return applyTo(*editor, edits, caret);
}

void LanguageBridge::select(text::ByteRange range) {
    ui::EditorView* editor = activeEditor();
    if (editor == nullptr) {
        return;
    }
    const text::Selection selection{range.begin, range.end};
    editor->setState(text::EditorState::from(text::SelectionSet::fromSelections(std::span(&selection, 1), 0)));
}

void LanguageBridge::openLocation(std::string_view path, lang::LspPosition position) {
    // Through the one open path (a jump in the navigation history), then the exact LSP position.
    if (!m_workbench.openPath(path)) {
        return;
    }
    DocumentEditorView* view = m_workbench.currentView();
    if (view == nullptr) {
        return;
    }
    const text::Snapshot& text = view->editor().editor().text();
    const Result<ByteOffset> offset = lang::fromLspPosition(text, position, encoding());
    if (!offset) {
        return;
    }
    const text::Point point = text.pointAt(*offset);
    view->goToLine(static_cast<int>(point.line.value()) + 1, static_cast<int>(point.column.value()) + 1);
}

void LanguageBridge::openReadOnlyLocation(std::string_view path, lang::LspPosition position) {
    openLocation(path, position);
    DocumentEditorView* view = m_workbench.currentView();
    if (view != nullptr && lang::documentKey(view->path()) == lang::documentKey(path))
        view->setForcedReadOnly(true);
}

void LanguageBridge::setUsageHighlights(std::vector<lang::DocumentHighlight> highlights) {
    ui::EditorView* editor = activeEditor();
    if (editor == nullptr) {
        return;
    }
    // Editor-insight's usage boxes (read and write usages differ) and their error-stripe marks.
    editor->setUsageHighlights(ui::lang::toUsageHighlights(highlights, editor->editor().text(), encoding()));
}

std::vector<std::pair<std::string, text::Snapshot>> LanguageBridge::openDocuments() const {
    std::vector<std::pair<std::string, text::Snapshot>> documents;
    for (DocumentEditorView* view : m_workbench.editors().uniqueDocumentViews()) {
        documents.emplace_back(view->path(), view->editor().editor().text());
    }
    return documents;
}

QWidget* LanguageBridge::editorWidget() const {
    ui::EditorView* editor = activeEditor();
    return editor != nullptr ? static_cast<QWidget*>(editor) : static_cast<QWidget*>(m_workbench.window());
}

// --- IWorkspaceDocuments ---

std::optional<lang::OpenDocumentState> LanguageBridge::find(std::string_view path) {
    DocumentEditorView* view = m_workbench.editors().find(path);
    if (view == nullptr) {
        return std::nullopt;
    }
    lang::OpenDocumentState state;
    state.snapshot = view->editor().editor().text();
    if (isManagedSource(path) && m_workbench.m_managed)
        state.lspVersion = m_workbench.m_managed->documentVersion(path);
    else if (isLuaSource(path) && m_workbench.m_lua)
        state.lspVersion = m_workbench.m_lua->documentVersion(path);
    else if (isCFamilySource(path) && m_workbench.m_clangd)
        state.lspVersion = m_workbench.m_clangd->documentVersion(path);
    state.dirty = view->document().dirty();
    state.changedOnDisk = view->document().diskState() == document::DiskState::Changed ||
                          view->document().diskState() == document::DiskState::OutOfDate;
    return state;
}

Result<void> LanguageBridge::applyEdits(std::string_view path, std::span<const lang::ByteEdit> edits,
                                        std::string_view) {
    DocumentEditorView* view = m_workbench.editors().find(path);
    if (view == nullptr) {
        return fail(ErrorCode::NotFound, std::string(path) + " is not open");
    }
    return applyTo(view->editor(), edits, std::nullopt);
}

namespace {

sched::Task<void> runWorkspaceSymbolQuery(lang::ILspRequester& server, std::string query, bool classesOnly,
                                          std::stop_token token,
                                          std::function<void(std::vector<ui::WorkbenchSymbol>)> deliver) {
    auto result = co_await lang::requestWorkspaceSymbols(server, std::move(query), std::move(token));
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) {
        co_return;
    }
    std::vector<ui::WorkbenchSymbol> out;
    if (result) {
        for (const lang::WorkspaceSymbol& sym : *result) {
            const bool isClass = sym.kind == lang::SymbolKind::Class || sym.kind == lang::SymbolKind::Struct ||
                                 sym.kind == lang::SymbolKind::Interface || sym.kind == lang::SymbolKind::Enum ||
                                 sym.kind == lang::SymbolKind::Namespace;
            if (classesOnly && !isClass) {
                continue;
            }
            out.push_back(ui::WorkbenchSymbol{
                .name = sym.name,
                .container = sym.containerName,
                .path = sym.location.path,
                .line = static_cast<int>(sym.location.range.start.line) + 1,
                .column = static_cast<int>(sym.location.range.start.character) + 1,
                .isClassLike = isClass,
            });
        }
    }
    QMetaObject::invokeMethod(
        app, [deliver = std::move(deliver), out = std::move(out)]() mutable { deliver(std::move(out)); },
        Qt::QueuedConnection);
}

// Reads `database` (JSON compilation database format: an array of {directory, file, ...}). Runs on
// a worker; O(size of the file).
std::shared_ptr<const CompileDatabaseState> readCompileDatabase(io::IFileSystem& fs, std::string database,
                                                                io::FileStatus status) {
    auto state = std::make_shared<CompileDatabaseState>();
    state->database = std::move(database);
    state->status = status;
    auto bytes = fs.readFile(state->database);
    if (!bytes)
        return state;
    auto json = lang::parseJson(*bytes);
    if (!json || !json->isArray()) {
        state->kind = CompileDatabaseState::Kind::Unparseable;
        return state;
    }
    // An array whose entries clangd rejects (no "file", no command) still counts as Entries: clangd
    // would then fall back, but claiming nothing (Unknown) is never false.
    state->kind = json->items().empty() ? CompileDatabaseState::Kind::Empty : CompileDatabaseState::Kind::Entries;
    return state;
}

sched::Task<void> readCompileDatabaseTask(io::IFileSystem* fs, std::string database, io::FileStatus status,
                                          std::function<void(std::shared_ptr<const CompileDatabaseState>)> done) {
    done(readCompileDatabase(*fs, std::move(database), status));
    co_return;
}

} // namespace

ui::lang::ServerReadiness Workbench::languageReadiness(std::string_view path) {
    using Readiness = ui::lang::ServerReadiness;
    if (isLuaSource(path))
        return m_lua && m_lua->state() == lang::LuaState::Running ? Readiness::Unknown : Readiness::NotRunning;
    if (isManagedSource(path))
        return m_managed && m_managed->state() == lang::LuaState::Running ? Readiness::Unknown : Readiness::NotRunning;
    if (!isCFamilySource(path) || !m_clangd || m_clangd->state() != lang::ClangdState::Running)
        return Readiness::NotRunning;
    if (clangdUsesFallbackFlags(path) == true)
        return Readiness::NoCompileCommand;
    return m_clangdIndexing ? Readiness::Indexing : Readiness::Unknown;
}

std::optional<bool> Workbench::clangdUsesFallbackFlags(std::string_view path) {
    using Kind = CompileDatabaseState::Kind;
    if (!m_clangd)
        return std::nullopt;
    // <directory>/<name> when it exists, "" when it does not, nullopt when that cannot be told.
    const auto existing = [&](std::string_view directory, std::string_view name) -> std::optional<std::string> {
        auto joined = io::joinPath(directory, name);
        auto file = joined ? io::normalizePath(*joined) : joined;
        if (!file)
            return std::nullopt;
        auto found = m_services.fs.exists(*file);
        if (!found)
            return std::nullopt;
        return *found ? std::move(*file) : std::string();
    };
    if (const std::optional<lang::CompileCommandsLocation> location = m_clangd->compileCommands()) {
        // --compile-commands-dir: clangd reads that folder's compile_commands.json and nothing else;
        // a missing or unparseable one leaves the fallback (both checked with `clangd --check`).
        const std::optional<std::string> database = existing(location->directory, "compile_commands.json");
        if (!database)
            return std::nullopt;
        if (database->empty())
            return true;
        const CompileDatabaseState* state = compileDatabaseState(*database);
        if (state == nullptr || state->kind == Kind::Unreadable)
            return std::nullopt;
        return state->kind != Kind::Entries;
    }
    /*
     * No --compile-commands-dir: the nearest folder that gives clangd a database, from the file's
     * own upward. Checked with `clangd --check`: in one folder compile_commands.json wins over
     * compile_flags.txt, an empty database there stops the search (fallback), and one clangd cannot
     * parse is skipped and the search goes on.
     */
    auto normalized = io::normalizePath(path);
    if (!normalized)
        return std::nullopt;
    for (std::string_view directory = io::parentPath(*normalized); !directory.empty();
         directory = io::parentPath(directory)) {
        for (const std::string_view name :
             {std::string_view("compile_commands.json"), std::string_view("build/compile_commands.json")}) {
            const std::optional<std::string> database = existing(directory, name);
            if (!database)
                return std::nullopt;
            if (database->empty())
                continue;
            const CompileDatabaseState* state = compileDatabaseState(*database);
            if (state == nullptr || state->kind == Kind::Unreadable)
                return std::nullopt;
            if (state->kind == Kind::Unparseable)
                continue;
            return state->kind == Kind::Empty;
        }
        const std::optional<std::string> flags = existing(directory, "compile_flags.txt");
        if (!flags)
            return std::nullopt;
        if (!flags->empty())
            return false; // clangd gives every file below it these flags.
    }
    return true; // No database above the file: clangd's "Failed to find compilation database".
}

const CompileDatabaseState* Workbench::compileDatabaseState(const std::string& database) {
    const Result<io::FileStatus> status = m_services.fs.status(database);
    if (!status)
        return nullptr;
    if (const auto known = m_compileDatabases.find(database);
        known != m_compileDatabases.end() && known->second->status == *status)
        return known->second.get();
    // Not read yet, or changed since (a reconfigure rewrites it in place): read it on a worker and
    // claim nothing until then. One read at a time; the next question after it ends checks again.
    if (!m_compileDatabaseReading && m_pool && !m_shutDown) {
        m_compileDatabaseReading = true;
        const QPointer<Workbench> self(this);
        auto done = [self](std::shared_ptr<const CompileDatabaseState> state) {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self, state = std::move(state)] {
                    if (!self)
                        return;
                    self->m_compileDatabaseReading = false;
                    // clangd's database and those above a few files: a handful, bounded anyway.
                    if (self->m_compileDatabases.size() >= 16)
                        self->m_compileDatabases.clear();
                    self->m_compileDatabases[state->database] = state;
                },
                Qt::QueuedConnection);
        };
        m_pool
            ->spawn(sched::Lane::Background,
                    readCompileDatabaseTask(&m_services.fs, database, *status, std::move(done)))
            .detach();
    }
    return nullptr;
}

void Workbench::queryWorkspaceSymbols(std::string_view query, bool classesOnly,
                                      std::function<void(std::vector<ui::WorkbenchSymbol>)> done) {
    if (query.empty() || !m_bridge || !m_pool || m_shutDown) {
        done({});
        return;
    }
    const sched::LatestWins::Ticket ticket = m_searchSymbolsSlot.next();
    const QPointer<Workbench> self(this);
    m_pool
        ->spawn(sched::Lane::Interactive,
                runWorkspaceSymbolQuery(
                    *m_bridge, std::string(query), classesOnly, ticket.token,
                    [self, ticket, done = std::move(done)](std::vector<ui::WorkbenchSymbol> out) mutable {
                        if (!self || !self->m_searchSymbolsSlot.isCurrent(ticket)) {
                            return;
                        }
                        done(std::move(out));
                    }))
        .detach();
}

} // namespace frost::app
