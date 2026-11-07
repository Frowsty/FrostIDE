// SPDX-License-Identifier: Apache-2.0
//
// LanguageController (frost/ui/lang/language_controller.hpp): the language features of the
// active editor, from the action handlers and typing hooks to frost::lang's request models and
// back to the widgets on the UI thread.

#include "frost/ui/lang/language_controller.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/clangd_actions.hpp"
#include "frost/lang/code_assist.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/snippet.hpp"
#include "frost/lang/symbols.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"

#include "language_controller_internal.hpp"
#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <map>
#include <sstream>
#include <utility>

namespace frost::ui::lang {

namespace fl = ::frost::lang;

namespace {

using detail::fileName;
using detail::lowerAscii;
using detail::qs;

bool allIdentifierBytes(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) { return fl::isIdentifierByte(c); });
}

bool isHeaderExt(std::string_view ext) {
    return ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".inl" || ext == ".ipp";
}

bool isSourceExt(std::string_view ext) {
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".m" || ext == ".mm";
}

bool isClassLikeSymbol(fl::SymbolKind kind) {
    return kind == fl::SymbolKind::Class || kind == fl::SymbolKind::Struct || kind == fl::SymbolKind::Interface ||
           kind == fl::SymbolKind::Enum || kind == fl::SymbolKind::Namespace;
}

struct SplitPathParts {
    std::string dir;
    std::string stem;
    std::string ext;
    std::string prefixWithoutExt;
};

SplitPathParts splitPathParts(std::string_view path) {
    SplitPathParts out;
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view dirView = slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
    const std::string_view baseView = slash == std::string_view::npos ? path : path.substr(slash + 1);
    out.dir = lowerAscii(dirView);
    std::ranges::replace(out.dir, '\\', '/');
    const std::size_t dot = baseView.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        out.stem = lowerAscii(baseView);
        out.prefixWithoutExt = std::string(path);
    } else {
        out.stem = lowerAscii(baseView.substr(0, dot));
        out.ext = lowerAscii(baseView.substr(dot));
        const std::size_t baseOffset = slash == std::string_view::npos ? 0 : slash + 1;
        out.prefixWithoutExt = std::string(path.substr(0, baseOffset + dot));
    }
    return out;
}

std::optional<std::string> findRelatedProjectFile(std::string_view currentPath,
                                                  const std::function<std::vector<std::string>()>& projectFiles,
                                                  io::IFileSystem& fs) {
    const SplitPathParts current = splitPathParts(currentPath);
    const bool wantSource = isHeaderExt(current.ext);
    const bool wantHeader = isSourceExt(current.ext);
    if ((!wantSource && !wantHeader) || current.stem.empty()) {
        return std::nullopt;
    }

    if (projectFiles) {
        std::optional<std::string> best;
        int bestScore = -1;
        for (const std::string& candidateStr : projectFiles()) {
            if (fl::documentKey(candidateStr) == fl::documentKey(currentPath)) {
                continue;
            }
            const SplitPathParts candidate = splitPathParts(candidateStr);
            if (candidate.stem != current.stem) {
                continue;
            }
            if ((wantSource && !isSourceExt(candidate.ext)) || (wantHeader && !isHeaderExt(candidate.ext))) {
                continue;
            }
            int score = 1;
            if (candidate.dir == current.dir) {
                score = 100;
            } else {
                const std::size_t limit = std::min(candidate.dir.size(), current.dir.size());
                std::size_t prefix = 0;
                while (prefix < limit && candidate.dir[prefix] == current.dir[prefix]) {
                    ++prefix;
                }
                score = 10 + static_cast<int>(prefix);
            }
            if (score > bestScore) {
                bestScore = score;
                best = candidateStr;
            }
        }
        if (best) {
            return best;
        }
    }

    static constexpr std::string_view kSourceExts[] = {".cpp", ".cc", ".cxx", ".c", ".mm", ".m"};
    static constexpr std::string_view kHeaderExts[] = {".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp"};
    const std::span<const std::string_view> exts =
        wantSource ? std::span<const std::string_view>(kSourceExts) : std::span<const std::string_view>(kHeaderExts);
    for (std::string_view targetExt : exts) {
        const std::string siblingStr = current.prefixWithoutExt + std::string(targetExt);
        if (fs.exists(siblingStr) == true) {
            return siblingStr;
        }
    }
    return std::nullopt;
}

// The identifier at `position`, for messages ("Source not available for 'X'"). "" when none.
std::string identifierAtPosition(const std::optional<text::Snapshot>& snapshot, fl::LspPosition position,
                                 fl::PositionEncoding encoding) {
    if (!snapshot) {
        return {};
    }
    const auto offset = fl::fromLspPosition(*snapshot, position, encoding);
    if (!offset) {
        return {};
    }
    const std::string text = snapshot->text();
    std::size_t pos = std::min(offset->value(), text.size());
    if (pos > 0 && (pos == text.size() || !fl::isIdentifierByte(text[pos])) && fl::isIdentifierByte(text[pos - 1])) {
        --pos;
    }
    if (pos >= text.size() || !fl::isIdentifierByte(text[pos])) {
        return {};
    }
    std::size_t begin = pos;
    while (begin > 0 && fl::isIdentifierByte(text[begin - 1])) {
        --begin;
    }
    std::size_t end = pos;
    while (end < text.size() && fl::isIdentifierByte(text[end])) {
        ++end;
    }
    return text.substr(begin, end - begin);
}

// A metadata URI as C# language servers answer for symbols without source (csharp:/metadata/...).
bool isMetadataUri(std::string_view uri) {
    return uri.starts_with("csharp:") || uri.starts_with("roslyn:") || uri.starts_with("metadata:");
}

// A file name for metadata `uri` under the cache: its last segment reduced to [A-Za-z0-9._-],
// prefixed by a hash of the whole URI so two assemblies' "Console.cs" do not collide.
std::string metadataFileName(std::string_view uri) {
    const std::size_t slash = uri.find_last_of("/\\:");
    std::string_view last = slash == std::string_view::npos ? uri : uri.substr(slash + 1);
    std::string name;
    for (const char c : last) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '_' || c == '-';
        name.push_back(safe ? c : '_');
    }
    while (!name.empty() && name.front() == '.') {
        name.erase(name.begin());
    }
    if (name.empty()) {
        name = "metadata.cs";
    }
    // FNV-1a (public domain), 64-bit.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : uri) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return std::format("{:016x}-{}", hash, name);
}

/*
 * Real metadata-as-source for the symbol at `position` (LTR-1): textDocument/definition answered
 * with a metadata URI, and the server's csharp/metadata answered with its source. The source is
 * written to `cacheDir` (never the workspace) and the location points into that file at the range
 * the server gave. nullopt when the server has no metadata (or no cache directory is configured).
 */
sched::Task<Result<std::optional<fl::Location>>> metadataSourceTask(fl::ILspRequester& server, io::IFileSystem& fs,
                                                                    std::string cacheDir, std::string path,
                                                                    fl::LspPosition position, std::stop_token token) {
    const auto uri = fl::pathToUri(path);
    if (!uri) {
        co_return std::unexpected(uri.error());
    }
    fl::Json params = fl::Json::object()
                          .set("textDocument", fl::Json::object().set("uri", *uri))
                          .set("position", fl::Json::object()
                                               .set("line", static_cast<std::int64_t>(position.line))
                                               .set("character", static_cast<std::int64_t>(position.character)));
    auto definition = co_await server.request("textDocument/definition", std::move(params), token);
    if (!definition) {
        co_return std::unexpected(std::move(definition.error()));
    }
    std::string metadataUri;
    fl::LspRange range{};
    const auto consider = [&](const fl::Json& item) {
        for (std::string_view key : {"uri", "targetUri"}) {
            if (const std::string_view value = item.stringAt(key); metadataUri.empty() && isMetadataUri(value)) {
                metadataUri = std::string(value);
                for (std::string_view rangeKey : {"targetSelectionRange", "range", "targetRange"}) {
                    if (const fl::Json* r = item.find(rangeKey); r != nullptr) {
                        if (auto parsed = fl::parseRange(*r)) {
                            range = *parsed;
                            break;
                        }
                    }
                }
            }
        }
    };
    if (definition->isObject()) {
        consider(*definition);
    } else {
        for (const fl::Json& item : definition->items()) {
            if (item.isObject()) {
                consider(item);
            }
        }
    }
    if (metadataUri.empty() || cacheDir.empty()) {
        co_return std::optional<fl::Location>();
    }
    auto metadata = co_await server.request(
        "csharp/metadata", fl::Json::object().set("textDocument", fl::Json::object().set("uri", metadataUri)), token);
    if (!metadata || !metadata->isObject() || metadata->stringAt("source").empty()) {
        co_return std::optional<fl::Location>();
    }
    auto file = io::joinPath(cacheDir, metadataFileName(metadataUri));
    if (!file) {
        co_return std::unexpected(std::move(file.error()));
    }
    if (auto made = fs.createDirectories(cacheDir); !made) {
        co_return std::unexpected(std::move(made.error()));
    }
    if (auto wrote = fs.writeFileAtomic(*file, metadata->stringAt("source")); !wrote) {
        co_return std::unexpected(std::move(wrote.error()));
    }
    const std::string fileUri = fl::pathToUri(*file).value_or("");
    co_return std::optional<fl::Location>(fl::Location{.path = *file, .uri = fileUri, .range = range});
}

// Go to Declaration or Usages from the server alone; when it found nothing for a C# document, the
// server's metadata source (metadataSourceTask) if it has one.
struct GotoOutcome {
    fl::GotoResult result;
    bool metadata = false; // result.locations holds one location in the metadata cache.
};

sched::Task<Result<GotoOutcome>> gotoDeclarationTask(fl::ILspRequester& server, io::IFileSystem& fs,
                                                     std::string cacheDir, bool askMetadata, std::string path,
                                                     fl::LspPosition position, std::stop_token token) {
    auto result = co_await fl::gotoDeclarationOrUsages(server, path, position, token);
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    GotoOutcome outcome{.result = std::move(*result), .metadata = false};
    if (askMetadata && !outcome.result.showUsages && outcome.result.locations.empty()) {
        auto metadata = co_await metadataSourceTask(server, fs, std::move(cacheDir), path, position, token);
        if (metadata && *metadata) {
            outcome.result.locations.push_back(std::move(**metadata));
            outcome.metadata = true;
        }
    }
    co_return outcome;
}

} // namespace

LanguageController::LanguageController(LanguageServices services, QObject* parent)
    : QObject(parent), m_services(std::move(services)), m_completion(std::make_unique<CompletionPopup>()),
      m_parameterInfo(std::make_unique<ParameterInfoPopup>()), m_quickDoc(std::make_unique<QuickDocPopup>()),
      m_usages(std::make_unique<FindUsagesPanel>()), m_symbols(std::make_unique<SymbolPopup>()),
      m_structure(std::make_unique<StructurePanel>()), m_rename(std::make_unique<RenameInput>()),
      m_choice(std::make_unique<ChoicePopup>()), m_quickDef(std::make_unique<QuickDefinitionPopup>()),
      m_hierarchy(std::make_unique<HierarchyPopup>()) {
    m_highlightTimer.setSingleShot(true);
    m_highlightTimer.setInterval(300);
    connect(&m_highlightTimer, &QTimer::timeout, this, &LanguageController::highlightUsages);
    m_symbolQueryTimer.setSingleShot(true);
    m_symbolQueryTimer.setInterval(150);
    connect(&m_symbolQueryTimer, &QTimer::timeout, this,
            [this] { requestWorkspaceSymbols(m_symbols->query().trimmed()); });
    m_structureTimer.setSingleShot(true);
    m_structureTimer.setInterval(150);
    connect(&m_structureTimer, &QTimer::timeout, this, &LanguageController::requestStructure);
    initServiceHooks();

    connect(m_completion.get(), &CompletionPopup::commitRequested, this, &LanguageController::commitCompletion);
    connect(m_completion.get(), &CompletionPopup::cancelled, this, &LanguageController::closeLookup);
    connect(m_completion.get(), &CompletionPopup::selectionChanged, this, &LanguageController::resolveSelected);
    connect(m_usages.get(), &FindUsagesPanel::openRequested, this,
            [this](const QString& path, fl::LspPosition position) {
                m_services.host.openLocation(path.toStdString(), position);
            });
    connect(m_symbols.get(), &SymbolPopup::activated, this, [this](const QString& path, fl::LspPosition position) {
        m_services.host.openLocation(path.toStdString(), position);
    });
    connect(m_symbols.get(), &SymbolPopup::queryChanged, this, [this](const QString&) { m_symbolQueryTimer.start(); });
    connect(m_quickDef.get(), &QuickDefinitionPopup::openRequested, this,
            [this](const QString& path, fl::LspPosition position) {
                m_services.host.openLocation(path.toStdString(), position);
            });
    connect(m_hierarchy.get(), &HierarchyPopup::activated, this, [this](const QString& path, fl::LspPosition position) {
        m_services.host.openLocation(path.toStdString(), position);
    });
    connect(m_hierarchy.get(), &HierarchyPopup::modeChanged, this, [this](bool secondary) {
        if (m_hierarchyIsType) {
            refreshTypeHierarchy(secondary);
        } else {
            refreshCallHierarchy(secondary);
        }
    });
    connect(m_choice.get(), &ChoicePopup::chosen, this, [this](std::size_t index) {
        std::vector<std::function<void()>> actions = std::move(m_choiceActions);
        m_choiceActions.clear();
        if (index < actions.size() && actions[index]) {
            actions[index]();
        }
    });
    connect(m_choice.get(), &ChoicePopup::cancelled, this, [this] { m_choiceActions.clear(); });
    connect(m_rename.get(), &RenameInput::accepted, this, &LanguageController::onRenameAccepted);
}

void LanguageController::initServiceHooks() {
    connect(m_structure.get(), &StructurePanel::activated, this, [this](const QString& path, fl::LspPosition position) {
        const auto snapshot = m_services.host.snapshot();
        if (m_structureVisible && !m_structurePath.empty() && path.toStdString() == m_structurePath &&
            m_services.host.documentPath() == m_structurePath &&
            m_services.host.documentVersion() == m_structureVersion && snapshot &&
            snapshot->version() == m_structureRevision && m_services.host.structureIdentity() == m_structureIdentity &&
            m_services.server.encoding() == m_structureEncoding) {
            const std::string targetPath = m_structurePath;
            m_services.host.openLocation(targetPath, position);
        }
    });
    m_usages->setLineReader([this](std::string_view file, std::uint32_t lineIndex) -> std::optional<std::string> {
        for (const auto& [openPath, snap] : m_services.host.openDocuments()) {
            if (fl::documentKey(openPath) == fl::documentKey(file)) {
                if (auto info = snap.lineInfo(LineIndex{lineIndex})) {
                    return snap.text(info->content);
                }
                return std::nullopt;
            }
        }
        if (auto content = m_services.fs.readFile(file)) {
            if (auto buf = text::TextBuffer::fromUtf8(std::move(*content))) {
                const auto snap = buf->snapshot();
                if (auto info = snap.lineInfo(LineIndex{lineIndex})) {
                    return snap.text(info->content);
                }
            }
        }
        return std::nullopt;
    });
    m_services.server.setApplyEditHandler([self = QPointer<LanguageController>(this)](const fl::Json& params) -> bool {
        if (!self) {
            return false;
        }
        const fl::Json* editJson = params.find("edit");
        auto parsed = fl::parseWorkspaceEdit(editJson != nullptr ? *editJson : params);
        if (!parsed || parsed->empty()) {
            return false;
        }
        const QString label =
            params.find("label") != nullptr ? qs(params.stringAt("label")) : QStringLiteral("Apply Server Edit");
        if (QThread::currentThread() == self->thread()) {
            self->applyWorkspaceEdit(*parsed, label);
            return true;
        }
        if (QCoreApplication* app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(
                app,
                [self, edit = std::move(*parsed), label]() {
                    if (self) {
                        self->applyWorkspaceEdit(edit, label);
                    }
                },
                Qt::QueuedConnection);
            return true;
        }
        return false;
    });
}

LanguageController::~LanguageController() {
    m_structureSlot.cancel();
    for (sched::LatestWins* slot :
         {&m_completionSlot, &m_resolveSlot, &m_signatureSlot, &m_hoverSlot, &m_navigationSlot, &m_usagesSlot,
          &m_highlightSlot, &m_symbolSlot, &m_renameSlot, &m_actionSlot, &m_formatSlot, &m_hierarchySlot}) {
        slot->cancel();
    }
}

void LanguageController::editorChanged() {
    const auto snapshot = m_services.host.snapshot();
    const bool sameStructureDoc =
        m_structureVisible && !m_structurePath.empty() && m_services.host.documentPath() == m_structurePath &&
        m_services.host.documentVersion() == m_structureVersion && snapshot &&
        snapshot->version() == m_structureRevision && m_services.host.structureIdentity() == m_structureIdentity &&
        m_services.server.encoding() == m_structureEncoding;
    if (!sameStructureDoc) {
        structureChanged();
    }
    for (sched::LatestWins* slot :
         {&m_completionSlot, &m_resolveSlot, &m_signatureSlot, &m_hoverSlot, &m_navigationSlot, &m_usagesSlot,
          &m_highlightSlot, &m_symbolSlot, &m_renameSlot, &m_actionSlot, &m_formatSlot, &m_hierarchySlot}) {
        slot->cancel();
    }
    m_highlightTimer.stop();
    m_symbolQueryTimer.stop();
    closeLookup();
    m_parameterInfo->hide();
    m_quickDoc->hide();
    m_rename->hide();
    m_choice->hide();
    m_quickDef->hide();
    m_hierarchy->hide();
    m_tabStops.clear();
    m_renamePath.clear();
    m_renameOldName.clear();
}

StructurePanel& LanguageController::structurePanel() { return *m_structure; }

void LanguageController::setStructureVisible(bool visible) {
    m_structureVisible = visible;
    structureChanged();
}

void LanguageController::structureChanged() { invalidateStructure(true); }

void LanguageController::structureReplaced() { invalidateStructure(false); }

void LanguageController::invalidateStructure(bool keepSameIdentityTree) {
    ++m_structureGeneration;
    m_structureSlot.cancel();
    m_structureTimer.stop();
    const bool keepExistingTreeForSameIdentity = keepSameIdentityTree && m_structureVisible && hasDocument() &&
                                                 !m_structurePath.empty() &&
                                                 m_services.host.documentPath() == m_structurePath &&
                                                 m_services.host.structureIdentity() == m_structureIdentity &&
                                                 m_services.server.encoding() == m_structureEncoding;
    m_structurePath.clear();
    if (!m_structureVisible) {
        m_structure->setStatus(QStringLiteral("Open Structure to view symbols."));
        return;
    }
    if (!hasDocument()) {
        m_structure->setStatus(QStringLiteral("Open a file to view its structure."));
        return;
    }
    if (!keepExistingTreeForSameIdentity) {
        m_structure->setStatus(QStringLiteral("Loading structure..."));
    }
    m_structureTimer.start();
}

void LanguageController::requestStructure() {
    if (!m_structureVisible || !hasDocument())
        return;
    const auto generation = m_structureGeneration;
    const std::string path = m_services.host.documentPath();
    const auto version = m_services.host.documentVersion();
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot)
        return;
    const auto revision = snapshot->version();
    const auto identity = m_services.host.structureIdentity();
    const auto encoding = m_services.server.encoding();
    start<std::vector<fl::DocumentSymbol>>(
        m_structureSlot,
        [&server = m_services.server, path](std::stop_token token) {
            return fl::requestDocumentSymbols(server, path, std::move(token));
        },
        [this, generation, path, version, revision, identity,
         encoding](Result<std::vector<fl::DocumentSymbol>> symbols) {
            const auto current = m_services.host.snapshot();
            if (!m_structureVisible || generation != m_structureGeneration || m_services.host.documentPath() != path ||
                m_services.host.documentVersion() != version || m_services.host.structureIdentity() != identity ||
                !current || current->version() != revision || m_services.server.encoding() != encoding)
                return;
            if (!symbols) {
                m_structure->setStatus(QStringLiteral("Cannot load structure: %1").arg(qs(symbols.error().message())));
                return;
            }
            m_structurePath = path;
            m_structureVersion = version;
            m_structureRevision = revision;
            m_structureIdentity = identity;
            m_structureEncoding = encoding;
            m_structure->setSymbols(path, std::move(*symbols));
        });
}

namespace {

// Which documents an action's engine serves (registerActions; language_capabilities.hpp).
enum class Availability : std::uint8_t {
    Always,            // Any document (LSP-backed or editor-wide).
    ClangdTweaks,      // C++ only: clangd tweaks.
    OptimizeImports,   // capabilities.optimizeImports.
    Surround,          // capabilities.surround.
    CompleteStatement, // capabilities.completeStatement.
    LiveTemplates,     // capabilities.liveTemplates.
    Never,             // No correct engine yet (the description names the epic).
};

bool available(Availability availability, const LanguageCapabilities& caps) {
    switch (availability) {
    case Availability::Always: return true;
    case Availability::ClangdTweaks: return caps.clangdTweaks;
    case Availability::OptimizeImports: return caps.optimizeImports;
    case Availability::Surround: return caps.surround;
    case Availability::CompleteStatement: return caps.completeStatement;
    case Availability::LiveTemplates: return caps.liveTemplates;
    case Availability::Never: return false;
    }
    return false;
}

} // namespace

Result<void> LanguageController::registerActions(input::ActionRegistry& registry) {
    struct Entry {
        std::string_view id;
        std::string_view name;
        std::string_view group;
        void (LanguageController::*handler)();
        bool needsEditor;
        Availability availability = Availability::Always;
        std::string_view description = {};
    };
    using A = Availability;
    const Entry entries[] = {
        {"code.completion", "Basic Completion", "Code", &LanguageController::completeCode, true},
        {"code.parameterInfo", "Parameter Info", "Code", &LanguageController::showParameterInfo, true},
        {"code.quickDocumentation", "Quick Documentation", "Code", &LanguageController::showQuickDocumentation, true},
        {"navigate.declaration", "Go to Declaration or Usages", "Navigate", &LanguageController::gotoDeclaration, true},
        {"navigate.implementation", "Go to Implementation", "Navigate", &LanguageController::gotoImplementation, true},
        {"navigate.relatedFiles", "Go to Related Files", "Navigate", &LanguageController::gotoRelatedFile, true},
        {"navigate.findUsages", "Find Usages", "Navigate", &LanguageController::findUsages, true},
        {"navigate.highlightUsages", "Highlight Usages in File", "Navigate", &LanguageController::highlightUsages,
         true},
        {"navigate.fileStructure", "File Structure", "Navigate", &LanguageController::showFileStructure, true},
        {"navigate.gotoClass", "Go to Class...", "Navigate", &LanguageController::gotoClass, false},
        {"navigate.gotoSymbol", "Go to Symbol", "Navigate", &LanguageController::gotoSymbol, false},
        {"navigate.quickDefinition", "Quick Definition", "Navigate", &LanguageController::quickDefinition, true},
        {"navigate.showUsages", "Show Usages", "Navigate", &LanguageController::showUsagesPopup, true},
        {"navigate.callHierarchy", "Call Hierarchy", "Navigate", &LanguageController::callHierarchy, true},
        {"navigate.typeHierarchy", "Type Hierarchy", "Navigate", &LanguageController::typeHierarchy, true},
        {"navigate.superMethod", "Go to Super Method / Base", "Navigate", &LanguageController::gotoSuperMethod, true},
        {"navigate.typeDeclaration", "Go to Type Declaration", "Navigate", &LanguageController::gotoTypeDeclaration,
         true},
        {"navigate.decompileSymbol", "Show Metadata Source", "Navigate", &LanguageController::decompileExternalSymbol,
         true, A::Always, "Shows the metadata source the C# language server returns; the decompiler is LTR-6"},
        {"refactor.rename", "Rename", "Refactor", &LanguageController::rename, true},
        {"refactor.this", "Refactor This...", "Refactor", &LanguageController::refactorThis, true, A::Always,
         "Rename and the refactorings the language server offers at the caret"},
        {"refactor.extractVariable", "Extract Variable...", "Refactor", &LanguageController::extractVariable, true,
         A::ClangdTweaks, "C++: clangd's Extract Variable; other languages require REF-7"},
        {"refactor.extractConstant", "Extract Constant...", "Refactor", &LanguageController::extractConstant, true,
         A::Never, "Requires the extract refactorings (REF-7)"},
        {"refactor.extractField", "Extract Field...", "Refactor", &LanguageController::extractField, true, A::Never,
         "Requires the extract refactorings (REF-7)"},
        {"refactor.extractParameter", "Extract Parameter...", "Refactor", &LanguageController::extractParameter, true,
         A::Never, "Requires the extract refactorings (REF-7)"},
        {"refactor.extractMethod", "Extract Method...", "Refactor", &LanguageController::extractMethod, true,
         A::ClangdTweaks, "C++: clangd's Extract to Function; other languages require REF-7"},
        {"refactor.inline", "Inline...", "Refactor", &LanguageController::inlineSymbol, true, A::Never,
         "Requires the inline refactoring (REF-6)"},
        {"refactor.changeSignature", "Change Signature...", "Refactor", &LanguageController::changeSignature, true,
         A::Never, "Requires the change signature refactoring (REF-6)"},
        {"refactor.safeDelete", "Safe Delete...", "Refactor", &LanguageController::safeDelete, true, A::Never,
         "Requires the safe delete refactoring (REF-8)"},
        {"refactor.move", "Move...", "Refactor", &LanguageController::moveSymbol, true, A::Never,
         "Requires the move refactoring (REF-8)"},
        {"code.showIntentions", "Show Context Actions", "Code", &LanguageController::showIntentions, true},
        {"code.reformat", "Reformat Code", "Code", &LanguageController::reformat, true},
        {"code.generate", "Generate...", "Code", &LanguageController::generateCode, true, A::ClangdTweaks,
         "C++: clangd's constructor, special members and pure virtual overrides; the rest requires REF-9"},
        {"code.overrideMethods", "Override Methods...", "Code", &LanguageController::overrideMethods, true, A::Never,
         "Requires the generate engine (REF-9); Implement Methods overrides pure virtual methods"},
        {"code.implementMethods", "Implement Methods...", "Code", &LanguageController::implementMethods, true,
         A::ClangdTweaks, "C++: clangd's Override pure virtual methods; other languages require REF-9"},
        {"code.surroundWith", "Surround With...", "Code", &LanguageController::surroundWith, true, A::Surround,
         "C++ and C# only; templates for other languages arrive with REF-13"},
        {"code.completeStatement", "Complete Current Statement", "Code", &LanguageController::completeStatement, true,
         A::CompleteStatement, "C++ and C# only"},
        {"code.optimizeImports", "Optimize Imports / Includes", "Code", &LanguageController::optimizeImports, true,
         A::OptimizeImports, "C++: clangd's include cleaner (REF-11); C#: the server's organize imports (LTR-5)"},
        {"code.insertLiveTemplate", "Insert Live Template...", "Code", &LanguageController::insertLiveTemplate, true,
         A::LiveTemplates, "C++ and C# templates; the template engine is UX-6"},
    };
    for (const Entry& entry : entries) {
        input::ActionSpec spec;
        spec.id = std::string(entry.id);
        spec.name = std::string(entry.name);
        spec.group = std::string(entry.group);
        spec.description = std::string(entry.description);
        if (entry.needsEditor) {
            spec.context = input::ContextRule{.all = {input::ContextKey::EditorFocus}, .none = {}};
            spec.enabled = [this, availability = entry.availability](const input::ActionContext&) {
                return hasDocument() && available(availability, capabilities());
            };
        }
        spec.handler = [this, handler = entry.handler](const input::ActionContext&, const input::InvokeInfo&) {
            (this->*handler)();
        };
        if (auto added = registry.add(std::move(spec)); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return {};
}

// --- Helpers ---

bool LanguageController::hasDocument() const {
    return !m_services.host.documentPath().empty() && m_services.host.snapshot().has_value();
}

const LanguageCapabilities& LanguageController::capabilities() const {
    return capabilitiesForPath(m_services.host.documentPath());
}

void LanguageController::unavailable(const QString& reason) { emit statusMessage(reason); }

QString LanguageController::emptyResultMessage(const QString& none) const {
    const std::string path = m_services.host.documentPath();
    const LanguageCapabilities& caps = capabilities();
    const QString server = caps.serverName.empty() ? QStringLiteral("the language server") : qs(caps.serverName);
    ServerReadiness readiness = m_services.readiness ? m_services.readiness(path) : ServerReadiness::Unknown;
    if (!m_services.server.serverCapabilities()) {
        readiness = ServerReadiness::NotRunning;
    }
    switch (readiness) {
    case ServerReadiness::NotRunning: return QStringLiteral("%1: %2 is not running for this file").arg(none, server);
    case ServerReadiness::NoCompileCommand:
        return QStringLiteral("%1: this file has no compile command, so %2 parses it with fallback flags")
            .arg(none, server);
    case ServerReadiness::Indexing:
        return QStringLiteral("%1: %2 is still indexing; try again when indexing finishes").arg(none, server);
    case ServerReadiness::Unknown: break;
    }
    return QStringLiteral("%1 (%2 returned no result)").arg(none, server);
}

void LanguageController::runCodeAction(fl::CodeActionItem action) {
    const QString title = qs(action.title);
    if (!action.edit.empty()) {
        applyWorkspaceEdit(action.edit, title);
    }
    if (!action.commandName.empty()) {
        // clangd.applyTweak answers with a workspace/applyEdit (the server's apply-edit handler).
        executeServerCommand(std::move(action.commandName), std::move(action.commandArguments), title);
    }
}

void LanguageController::runClangdTweak(std::string_view tweakId, const QString& label, const QString& none) {
    if (!hasDocument()) {
        return;
    }
    if (!capabilities().clangdTweaks) {
        unavailable(QStringLiteral("%1 is not available for this language yet").arg(label));
        return;
    }
    const std::string path = m_services.host.documentPath();
    fl::LspRange range{caretPosition(), caretPosition()};
    if (const auto selection = m_services.host.selection()) {
        range = fl::LspRange{positionAt(selection->begin), positionAt(selection->end)};
    }
    start<std::vector<fl::CodeActionItem>>(
        m_actionSlot,
        [&server = m_services.server, path, range](std::stop_token token) {
            return fl::requestCodeActions(server, path, range, {}, std::move(token));
        },
        [this, id = std::string(tweakId), label, none](Result<std::vector<fl::CodeActionItem>> actions) {
            if (!actions) {
                report(actions.error(), label.toStdString());
                return;
            }
            if (const fl::CodeActionItem* tweak = fl::findTweak(*actions, id)) {
                runCodeAction(*tweak);
                return;
            }
            emit statusMessage(none);
        });
}

fl::LspPosition LanguageController::positionAt(ByteOffset offset) const {
    const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
    return snapshot ? fl::toLspPosition(*snapshot, offset, m_services.server.encoding()) : fl::LspPosition{};
}

fl::LspPosition LanguageController::caretPosition() const { return positionAt(m_services.host.caretOffset()); }

std::string LanguageController::lineBeforeCaret() const {
    const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
    if (!snapshot) {
        return {};
    }
    const ByteOffset caret = snapshot->clip(m_services.host.caretOffset());
    auto line = snapshot->lineInfo(snapshot->lineAt(caret));
    if (!line) {
        return {};
    }
    return snapshot->text(text::ByteRange{.begin = line->content.begin, .end = caret});
}

void LanguageController::report(const Error& error, std::string_view what) {
    if (error.code() == ErrorCode::Cancelled) {
        return;
    }
    emit statusMessage(QStringLiteral("%1: %2").arg(qs(what), qs(error.message())));
}

void LanguageController::openTargets(std::vector<fl::Location> locations, const QString& title, const QString& none) {
    if (locations.empty()) {
        emit statusMessage(none);
        return;
    }
    if (locations.size() == 1) {
        m_services.host.openLocation(locations.front().path, locations.front().range.start);
        return;
    }
    std::vector<QString> choices;
    m_choiceActions.clear();
    for (const fl::Location& location : locations) {
        choices.push_back(QStringLiteral("%1:%2").arg(qs(fileName(location.path))).arg(location.range.start.line + 1));
        m_choiceActions.emplace_back(
            [this, location] { m_services.host.openLocation(location.path, location.range.start); });
    }
    m_choice->start(title, choices, m_services.host.caretScreenRect());
}

// --- Completion ---

void LanguageController::completeCode() {
    if (capabilities().postfix && tryExpandPostfixCompletion()) {
        return;
    }
    requestCompletion(fl::CompletionTrigger{});
}

void LanguageController::requestCompletion(fl::CompletionTrigger trigger) {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const std::string line = lineBeforeCaret();
    const ByteOffset caret = m_services.host.caretOffset();
    const std::size_t typed = line.size() - fl::identifierStart(line);
    m_lookupPath = path;
    m_lookupWordStart = ByteOffset{caret.value() - typed};
    const fl::LspPosition position = positionAt(caret);
    start<fl::CompletionList>(
        m_completionSlot,
        [&server = m_services.server, path, position, trigger](std::stop_token token) {
            return fl::requestCompletion(server, path, position, trigger, std::move(token));
        },
        [this, path](Result<fl::CompletionList> list) {
            if (!list) {
                report(list.error(), "Completion");
                return;
            }
            if (!m_lookupWordStart || m_services.host.documentPath() != path) {
                return;
            }
            m_resolveSlot.cancel();
            m_completion->setList(std::move(*list));
            updateLookupPrefix();
        });
}

void LanguageController::updateLookupPrefix() {
    if (!m_lookupWordStart) {
        return;
    }
    const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
    const ByteOffset caret = m_services.host.caretOffset();
    if (!snapshot || m_services.host.documentPath() != m_lookupPath || caret < *m_lookupWordStart) {
        closeLookup();
        return;
    }
    const std::string prefix = snapshot->text(text::ByteRange{.begin = *m_lookupWordStart, .end = caret});
    if (!prefix.empty() && !allIdentifierBytes(prefix)) {
        closeLookup();
        return;
    }
    m_completion->setPrefix(prefix);
    if (m_completion->model().empty()) {
        m_completion->hide(); // Nothing matches; typing on may match again or close the lookup.
        return;
    }
    m_completion->showAt(m_services.host.screenRectAt(*m_lookupWordStart));
}

void LanguageController::closeLookup() {
    m_completionSlot.cancel();
    m_resolveSlot.cancel();
    m_lookupWordStart.reset();
    m_completion->hide();
}

void LanguageController::commitCompletion(std::size_t itemIndex, bool replaceWord) {
    const auto items = m_completion->model().items();
    const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
    if (itemIndex >= items.size() || !snapshot || !m_lookupWordStart) {
        closeLookup();
        return;
    }
    const fl::CompletionItem item = items[itemIndex];
    const ByteOffset wordStart = *m_lookupWordStart;
    ByteOffset end = snapshot->clip(m_services.host.caretOffset());
    if (replaceWord) {
        // Tab replaces the rest of the identifier after the caret, as in Rider.
        while (end < snapshot->size() && fl::isIdentifierByte(snapshot->byteAt(end))) {
            end = ByteOffset{end.value() + 1};
        }
    }
    closeLookup();
    auto commit = fl::commitCompletion(item, *snapshot, wordStart, end, m_services.server.encoding());
    if (!commit) {
        report(commit.error(), "Completion");
        return;
    }
    if (auto applied = m_services.host.applyEdits(commit->edits, "Complete Code", commit->caret); !applied) {
        report(applied.error(), "Completion");
        return;
    }
    // LuaLS and other servers use this editor-local command to request signature help after
    // inserting a call snippet. Do not execute arbitrary commands supplied by a server.
    const fl::Json* command = item.raw.find("command");
    const bool triggerParameterHints = command && command->stringAt("command") == "editor.action.triggerParameterHints";
    m_tabStops = std::move(commit->tabStops);
    if (!m_tabStops.empty()) {
        const std::optional<text::Snapshot> after = m_services.host.snapshot();
        m_tabStopsDocumentSize = after ? after->size().value() : 0;
        // Only an empty final stop: the caret is already there.
        if (m_tabStops.size() == 1 && m_tabStops.front().empty()) {
            m_tabStops.clear();
        } else {
            static_cast<void>(nextTabStop());
        }
    }
    if (triggerParameterHints)
        requestSignatureHelp(std::nullopt);
}

bool LanguageController::nextTabStop() {
    const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
    if (m_tabStops.empty() || !snapshot) {
        m_tabStops.clear();
        return false;
    }
    // Typing inside the current placeholder shifts every later stop by the size change.
    const std::uint64_t size = snapshot->size().value();
    const auto shift = [&](ByteOffset offset) {
        const std::int64_t moved =
            static_cast<std::int64_t>(offset.value()) +
            (static_cast<std::int64_t>(size) - static_cast<std::int64_t>(m_tabStopsDocumentSize));
        return snapshot->clip(ByteOffset{static_cast<std::uint64_t>(std::max<std::int64_t>(0, moved))});
    };
    for (text::ByteRange& stop : m_tabStops) {
        stop = text::ByteRange{.begin = shift(stop.begin), .end = shift(stop.end)};
    }
    m_tabStopsDocumentSize = size;
    const text::ByteRange next = m_tabStops.front();
    m_tabStops.erase(m_tabStops.begin());
    m_services.host.select(next);
    return true;
}

void LanguageController::resolveSelected(std::size_t itemIndex) {
    const auto capabilities = m_services.server.serverCapabilities();
    const auto items = m_completion->model().items();
    if (!capabilities || !capabilities->completionResolve || itemIndex >= items.size() || items[itemIndex].resolved) {
        return;
    }
    start<fl::CompletionItem>(
        m_resolveSlot,
        [&server = m_services.server, item = items[itemIndex]](std::stop_token token) {
            return fl::resolveCompletionItem(server, item, std::move(token));
        },
        [this, itemIndex](Result<fl::CompletionItem> resolved) {
            if (resolved && m_lookupWordStart && itemIndex < m_completion->model().items().size()) {
                m_completion->replaceItem(itemIndex, std::move(*resolved));
            }
        });
}

// --- Editor hooks ---

bool LanguageController::keyPress(int key, Qt::KeyboardModifiers modifiers) {
    if (m_completion->isVisible() && m_completion->handleKey(key, modifiers)) {
        return true;
    }
    const bool plain =
        (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier)) == 0;
    if (key == Qt::Key_Escape) {
        bool consumed = false;
        for (QWidget* popup : {static_cast<QWidget*>(m_parameterInfo.get()), static_cast<QWidget*>(m_quickDoc.get()),
                               static_cast<QWidget*>(m_quickDef.get()), static_cast<QWidget*>(m_hierarchy.get())}) {
            if (popup->isVisible()) {
                popup->hide();
                consumed = true;
            }
        }
        if (!m_tabStops.empty()) {
            m_tabStops.clear();
            consumed = true;
        }
        return consumed;
    }
    if (key == Qt::Key_Tab && plain && !m_tabStops.empty()) {
        return nextTabStop();
    }
    if (key == Qt::Key_Tab && plain && hasDocument() && capabilities().postfix && tryExpandPostfixCompletion()) {
        return true;
    }
    return false;
}

void LanguageController::textTyped(std::string_view text) {
    if (!hasDocument() || text.empty()) {
        return;
    }
    if (m_lookupWordStart) {
        if (allIdentifierBytes(text)) {
            if (m_completion->model().isIncomplete()) {
                requestCompletion(fl::CompletionTrigger{
                    .kind = fl::CompletionTriggerKind::TriggerForIncompleteCompletions, .character = {}});
            } else {
                updateLookupPrefix();
            }
            return;
        }
        closeLookup();
    }
    if (auto trigger = fl::autoCompletionTrigger(lineBeforeCaret())) {
        requestCompletion(*trigger);
    }
    const char last = text.back();
    if (fl::isSignatureHelpTrigger(last)) {
        requestSignatureHelp(last);
    } else if (last == ')' && m_parameterInfo->isVisible()) {
        requestSignatureHelp(std::nullopt);
    }
}

void LanguageController::caretMoved() {
    if (m_lookupWordStart) {
        const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
        const ByteOffset caret = m_services.host.caretOffset();
        bool inside = snapshot && m_services.host.documentPath() == m_lookupPath && caret >= *m_lookupWordStart;
        if (inside) {
            const std::string prefix = snapshot->text(text::ByteRange{.begin = *m_lookupWordStart, .end = caret});
            inside = prefix.empty() || allIdentifierBytes(prefix);
        }
        if (!inside) {
            closeLookup();
        }
    }
    if (m_quickDoc->isVisible()) {
        m_quickDoc->hide();
    }
    if (m_parameterInfo->isVisible()) {
        requestSignatureHelp(std::nullopt); // The server answers null once the caret leaves the call.
    }
    if (m_structureVisible && hasDocument()) {
        m_structure->highlightLine(static_cast<int>(caretPosition().line));
    }
    m_highlightTimer.start();
}

void LanguageController::setHighlightDelay(std::chrono::milliseconds delay) {
    m_highlightTimer.setInterval(static_cast<int>(delay.count()));
}

// --- Parameter info and quick documentation ---

void LanguageController::showParameterInfo() { requestSignatureHelp(std::nullopt); }

void LanguageController::requestSignatureHelp(std::optional<char> trigger) {
    if (!hasDocument()) {
        return;
    }
    const bool retrigger = m_parameterInfo->isVisible();
    start<std::optional<fl::SignatureHelp>>(
        m_signatureSlot,
        [&server = m_services.server, path = m_services.host.documentPath(), position = caretPosition(), trigger,
         retrigger](std::stop_token token) {
            return fl::requestSignatureHelp(server, path, position, trigger, retrigger, std::move(token));
        },
        [this, explicitRequest = !trigger && !retrigger](Result<std::optional<fl::SignatureHelp>> help) {
            if (!help) {
                report(help.error(), "Parameter info");
                return;
            }
            if (!*help) {
                m_parameterInfo->hide();
                if (explicitRequest) {
                    emit statusMessage(QStringLiteral("No parameter info here"));
                }
                return;
            }
            m_parameterInfo->setHelp(**help);
            m_parameterInfo->showAt(m_services.host.caretScreenRect(), /*above=*/true);
        });
}

void LanguageController::showQuickDocumentation() {
    if (!hasDocument()) {
        return;
    }
    start<std::optional<fl::HoverInfo>>(
        m_hoverSlot,
        [&server = m_services.server, path = m_services.host.documentPath(), position = caretPosition()](
            std::stop_token token) { return fl::requestHover(server, path, position, std::move(token)); },
        [this](Result<std::optional<fl::HoverInfo>> hover) {
            if (!hover) {
                report(hover.error(), "Quick documentation");
                return;
            }
            if (!*hover) {
                emit statusMessage(QStringLiteral("No documentation found"));
                return;
            }
            m_quickDoc->setMarkup((*hover)->contents);
            m_quickDoc->showAt(m_services.host.caretScreenRect());
        });
}

// --- Navigation ---

void LanguageController::gotoDeclaration() {
    if (hasDocument()) {
        gotoDeclarationAtPosition(caretPosition());
    }
}

void LanguageController::gotoDeclarationAt(ByteOffset offset) {
    if (hasDocument()) {
        gotoDeclarationAtPosition(positionAt(offset));
    }
}

void LanguageController::gotoDeclarationAtPosition(fl::LspPosition position) {
    const std::string path = m_services.host.documentPath();
    const std::string symbol = identifierAtPosition(m_services.host.snapshot(), position, m_services.server.encoding());
    // Only C# servers answer with metadata URIs; C++ declarations come from clangd alone.
    const bool askMetadata = capabilities().languageId == "csharp";
    start<GotoOutcome>(
        m_navigationSlot,
        [&server = m_services.server, &fs = m_services.fs, cacheDir = m_services.metadataCacheDir, askMetadata, path,
         position](std::stop_token token) {
            return gotoDeclarationTask(server, fs, cacheDir, askMetadata, path, position, std::move(token));
        },
        [this, path, position, symbol](Result<GotoOutcome> outcome) {
            if (!outcome) {
                report(outcome.error(), "Go to declaration");
                return;
            }
            if (outcome->result.showUsages) {
                findUsagesAt(path, position);
                return;
            }
            if (outcome->metadata) {
                openMetadataLocation(outcome->result.locations.front(), qs(symbol));
                return;
            }
            openTargets(std::move(outcome->result.locations), QStringLiteral("Choose Declaration"),
                        emptyResultMessage(QStringLiteral("Cannot find a declaration to go to")));
        });
}

void LanguageController::openMetadataLocation(const fl::Location& location, const QString& symbol) {
    emit statusMessage(QStringLiteral("Showing metadata source for '%1' from %2 (read-only cache)")
                           .arg(symbol, qs(capabilities().serverName)));
    m_services.host.openReadOnlyLocation(location.path, location.range.start);
}

void LanguageController::decompileExternalSymbol() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const fl::LspPosition position = caretPosition();
    const std::string symbol = identifierAtPosition(m_services.host.snapshot(), position, m_services.server.encoding());
    if (symbol.empty()) {
        emit statusMessage(QStringLiteral("Place the caret on a symbol to show its metadata source"));
        return;
    }
    const auto& caps = capabilities();
    if (caps.languageId != "csharp") {
        emit statusMessage(
            QStringLiteral("Source not available for '%1': %2 provides no metadata or decompiled "
                           "source (library sources and decompiler: LTR-6)")
                .arg(qs(symbol), caps.serverName.empty() ? QStringLiteral("no language server") : qs(caps.serverName)));
        return;
    }
    if (m_services.metadataCacheDir.empty()) {
        emit statusMessage(
            QStringLiteral("Source not available for '%1': no metadata cache is configured (LTR-6)").arg(qs(symbol)));
        return;
    }
    start<std::optional<fl::Location>>(
        m_navigationSlot,
        [&server = m_services.server, &fs = m_services.fs, cacheDir = m_services.metadataCacheDir, path,
         position](std::stop_token token) {
            return metadataSourceTask(server, fs, cacheDir, path, position, std::move(token));
        },
        [this, symbol](Result<std::optional<fl::Location>> location) {
            if (!location) {
                report(location.error(), "Metadata source");
                return;
            }
            if (!*location) {
                emit statusMessage(
                    QStringLiteral("Source not available for '%1': %2 returned no metadata source (LTR-6)")
                        .arg(qs(symbol), qs(capabilities().serverName)));
                return;
            }
            openMetadataLocation(**location, qs(symbol));
        });
}

void LanguageController::gotoImplementation() {
    if (!hasDocument()) {
        return;
    }
    start<std::vector<fl::Location>>(
        m_navigationSlot,
        [&server = m_services.server, path = m_services.host.documentPath(), position = caretPosition()](
            std::stop_token token) { return fl::gotoImplementation(server, path, position, std::move(token)); },
        [this](Result<std::vector<fl::Location>> locations) {
            if (!locations) {
                report(locations.error(), "Go to implementation");
                return;
            }
            openTargets(std::move(*locations), QStringLiteral("Choose Implementation"),
                        emptyResultMessage(QStringLiteral("No implementations found")));
        });
}

void LanguageController::gotoRelatedFile() {
    if (!hasDocument()) {
        return;
    }
    const std::string currentPath = m_services.host.documentPath();
    start<std::optional<std::string>>(
        m_navigationSlot,
        [&server = m_services.server, path = currentPath](std::stop_token token) {
            return fl::requestSwitchSourceHeader(server, path, std::move(token));
        },
        [this, currentPath](Result<std::optional<std::string>> related) {
            if (related && *related && !(*related)->empty()) {
                m_services.host.openLocation(**related, fl::LspPosition{});
                return;
            }
            if (const auto fallback = findRelatedProjectFile(currentPath, m_services.projectFiles, m_services.fs)) {
                m_services.host.openLocation(*fallback, fl::LspPosition{});
                return;
            }
            if (!related) {
                report(related.error(), "Go to related files");
                return;
            }
            emit statusMessage(QStringLiteral("No related file found"));
        });
}

void LanguageController::findUsages() {
    if (hasDocument()) {
        findUsagesAt(m_services.host.documentPath(), caretPosition());
    }
}

void LanguageController::findUsagesAt(std::string path, fl::LspPosition position) {
    // The symbol's name for the panel title: the identifier around the caret.
    QString name;
    if (const std::optional<text::Snapshot> snapshot = m_services.host.snapshot()) {
        ByteOffset begin = snapshot->clip(m_services.host.caretOffset());
        ByteOffset end = begin;
        while (begin.value() > 0 && fl::isIdentifierByte(snapshot->byteAt(ByteOffset{begin.value() - 1}))) {
            begin = ByteOffset{begin.value() - 1};
        }
        while (end < snapshot->size() && fl::isIdentifierByte(snapshot->byteAt(end))) {
            end = ByteOffset{end.value() + 1};
        }
        name = qs(snapshot->text(text::ByteRange{.begin = begin, .end = end}));
    }
    start<std::vector<fl::UsageGroup>>(
        m_usagesSlot,
        [&server = m_services.server, &fs = m_services.fs, open = m_services.host.openDocuments(), path, position,
         encoding = m_services.server.encoding()](std::stop_token token) {
            return detail::runFindUsagesTask(server, fs, open, path, position, encoding, std::move(token));
        },
        [this, name](Result<std::vector<fl::UsageGroup>> groups) {
            if (!groups) {
                report(groups.error(), "Find usages");
                return;
            }
            m_usages->setUsages(name.isEmpty() ? QStringLiteral("Usages") : QStringLiteral("Usages of '%1'").arg(name),
                                std::move(*groups));
            if (m_usages->usageCount() == 0) {
                emit statusMessage(QStringLiteral("No usages found"));
            }
            emit usagesReady();
        });
}

void LanguageController::highlightUsages() {
    m_highlightTimer.stop();
    const auto capabilities = m_services.server.serverCapabilities();
    if (!hasDocument() || !capabilities || !capabilities->documentHighlight) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const std::int64_t version = m_services.host.documentVersion();
    start<std::vector<fl::DocumentHighlight>>(
        m_highlightSlot,
        [&server = m_services.server, path, position = caretPosition()](std::stop_token token) {
            return fl::requestDocumentHighlights(server, path, position, std::move(token));
        },
        [this, path, version](Result<std::vector<fl::DocumentHighlight>> highlights) {
            // Highlights follow the caret silently: a failure only leaves the old ones cleared.
            if (m_services.host.documentPath() != path || m_services.host.documentVersion() != version) {
                return;
            }
            m_services.host.setUsageHighlights(highlights ? std::move(*highlights)
                                                          : std::vector<fl::DocumentHighlight>{});
        });
}

void LanguageController::showFileStructure() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    start<std::vector<fl::DocumentSymbol>>(
        m_symbolSlot,
        [&server = m_services.server, path](std::stop_token token) {
            return fl::requestDocumentSymbols(server, path, std::move(token));
        },
        [this, path](Result<std::vector<fl::DocumentSymbol>> symbols) {
            if (!symbols) {
                report(symbols.error(), "File structure");
                return;
            }
            // Structure mode first, so clearing the query filters locally instead of searching.
            m_symbolQueryTimer.stop();
            m_symbols->setStructure(QStringLiteral("Structure of %1").arg(qs(fileName(path))), path,
                                    std::move(*symbols));
            m_symbols->setQuery(QString());
            m_symbols->popup(m_services.host.editorWidget());
        });
}

void LanguageController::gotoClass() {
    m_symbolClassesOnly = true;
    m_symbolSlot.cancel();
    m_symbols->setWorkspaceSymbols(QStringLiteral("Go to Class"), {});
    m_symbols->setQuery(QString());
    m_symbolQueryTimer.stop();
    m_symbols->popup(m_services.host.editorWidget());
}

void LanguageController::gotoSymbol() {
    m_symbolClassesOnly = false;
    m_symbolSlot.cancel();
    m_symbols->setWorkspaceSymbols(QStringLiteral("Go to Symbol"), {});
    m_symbols->setQuery(QString());
    m_symbolQueryTimer.stop();
    m_symbols->popup(m_services.host.editorWidget());
}

void LanguageController::requestWorkspaceSymbols(const QString& query) {
    if (m_symbols->structureMode()) {
        return;
    }
    const QString title = m_symbolClassesOnly ? QStringLiteral("Go to Class") : QStringLiteral("Go to Symbol");
    if (query.isEmpty()) {
        m_symbolSlot.cancel();
        m_symbols->setWorkspaceSymbols(title, {});
        return;
    }
    const bool classesOnly = m_symbolClassesOnly;
    start<std::vector<fl::WorkspaceSymbol>>(
        m_symbolSlot,
        [&server = m_services.server, query = query.toStdString()](std::stop_token token) {
            return fl::requestWorkspaceSymbols(server, query, std::move(token));
        },
        [this, classesOnly, title](Result<std::vector<fl::WorkspaceSymbol>> symbols) {
            if (!symbols) {
                report(symbols.error(), classesOnly ? "Go to class" : "Go to symbol");
                return;
            }
            if (m_symbols->isVisible() && !m_symbols->structureMode()) {
                std::vector<fl::WorkspaceSymbol> filtered = std::move(*symbols);
                if (classesOnly) {
                    std::erase_if(filtered, [](const fl::WorkspaceSymbol& s) { return !isClassLikeSymbol(s.kind); });
                }
                m_symbols->setWorkspaceSymbols(title, std::move(filtered));
            }
        });
}

// --- Rename, quick fixes, reformat ---

void LanguageController::rename() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const fl::LspPosition position = caretPosition();
    start<std::optional<fl::PrepareRenameResult>>(
        m_renameSlot,
        [&server = m_services.server, path, position](std::stop_token token) {
            return fl::requestPrepareRename(server, path, position, std::move(token));
        },
        [this, path, position](Result<std::optional<fl::PrepareRenameResult>> prepared) {
            if (!prepared) {
                report(prepared.error(), "Cannot rename");
                return;
            }
            if (!*prepared) {
                emit statusMessage(QStringLiteral("This element cannot be renamed"));
                return;
            }
            QString current = qs((*prepared)->placeholder);
            const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
            if (current.isEmpty() && snapshot) {
                const fl::PositionEncoding encoding = m_services.server.encoding();
                auto begin = fl::fromLspPosition(*snapshot, (*prepared)->range.start, encoding);
                auto end = fl::fromLspPosition(*snapshot, (*prepared)->range.end, encoding);
                if (begin && end && *begin <= *end) {
                    current = qs(snapshot->text(text::ByteRange{.begin = *begin, .end = *end}));
                }
            }
            m_renamePath = path;
            m_renameOldName = current.toStdString();
            m_renamePosition = position;
            m_rename->start(current, m_services.host.caretScreenRect());
        });
}

void LanguageController::applyWorkspaceEdit(const fl::WorkspaceEdit& edit, const QString& label) {
    if (auto applied = m_services.applier.apply(edit, label.toStdString()); !applied) {
        emit statusMessage(QStringLiteral("%1: %2").arg(label, qs(applied.error().message())));
    }
}

void LanguageController::executeServerCommand(std::string commandName, std::vector<fl::Json> arguments,
                                              const QString& label) {
    if (commandName.empty()) {
        return;
    }
    start<fl::Json>(
        m_actionSlot,
        [&server = m_services.server, cmd = std::move(commandName), args = std::move(arguments)](
            std::stop_token token) { return fl::requestExecuteCommand(server, cmd, args, std::move(token)); },
        [this, label](Result<fl::Json> result) {
            if (!result) {
                report(result.error(), label.toStdString());
            }
        });
}

void LanguageController::insertSnippetAt(text::ByteRange replaceRange, std::string_view rawSnippet,
                                         std::string_view undoLabel) {
    const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
    if (!snapshot) {
        return;
    }
    const fl::ParsedSnippet parsed = fl::parseSnippet(rawSnippet);
    const ByteOffset base = replaceRange.begin;
    const fl::ByteEdit edit{.range = replaceRange, .text = parsed.text};
    const ByteOffset caret{base.value() + parsed.caret()};
    if (auto applied = m_services.host.applyEdits(std::span<const fl::ByteEdit>(&edit, 1), undoLabel, caret);
        !applied) {
        report(applied.error(), undoLabel);
        return;
    }
    m_tabStops.clear();
    for (const fl::SnippetTabStop& stop : parsed.tabStops) {
        m_tabStops.push_back(text::ByteRange{.begin = ByteOffset{base.value() + stop.begin},
                                             .end = ByteOffset{base.value() + stop.end}});
    }
    if (!m_tabStops.empty()) {
        const std::optional<text::Snapshot> after = m_services.host.snapshot();
        m_tabStopsDocumentSize = after ? after->size().value() : 0;
        if (m_tabStops.size() == 1 && m_tabStops.front().empty()) {
            m_tabStops.clear();
        } else {
            static_cast<void>(nextTabStop());
        }
    }
}

void LanguageController::showIntentions() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    fl::LspRange range{caretPosition(), caretPosition()};
    if (const auto selection = m_services.host.selection()) {
        range = fl::LspRange{positionAt(selection->begin), positionAt(selection->end)};
    }
    std::vector<fl::LiveDiagnostic> all;
    if (m_services.diagnostics) {
        all = m_services.diagnostics(path);
    }
    std::vector<fl::LiveDiagnostic> here = fl::diagnosticsAt(all, range);
    if (here.empty()) {
        // As in Rider, Alt+Enter anywhere on a line offers the fixes of that line's problems.
        here = fl::diagnosticsAt(all, fl::LspRange{{range.start.line, 0}, {range.start.line, UINT32_MAX}});
    }
    start<std::vector<fl::CodeActionItem>>(
        m_actionSlot,
        [&server = m_services.server, path, range, here](std::stop_token token) {
            return fl::requestCodeActions(server, path, range, here, std::move(token));
        },
        [this](Result<std::vector<fl::CodeActionItem>> actions) {
            std::vector<QString> titles;
            std::vector<bool> preferred;
            m_choiceActions.clear();
            if (actions) {
                for (fl::CodeActionItem& action : *actions) {
                    titles.push_back(qs(action.title));
                    preferred.push_back(action.preferred);
                    m_choiceActions.emplace_back([this, title = qs(action.title), edit = std::move(action.edit),
                                                  cmd = std::move(action.commandName),
                                                  args = std::move(action.commandArguments)]() mutable {
                        if (!edit.empty()) {
                            applyWorkspaceEdit(edit, title);
                        }
                        if (!cmd.empty()) {
                            executeServerCommand(std::move(cmd), std::move(args), title);
                        }
                    });
                }
            }
            const auto addContextAction = [&](QString title, std::function<void()> run) {
                titles.push_back(std::move(title));
                preferred.push_back(false);
                m_choiceActions.push_back(std::move(run));
            };
            addContextAction(QStringLiteral("Go to Declaration or Usages"), [this] { gotoDeclaration(); });
            addContextAction(QStringLiteral("Go to Implementation"), [this] { gotoImplementation(); });
            addContextAction(QStringLiteral("Find Usages"), [this] { findUsages(); });
            addContextAction(QStringLiteral("Highlight Usages in File"), [this] { highlightUsages(); });
            addContextAction(QStringLiteral("Go to Related Files"), [this] { gotoRelatedFile(); });
            addContextAction(QStringLiteral("Rename..."), [this] { rename(); });
            addContextAction(QStringLiteral("Quick Documentation"), [this] { showQuickDocumentation(); });
            addContextAction(QStringLiteral("File Structure"), [this] { showFileStructure(); });
            addContextAction(QStringLiteral("Reformat Code"), [this] { reformat(); });
            m_choice->start(QStringLiteral("Context Actions"), titles, m_services.host.caretScreenRect(),
                            std::move(preferred));
        });
}

void LanguageController::reformat() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    m_formatVersion = m_services.host.documentVersion();
    std::optional<fl::LspRange> range;
    if (const auto selection = m_services.host.selection()) {
        range = fl::LspRange{positionAt(selection->begin), positionAt(selection->end)};
    }
    const FormattingSettings settings = m_services.host.formattingOptions();
    const fl::FormattingOptions options{.tabSize = settings.insertSpaces ? settings.indentSize : settings.tabSize,
                                        .insertSpaces = settings.insertSpaces};
    start<std::vector<fl::TextEdit>>(
        m_formatSlot,
        [&server = m_services.server, path, range, options](std::stop_token token) {
            return fl::requestFormatting(server, path, range, options, std::move(token));
        },
        [this, path, settings](Result<std::vector<fl::TextEdit>> edits) {
            if (!edits) {
                report(edits.error(), "Reformat code");
                return;
            }
            const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
            const auto currentSettings = m_services.host.formattingOptions();
            if (!snapshot || m_services.host.documentPath() != path ||
                m_services.host.documentVersion() != m_formatVersion || currentSettings != settings) {
                emit statusMessage(QStringLiteral("The file or formatting settings changed; try again"));
                return;
            }
            auto bytes = fl::toByteEdits(*snapshot, *edits, m_services.server.encoding());
            if (!bytes) {
                report(bytes.error(), "Reformat code");
                return;
            }
            if (bytes->empty()) {
                emit statusMessage(QStringLiteral("The code is already formatted"));
                return;
            }
            const ByteOffset caret = fl::mapThroughEdits(m_services.host.caretOffset(), *bytes);
            if (auto applied = m_services.host.applyEdits(*bytes, "Reformat Code", caret); !applied) {
                report(applied.error(), "Reformat code");
            }
        });
}

// --- Widgets ---

CompletionPopup& LanguageController::completionPopup() { return *m_completion; }
ParameterInfoPopup& LanguageController::parameterInfoPopup() { return *m_parameterInfo; }
QuickDocPopup& LanguageController::quickDocPopup() { return *m_quickDoc; }
FindUsagesPanel& LanguageController::usagesPanel() { return *m_usages; }
SymbolPopup& LanguageController::symbolPopup() { return *m_symbols; }
RenameInput& LanguageController::renameInput() { return *m_rename; }
ChoicePopup& LanguageController::choicePopup() { return *m_choice; }
QuickDefinitionPopup& LanguageController::quickDefinitionPopup() { return *m_quickDef; }
HierarchyPopup& LanguageController::hierarchyPopup() { return *m_hierarchy; }

} // namespace frost::ui::lang
