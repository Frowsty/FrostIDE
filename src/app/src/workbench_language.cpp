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
#include <array>
#include <format>
#include <memory>
#include <unordered_set>
#include <utility>

namespace frost::app {

// The source files a compile_commands.json names (Workbench::languageReadiness).
struct CompileCommandFiles {
    std::string database;                 // The compile_commands.json that was read.
    io::FileStatus status;                // Its size and modification time then; a change means a new read.
    bool readable = false;                // false: it could not be read or parsed, so nothing is known.
    std::unordered_set<std::string> keys; // lang::documentKey of every entry's absolute "file".
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

// Reads `database` (JSON compilation database format: an array of {directory, file, ...}); a
// relative "file" is below its "directory". Runs on a worker; O(size of the file).
std::shared_ptr<const CompileCommandFiles> readCompileCommandFiles(io::IFileSystem& fs, std::string database,
                                                                   io::FileStatus status) {
    auto files = std::make_shared<CompileCommandFiles>();
    files->database = std::move(database);
    files->status = status;
    auto bytes = fs.readFile(files->database);
    if (!bytes)
        return files;
    auto json = lang::parseJson(*bytes);
    if (!json || !json->isArray())
        return files;
    for (const lang::Json& entry : json->items()) {
        if (!entry.isObject())
            continue;
        std::string file(entry.stringAt("file"));
        if (file.empty())
            continue;
        if (!io::isAbsolutePath(file)) {
            auto joined = io::joinPath(entry.stringAt("directory"), file);
            if (!joined)
                continue;
            file = std::move(*joined);
        }
        if (auto normalized = io::normalizePath(file))
            files->keys.insert(lang::documentKey(*normalized));
    }
    files->readable = true;
    return files;
}

sched::Task<void> readCompileCommandFilesTask(io::IFileSystem* fs, std::string database, io::FileStatus status,
                                              std::function<void(std::shared_ptr<const CompileCommandFiles>)> done) {
    done(readCompileCommandFiles(*fs, std::move(database), status));
    co_return;
}

// Headers get their compile command from a source that includes them (clangd interpolates), so a
// header absent from the database says nothing about its flags.
bool isHeaderPath(std::string_view path) {
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of("/\\");
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        return false;
    std::string extension(path.substr(dot));
    for (char& c : extension)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    constexpr std::array<std::string_view, 10> kHeaders{".h",   ".hh",  ".hpp", ".hxx", ".h++",
                                                        ".inl", ".ipp", ".tpp", ".cuh", ".inc"};
    return std::ranges::find(kHeaders, extension) != kHeaders.end();
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
    if (compileCommandListed(path) == false)
        return Readiness::NoCompileCommand;
    return m_clangdIndexing ? Readiness::Indexing : Readiness::Unknown;
}

std::optional<bool> Workbench::compileCommandListed(std::string_view path) {
    if (!m_clangd)
        return std::nullopt;
    const std::optional<lang::CompileCommandsLocation> location = m_clangd->compileCommands();
    if (!location)
        return false; // clangd runs without a database: every file gets its fallback flags.
    if (isHeaderPath(path))
        return std::nullopt;
    const CompileCommandFiles* files = currentCompileCommandFiles();
    if (files == nullptr || !files->readable)
        return std::nullopt;
    auto normalized = io::normalizePath(path);
    return normalized && files->keys.contains(lang::documentKey(*normalized));
}

const CompileCommandFiles* Workbench::currentCompileCommandFiles() {
    const std::optional<lang::CompileCommandsLocation> location = m_clangd ? m_clangd->compileCommands() : std::nullopt;
    if (!location)
        return nullptr;
    const Result<io::FileStatus> status = m_services.fs.status(location->file);
    if (!status)
        return nullptr;
    if (m_compileCommandFiles && m_compileCommandFiles->database == location->file &&
        m_compileCommandFiles->status == *status)
        return m_compileCommandFiles.get();
    // Not read yet, or changed since (a reconfigure rewrites it in place): read it on a worker and
    // claim nothing until then. One read at a time; the next question after it ends checks again.
    if (!m_compileCommandFilesReading && m_pool && !m_shutDown) {
        m_compileCommandFilesReading = true;
        const QPointer<Workbench> self(this);
        auto done = [self](std::shared_ptr<const CompileCommandFiles> files) {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self, files = std::move(files)] {
                    if (!self)
                        return;
                    self->m_compileCommandFilesReading = false;
                    self->m_compileCommandFiles = files;
                },
                Qt::QueuedConnection);
        };
        m_pool
            ->spawn(sched::Lane::Background,
                    readCompileCommandFilesTask(&m_services.fs, location->file, *status, std::move(done)))
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
