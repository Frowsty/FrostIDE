// SPDX-License-Identifier: Apache-2.0
//
// LanguageController (frost/ui/lang/language_controller.hpp): construction, the service hooks, the
// active editor and the structure view, the action registry and the shared request helpers. The
// features live beside it (FND-12a): completion, the editor hooks, parameter info and quick
// documentation in language_controller_completion.cpp; navigation in
// language_controller_navigation.cpp; rename, quick fixes and reformat in
// language_controller_editing.cpp; code generation, hierarchies and refactorings in
// language_controller_codegen.cpp, language_controller_hierarchy.cpp and
// language_controller_refactor.cpp.

#include "frost/ui/lang/language_controller.hpp"

#include "frost/io/file_system.hpp"
#include "frost/lang/clangd_actions.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/symbols.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"

#include "language_controller_internal.hpp"
#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <cstdint>
#include <utility>

namespace frost::ui::lang {

namespace fl = ::frost::lang;

namespace {

using detail::fileName;
using detail::lowerAscii;
using detail::qs;

} // namespace

LanguageController::LanguageController(LanguageServices services, QObject* parent)
    : QObject(parent), m_services(std::move(services)), m_completion(std::make_unique<CompletionPopup>()),
      m_parameterInfo(std::make_unique<ParameterInfoPopup>()), m_quickDoc(std::make_unique<QuickDocPopup>()),
      m_usages(std::make_unique<FindUsagesPanel>()), m_symbols(std::make_unique<SymbolPopup>()),
      m_structure(std::make_unique<StructurePanel>()), m_rename(std::make_unique<RenameInput>()),
      m_choice(std::make_unique<ChoicePopup>()), m_quickDef(std::make_unique<QuickDefinitionPopup>()),
      m_hierarchy(std::make_unique<HierarchyPopup>()), m_ast(std::make_unique<AstViewPopup>()) {
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
    connect(m_ast.get(), &AstViewPopup::rangeActivated, this, [this](fl::LspRange range) {
        const std::optional<text::Snapshot> snapshot = m_services.host.snapshot();
        if (!snapshot) {
            return;
        }
        const Result<ByteOffset> begin = fl::fromLspPosition(*snapshot, range.start, m_services.server.encoding());
        const Result<ByteOffset> end = fl::fromLspPosition(*snapshot, range.end, m_services.server.encoding());
        if (begin && end && *begin <= *end) {
            m_services.host.select(text::ByteRange{*begin, *end});
        }
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
          &m_highlightSlot, &m_symbolSlot, &m_renameSlot, &m_actionSlot, &m_formatSlot, &m_hierarchySlot, &m_astSlot}) {
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
          &m_highlightSlot, &m_symbolSlot, &m_renameSlot, &m_actionSlot, &m_formatSlot, &m_hierarchySlot, &m_astSlot}) {
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
    m_ast->hide();
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
        {"code.smartCompletion", "Smart Type-Matching Completion", "Code", &LanguageController::smartCompleteCode, true,
         A::Never, "Requires type-matching completion (CPP-14)"},
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
        {"refactor.copy", "Copy...", "Refactor", &LanguageController::copySymbol, true, A::Never,
         "Requires the copy refactoring (REF-8)"},
        {"code.showIntentions", "Show Context Actions", "Code", &LanguageController::showIntentions, true},
        {"code.reformat", "Reformat Code", "Code", &LanguageController::reformat, true},
        {"code.reformatFile", "Reformat File", "Code", &LanguageController::reformatFile, true},
        {"code.showAst", "Show AST (clangd)", "Code", &LanguageController::showAst, true, A::ClangdTweaks,
         "C and C++: clangd's AST node for the caret or the selection"},
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
AstViewPopup& LanguageController::astPopup() { return *m_ast; }

} // namespace frost::ui::lang
