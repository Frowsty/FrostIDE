// SPDX-License-Identifier: Apache-2.0
//
// LanguageController (frost/ui/lang/language_controller.hpp): the language features of the
// active editor, from the action handlers and typing hooks to frost::lang's request models and
// back to the widgets on the UI thread.

#include "frost/ui/lang/language_controller.hpp"

#include "frost/io/file_system.hpp"
#include "frost/lang/code_assist.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/symbols.hpp"
#include "frost/lang/uri.hpp"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>

namespace frost::ui::lang {

namespace fl = ::frost::lang;

namespace {

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

std::string fileName(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

bool allIdentifierBytes(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) { return fl::isIdentifierByte(c); });
}

// Awaits `task` on the scheduler and posts `done(result)` to the UI thread, where it runs only
// while the controller is alive and `ticket` is still the slot's latest.
template<class T>
sched::Task<void> deliver(sched::Task<Result<T>> task, QPointer<LanguageController> self, sched::LatestWins* slot,
                          sched::LatestWins::Ticket ticket, std::function<void(Result<T>)> done) {
    auto result = std::make_shared<Result<T>>(co_await std::move(task));
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) {
        co_return;
    }
    QMetaObject::invokeMethod(
        app,
        [self, slot, ticket, done = std::move(done), result]() {
            if (!self || !slot->isCurrent(ticket)) {
                return;
            }
            done(std::move(*result));
            if (self) {
                emit self->requestFinished();
            }
        },
        Qt::QueuedConnection);
}

// Find usages off the UI thread: the references, then their line previews from the open
// documents (`open`) or the disk.
sched::Task<Result<std::vector<fl::UsageGroup>>>
findUsagesTask(fl::ILspRequester& server, io::IFileSystem& fs, std::vector<std::pair<std::string, text::Snapshot>> open,
               std::string path, fl::LspPosition position, fl::PositionEncoding encoding, std::stop_token token) {
    auto references = co_await fl::requestReferences(server, path, position, true, token);
    if (!references) {
        co_return std::unexpected(std::move(references.error()));
    }
    std::map<std::string, text::Snapshot, std::less<>> byKey;
    for (auto& [openPath, snapshot] : open) {
        byKey.insert_or_assign(fl::documentKey(openPath), std::move(snapshot));
    }
    fl::FileLineSource lines(fs, [&byKey](std::string_view file) -> std::optional<text::Snapshot> {
        auto it = byKey.find(fl::documentKey(file));
        return it != byKey.end() ? std::optional<text::Snapshot>(it->second) : std::nullopt;
    });
    co_return fl::groupUsages(*references, lines.source(), encoding);
}

} // namespace

template<class T>
void LanguageController::start(sched::LatestWins& slot, std::function<sched::Task<Result<T>>(std::stop_token)> make,
                               std::function<void(Result<T>)> done) {
    const sched::LatestWins::Ticket ticket = slot.next();
    sched::Task<Result<T>> task = make(ticket.token);
    m_services.scheduler
        .spawn(sched::Lane::Interactive,
               deliver<T>(std::move(task), QPointer<LanguageController>(this), &slot, ticket, std::move(done)))
        .detach();
}

LanguageController::LanguageController(LanguageServices services, QObject* parent)
    : QObject(parent), m_services(std::move(services)), m_completion(std::make_unique<CompletionPopup>()),
      m_parameterInfo(std::make_unique<ParameterInfoPopup>()), m_quickDoc(std::make_unique<QuickDocPopup>()),
      m_usages(std::make_unique<FindUsagesPanel>()), m_symbols(std::make_unique<SymbolPopup>()),
      m_structure(std::make_unique<StructurePanel>()), m_rename(std::make_unique<RenameInput>()),
      m_choice(std::make_unique<ChoicePopup>()) {
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
    connect(m_structure.get(), &StructurePanel::activated, this, [this](const QString& path, fl::LspPosition position) {
        const auto snapshot = m_services.host.snapshot();
        if (m_structureVisible && !m_structurePath.empty() && path.toStdString() == m_structurePath &&
            m_services.host.documentPath() == m_structurePath &&
            m_services.host.documentVersion() == m_structureVersion && snapshot &&
            snapshot->version() == m_structureRevision && m_services.host.structureIdentity() == m_structureIdentity &&
            m_services.server.encoding() == m_structureEncoding) {
            // Opening can synchronously change editor state and invalidate m_structurePath.
            const std::string targetPath = m_structurePath;
            m_services.host.openLocation(targetPath, position);
        }
    });

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
    connect(m_choice.get(), &ChoicePopup::chosen, this, [this](std::size_t index) {
        std::vector<std::function<void()>> actions = std::move(m_choiceActions);
        m_choiceActions.clear();
        if (index < actions.size() && actions[index]) {
            actions[index]();
        }
    });
    connect(m_choice.get(), &ChoicePopup::cancelled, this, [this] { m_choiceActions.clear(); });
    connect(m_rename.get(), &RenameInput::accepted, this, [this](const QString& name) {
        if (name.isEmpty() || m_renamePath.empty()) {
            return;
        }
        start<fl::WorkspaceEdit>(
            m_renameSlot,
            [&server = m_services.server, path = m_renamePath, position = m_renamePosition,
             newName = name.toStdString()](std::stop_token token) {
                return fl::requestRename(server, path, position, newName, std::move(token));
            },
            [this](Result<fl::WorkspaceEdit> edit) {
                if (!edit) {
                    report(edit.error(), "Rename failed");
                    return;
                }
                if (edit->empty()) {
                    emit statusMessage(QStringLiteral("Nothing to rename"));
                    return;
                }
                applyWorkspaceEdit(*edit, QStringLiteral("Rename"));
            });
    });
}

LanguageController::~LanguageController() {
    m_structureSlot.cancel();
    for (sched::LatestWins* slot :
         {&m_completionSlot, &m_resolveSlot, &m_signatureSlot, &m_hoverSlot, &m_navigationSlot, &m_usagesSlot,
          &m_highlightSlot, &m_symbolSlot, &m_renameSlot, &m_actionSlot, &m_formatSlot}) {
        slot->cancel();
    }
}

void LanguageController::editorChanged() {
    structureChanged();
    for (sched::LatestWins* slot :
         {&m_completionSlot, &m_resolveSlot, &m_signatureSlot, &m_hoverSlot, &m_navigationSlot, &m_usagesSlot,
          &m_highlightSlot, &m_symbolSlot, &m_renameSlot, &m_actionSlot, &m_formatSlot}) {
        slot->cancel();
    }
    m_highlightTimer.stop();
    m_symbolQueryTimer.stop();
    closeLookup();
    m_parameterInfo->hide();
    m_quickDoc->hide();
    m_rename->hide();
    m_choice->hide();
    m_tabStops.clear();
    m_renamePath.clear();
}

StructurePanel& LanguageController::structurePanel() { return *m_structure; }

void LanguageController::setStructureVisible(bool visible) {
    m_structureVisible = visible;
    structureChanged();
}

void LanguageController::structureChanged() {
    ++m_structureGeneration;
    m_structureSlot.cancel();
    m_structureTimer.stop();
    m_structurePath.clear();
    if (!m_structureVisible) {
        m_structure->setStatus(QStringLiteral("Open Structure to view symbols."));
        return;
    }
    if (!hasDocument()) {
        m_structure->setStatus(QStringLiteral("Open a file to view its structure."));
        return;
    }
    m_structure->setStatus(QStringLiteral("Loading structure..."));
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

Result<void> LanguageController::registerActions(input::ActionRegistry& registry) {
    struct Entry {
        std::string_view id;
        std::string_view name;
        std::string_view group;
        void (LanguageController::*handler)();
        bool needsEditor;
    };
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
        {"navigate.gotoSymbol", "Go to Symbol", "Navigate", &LanguageController::gotoSymbol, false},
        {"refactor.rename", "Rename", "Refactor", &LanguageController::rename, true},
        {"code.showIntentions", "Show Context Actions", "Code", &LanguageController::showIntentions, true},
        {"code.reformat", "Reformat Code", "Code", &LanguageController::reformat, true},
    };
    for (const Entry& entry : entries) {
        input::ActionSpec spec;
        spec.id = std::string(entry.id);
        spec.name = std::string(entry.name);
        spec.group = std::string(entry.group);
        if (entry.needsEditor) {
            spec.context = input::ContextRule{.all = {input::ContextKey::EditorFocus}, .none = {}};
            spec.enabled = [this](const input::ActionContext&) { return hasDocument(); };
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

void LanguageController::completeCode() { requestCompletion(fl::CompletionTrigger{}); }

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
        for (QWidget* popup : {static_cast<QWidget*>(m_parameterInfo.get()), static_cast<QWidget*>(m_quickDoc.get())}) {
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
    start<fl::GotoResult>(
        m_navigationSlot,
        [&server = m_services.server, path, position](std::stop_token token) {
            return fl::gotoDeclarationOrUsages(server, path, position, std::move(token));
        },
        [this, path, position](Result<fl::GotoResult> result) {
            if (!result) {
                report(result.error(), "Go to declaration");
                return;
            }
            if (result->showUsages) {
                findUsagesAt(path, position);
                return;
            }
            openTargets(std::move(result->locations), QStringLiteral("Choose Declaration"),
                        QStringLiteral("Cannot find a declaration to go to"));
        });
}

void LanguageController::gotoImplementation() {
    if (!hasDocument()) {
        return;
    }
    start<std::vector<fl::Location>>(
        m_navigationSlot,
        [&server = m_services.server, path = m_services.host.documentPath(),
         position = caretPosition()](std::stop_token token) {
            return fl::requestLocations(server, fl::NavigationTarget::Implementation, path, position, std::move(token));
        },
        [this](Result<std::vector<fl::Location>> locations) {
            if (!locations) {
                report(locations.error(), "Go to implementation");
                return;
            }
            openTargets(std::move(*locations), QStringLiteral("Choose Implementation"),
                        QStringLiteral("No implementations found"));
        });
}

void LanguageController::gotoRelatedFile() {
    if (!hasDocument()) {
        return;
    }
    start<std::optional<std::string>>(
        m_navigationSlot,
        [&server = m_services.server, path = m_services.host.documentPath()](std::stop_token token) {
            return fl::requestSwitchSourceHeader(server, path, std::move(token));
        },
        [this](Result<std::optional<std::string>> related) {
            if (!related) {
                report(related.error(), "Go to related files");
                return;
            }
            if (!*related) {
                emit statusMessage(QStringLiteral("No related file found"));
                return;
            }
            m_services.host.openLocation(**related, fl::LspPosition{});
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
            return findUsagesTask(server, fs, open, path, position, encoding, std::move(token));
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

void LanguageController::gotoSymbol() {
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
    if (query.isEmpty()) {
        m_symbolSlot.cancel();
        m_symbols->setWorkspaceSymbols(QStringLiteral("Go to Symbol"), {});
        return;
    }
    start<std::vector<fl::WorkspaceSymbol>>(
        m_symbolSlot,
        [&server = m_services.server, query = query.toStdString()](std::stop_token token) {
            return fl::requestWorkspaceSymbols(server, query, std::move(token));
        },
        [this](Result<std::vector<fl::WorkspaceSymbol>> symbols) {
            if (!symbols) {
                report(symbols.error(), "Go to symbol");
                return;
            }
            if (m_symbols->isVisible() && !m_symbols->structureMode()) {
                m_symbols->setWorkspaceSymbols(QStringLiteral("Go to Symbol"), std::move(*symbols));
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
            m_renamePosition = position;
            m_rename->start(current, m_services.host.caretScreenRect());
        });
}

void LanguageController::applyWorkspaceEdit(const fl::WorkspaceEdit& edit, const QString& label) {
    if (auto applied = m_services.applier.apply(edit, label.toStdString()); !applied) {
        emit statusMessage(QStringLiteral("%1: %2").arg(label, qs(applied.error().message())));
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
            if (!actions) {
                report(actions.error(), "Quick fixes");
                return;
            }
            if (actions->empty()) {
                emit statusMessage(QStringLiteral("No quick fixes here"));
                return;
            }
            std::vector<QString> titles;
            std::vector<bool> preferred;
            m_choiceActions.clear();
            for (fl::CodeActionItem& action : *actions) {
                titles.push_back(qs(action.title));
                preferred.push_back(action.preferred);
                m_choiceActions.emplace_back([this, title = qs(action.title), edit = std::move(action.edit)] {
                    applyWorkspaceEdit(edit, title);
                });
            }
            m_choice->start(QString(), titles, m_services.host.caretScreenRect(), std::move(preferred));
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

} // namespace frost::ui::lang
