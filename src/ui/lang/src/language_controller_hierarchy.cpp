// SPDX-License-Identifier: Apache-2.0
//
// LanguageController navigation, hierarchies, and peek features (Milestone 2): Quick Definition
// peek popup, Show Usages floating popup, Call Hierarchy (Incoming/Outgoing), Type Hierarchy
// (Supertypes/Subtypes), Go to Super Method/Base, and Go to Type Declaration.

#include "frost/io/file_system.hpp"
#include "frost/lang/code_assist.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "language_controller_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::ui::lang {

namespace fl = ::frost::lang;
using detail::fileName;
using detail::qs;

namespace {

std::string identifierAtCaret(const std::optional<text::Snapshot>& snapshot, ByteOffset caretOffset) {
    if (!snapshot) {
        return "symbol";
    }
    ByteOffset begin = snapshot->clip(caretOffset);
    ByteOffset end = begin;
    while (begin.value() > 0 && fl::isIdentifierByte(snapshot->byteAt(ByteOffset{begin.value() - 1}))) {
        begin = ByteOffset{begin.value() - 1};
    }
    while (end < snapshot->size() && fl::isIdentifierByte(snapshot->byteAt(end))) {
        end = ByteOffset{end.value() + 1};
    }
    std::string ident = snapshot->text(text::ByteRange{begin, end});
    return ident.empty() ? "symbol" : ident;
}

QString buildLineWindowSnippet(const std::string& targetPath, std::uint32_t centerLine,
                               const std::vector<std::pair<std::string, text::Snapshot>>& openDocs,
                               io::IFileSystem& fs) {
    std::map<std::string, text::Snapshot, std::less<>> byKey;
    for (const auto& [openPath, snap] : openDocs) {
        byKey.insert_or_assign(fl::documentKey(openPath), snap);
    }
    fl::FileLineSource lineSource(fs, [&byKey](std::string_view file) -> std::optional<text::Snapshot> {
        const auto it = byKey.find(fl::documentKey(file));
        return it != byKey.end() ? std::optional<text::Snapshot>(it->second) : std::nullopt;
    });
    const fl::LineSource reader = lineSource.source();
    const std::uint32_t startLine = centerLine > 12 ? centerLine - 12 : 0;
    const std::uint32_t endLine = centerLine + 12;
    QStringList lines;
    for (std::uint32_t line = startLine; line <= endLine; ++line) {
        const auto text = reader(targetPath, line);
        if (!text) {
            if (line > centerLine) {
                break;
            }
            continue;
        }
        const QString marker = (line == centerLine) ? QStringLiteral(">") : QStringLiteral(" ");
        lines.push_back(QStringLiteral("%1 %2 | %3")
                            .arg(marker)
                            .arg(line + 1, 4)
                            .arg(QString::fromUtf8(text->data(), static_cast<qsizetype>(text->size()))));
    }
    return lines.join(QLatin1Char('\n'));
}

struct HierarchyQueryResult {
    QString rootName;
    std::vector<HierarchyPopup::Entry> entries;
};

sched::Task<Result<HierarchyQueryResult>> runCallHierarchyTask(fl::ILspRequester& server, std::string path,
                                                               fl::LspPosition position, bool outgoing,
                                                               std::string fallbackName, std::stop_token token) {
    HierarchyQueryResult out;
    out.rootName = qs(fallbackName);
    auto prepared = co_await fl::requestPrepareCallHierarchy(server, path, position, token);
    if (prepared && !prepared->empty()) {
        const fl::CallHierarchyItem& rootItem = prepared->front();
        out.rootName = qs(rootItem.name);
        HierarchyPopup::Entry rootNode{
            .label = qs(rootItem.name),
            .detail =
                QStringLiteral("%1:%2").arg(qs(fileName(rootItem.path))).arg(rootItem.selectionRange.start.line + 1),
            .kind = rootItem.kind,
            .path = rootItem.path,
            .position = rootItem.selectionRange.start,
            .children = {}};
        if (outgoing) {
            if (auto calls = co_await fl::requestOutgoingCalls(server, rootItem, token); calls) {
                for (const fl::CallHierarchyOutgoingCall& call : *calls) {
                    rootNode.children.push_back(
                        HierarchyPopup::Entry{.label = qs(call.to.name),
                                              .detail = QStringLiteral("%1:%2")
                                                            .arg(qs(fileName(call.to.path)))
                                                            .arg(call.to.selectionRange.start.line + 1),
                                              .kind = call.to.kind,
                                              .path = call.to.path,
                                              .position = call.to.selectionRange.start,
                                              .children = {}});
                }
            }
        } else {
            if (auto calls = co_await fl::requestIncomingCalls(server, rootItem, token); calls) {
                for (const fl::CallHierarchyIncomingCall& call : *calls) {
                    rootNode.children.push_back(
                        HierarchyPopup::Entry{.label = qs(call.from.name),
                                              .detail = QStringLiteral("%1:%2")
                                                            .arg(qs(fileName(call.from.path)))
                                                            .arg(call.from.selectionRange.start.line + 1),
                                              .kind = call.from.kind,
                                              .path = call.from.path,
                                              .position = call.from.selectionRange.start,
                                              .children = {}});
                }
            }
        }
        out.entries.push_back(std::move(rootNode));
        co_return out;
    }
    HierarchyPopup::Entry fallbackRoot{.label = qs(fallbackName),
                                       .detail = QStringLiteral("%1:%2").arg(qs(fileName(path))).arg(position.line + 1),
                                       .kind = fl::SymbolKind::Function,
                                       .path = path,
                                       .position = position,
                                       .children = {}};
    if (!outgoing) {
        if (auto refs = co_await fl::requestReferences(server, path, position, false, token); refs) {
            for (const fl::Location& loc : *refs) {
                fallbackRoot.children.push_back(HierarchyPopup::Entry{
                    .label = QStringLiteral("Call at %1:%2").arg(qs(fileName(loc.path))).arg(loc.range.start.line + 1),
                    .detail = qs(fileName(loc.path)),
                    .kind = fl::SymbolKind::Function,
                    .path = loc.path,
                    .position = loc.range.start,
                    .children = {}});
            }
        }
    }
    out.entries.push_back(std::move(fallbackRoot));
    co_return out;
}

sched::Task<Result<HierarchyQueryResult>> runTypeHierarchyTask(fl::ILspRequester& server, std::string path,
                                                               fl::LspPosition position, bool subtypes,
                                                               std::string fallbackName, std::stop_token token) {
    HierarchyQueryResult out;
    out.rootName = qs(fallbackName);
    auto prepared = co_await fl::requestPrepareTypeHierarchy(server, path, position, token);
    if (prepared && !prepared->empty()) {
        const fl::TypeHierarchyItem& rootItem = prepared->front();
        out.rootName = qs(rootItem.name);
        HierarchyPopup::Entry rootNode{
            .label = qs(rootItem.name),
            .detail =
                QStringLiteral("%1:%2").arg(qs(fileName(rootItem.path))).arg(rootItem.selectionRange.start.line + 1),
            .kind = rootItem.kind,
            .path = rootItem.path,
            .position = rootItem.selectionRange.start,
            .children = {}};
        auto related = subtypes ? co_await fl::requestSubtypes(server, rootItem, token)
                                : co_await fl::requestSupertypes(server, rootItem, token);
        if (related) {
            for (const fl::TypeHierarchyItem& item : *related) {
                rootNode.children.push_back(HierarchyPopup::Entry{
                    .label = qs(item.name),
                    .detail =
                        QStringLiteral("%1:%2").arg(qs(fileName(item.path))).arg(item.selectionRange.start.line + 1),
                    .kind = item.kind,
                    .path = item.path,
                    .position = item.selectionRange.start,
                    .children = {}});
            }
        }
        out.entries.push_back(std::move(rootNode));
        co_return out;
    }
    HierarchyPopup::Entry fallbackRoot{.label = qs(fallbackName),
                                       .detail = QStringLiteral("%1:%2").arg(qs(fileName(path))).arg(position.line + 1),
                                       .kind = fl::SymbolKind::Class,
                                       .path = path,
                                       .position = position,
                                       .children = {}};
    if (subtypes) {
        if (auto impls =
                co_await fl::requestLocations(server, fl::NavigationTarget::Implementation, path, position, token);
            impls) {
            for (const fl::Location& loc : *impls) {
                fallbackRoot.children.push_back(HierarchyPopup::Entry{
                    .label =
                        QStringLiteral("Subtype at %1:%2").arg(qs(fileName(loc.path))).arg(loc.range.start.line + 1),
                    .detail = qs(fileName(loc.path)),
                    .kind = fl::SymbolKind::Class,
                    .path = loc.path,
                    .position = loc.range.start,
                    .children = {}});
            }
        }
    }
    out.entries.push_back(std::move(fallbackRoot));
    co_return out;
}

sched::Task<Result<std::vector<fl::Location>>> runQuickDefinitionTask(fl::ILspRequester& server, std::string path,
                                                                      fl::LspPosition position,
                                                                      std::stop_token token) {
    auto defs = co_await fl::requestLocations(server, fl::NavigationTarget::Definition, path, position, token);
    if (defs && !defs->empty()) {
        co_return defs;
    }
    co_return co_await fl::requestLocations(server, fl::NavigationTarget::Declaration, path, position, token);
}

sched::Task<Result<std::vector<fl::Location>>> runGotoSuperMethodTask(fl::ILspRequester& server, std::string path,
                                                                      fl::LspPosition position,
                                                                      std::stop_token token) {
    if (auto prepared = co_await fl::requestPrepareTypeHierarchy(server, path, position, token);
        prepared && !prepared->empty()) {
        if (auto supers = co_await fl::requestSupertypes(server, prepared->front(), token);
            supers && !supers->empty()) {
            std::vector<fl::Location> locs;
            for (const fl::TypeHierarchyItem& item : *supers) {
                locs.push_back(fl::Location{.path = item.path, .uri = item.uri, .range = item.selectionRange});
            }
            co_return locs;
        }
    }
    co_return co_await fl::requestLocations(server, fl::NavigationTarget::Declaration, path, position, token);
}

} // namespace

void LanguageController::quickDefinition() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const fl::LspPosition position = caretPosition();
    start<std::vector<fl::Location>>(
        m_navigationSlot,
        [&server = m_services.server, path, position](std::stop_token token) {
            return runQuickDefinitionTask(server, path, position, std::move(token));
        },
        [this](Result<std::vector<fl::Location>> locations) {
            if (!locations) {
                report(locations.error(), "Quick definition");
                return;
            }
            if (locations->empty()) {
                emit statusMessage(QStringLiteral("No definition found"));
                return;
            }
            const fl::Location& target = locations->front();
            const QString snippet = buildLineWindowSnippet(target.path, target.range.start.line,
                                                           m_services.host.openDocuments(), m_services.fs);
            m_quickDef->showDefinition(qs(target.path), target.range.start, snippet, m_services.host.caretScreenRect());
        });
}

void LanguageController::showUsagesPopup() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const fl::LspPosition position = caretPosition();
    const std::string symbolName = identifierAtCaret(m_services.host.snapshot(), m_services.host.caretOffset());
    start<std::vector<fl::UsageGroup>>(
        m_usagesSlot,
        [&server = m_services.server, &fs = m_services.fs, open = m_services.host.openDocuments(), path, position,
         encoding = m_services.server.encoding()](std::stop_token token) {
            return detail::runFindUsagesTask(server, fs, open, path, position, encoding, std::move(token));
        },
        [this, symbolName](Result<std::vector<fl::UsageGroup>> groups) {
            if (!groups) {
                report(groups.error(), "Show usages");
                return;
            }
            std::vector<QString> choices;
            m_choiceActions.clear();
            for (const fl::UsageGroup& group : *groups) {
                for (const fl::Usage& item : group.usages) {
                    const QString preview = qs(item.lineText).trimmed();
                    const QString rowText =
                        preview.isEmpty() ? QStringLiteral("%1:%2").arg(qs(fileName(item.location.path))).arg(item.line)
                                          : QStringLiteral("%1:%2  %3")
                                                .arg(qs(fileName(item.location.path)))
                                                .arg(item.line)
                                                .arg(preview);
                    choices.push_back(rowText);
                    m_choiceActions.emplace_back(
                        [this, targetPath = item.location.path, pos = item.location.range.start] {
                            m_services.host.openLocation(targetPath, pos);
                        });
                }
            }
            if (choices.empty()) {
                emit statusMessage(QStringLiteral("No usages found"));
                return;
            }
            m_choice->start(QStringLiteral("Usages of '%1' (%2)").arg(qs(symbolName)).arg(choices.size()), choices,
                            m_services.host.caretScreenRect());
        });
}

void LanguageController::callHierarchy() {
    if (!hasDocument()) {
        return;
    }
    m_hierarchyPath = m_services.host.documentPath();
    m_hierarchyPosition = caretPosition();
    m_hierarchyIsType = false;
    m_hierarchy->configureModes(QStringLiteral("Incoming Calls"), QStringLiteral("Outgoing Calls"), false);
    refreshCallHierarchy(false);
}

void LanguageController::refreshCallHierarchy(bool outgoing) {
    if (m_hierarchyPath.empty()) {
        return;
    }
    const std::string fallbackName = identifierAtCaret(m_services.host.snapshot(), m_services.host.caretOffset());
    start<HierarchyQueryResult>(
        m_hierarchySlot,
        [&server = m_services.server, path = m_hierarchyPath, pos = m_hierarchyPosition, outgoing,
         fallbackName](std::stop_token token) {
            return runCallHierarchyTask(server, path, pos, outgoing, fallbackName, std::move(token));
        },
        [this, outgoing](Result<HierarchyQueryResult> res) {
            if (!res) {
                report(res.error(), "Call hierarchy");
                return;
            }
            const QString modePrefix =
                outgoing ? QStringLiteral("Outgoing calls from '%1'") : QStringLiteral("Incoming calls to '%1'");
            m_hierarchy->setEntries(modePrefix.arg(res->rootName), std::move(res->entries));
            if (!m_hierarchy->isVisible()) {
                m_hierarchy->popup(m_services.host.editorWidget());
            }
        });
}

void LanguageController::typeHierarchy() {
    if (!hasDocument()) {
        return;
    }
    m_hierarchyPath = m_services.host.documentPath();
    m_hierarchyPosition = caretPosition();
    m_hierarchyIsType = true;
    m_hierarchy->configureModes(QStringLiteral("Supertypes"), QStringLiteral("Subtypes"), false);
    refreshTypeHierarchy(false);
}

void LanguageController::refreshTypeHierarchy(bool subtypes) {
    if (m_hierarchyPath.empty()) {
        return;
    }
    const std::string fallbackName = identifierAtCaret(m_services.host.snapshot(), m_services.host.caretOffset());
    start<HierarchyQueryResult>(
        m_hierarchySlot,
        [&server = m_services.server, path = m_hierarchyPath, pos = m_hierarchyPosition, subtypes,
         fallbackName](std::stop_token token) {
            return runTypeHierarchyTask(server, path, pos, subtypes, fallbackName, std::move(token));
        },
        [this, subtypes](Result<HierarchyQueryResult> res) {
            if (!res) {
                report(res.error(), "Type hierarchy");
                return;
            }
            const QString modePrefix =
                subtypes ? QStringLiteral("Subtypes of '%1'") : QStringLiteral("Supertypes of '%1'");
            m_hierarchy->setEntries(modePrefix.arg(res->rootName), std::move(res->entries));
            if (!m_hierarchy->isVisible()) {
                m_hierarchy->popup(m_services.host.editorWidget());
            }
        });
}

void LanguageController::gotoSuperMethod() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const fl::LspPosition position = caretPosition();
    start<std::vector<fl::Location>>(
        m_navigationSlot,
        [&server = m_services.server, path, position](std::stop_token token) {
            return runGotoSuperMethodTask(server, path, position, std::move(token));
        },
        [this](Result<std::vector<fl::Location>> locations) {
            if (!locations) {
                report(locations.error(), "Go to super method / base");
                return;
            }
            openTargets(std::move(*locations), QStringLiteral("Choose Super / Base"),
                        QStringLiteral("No super method or base type found"));
        });
}

void LanguageController::gotoTypeDeclaration() {
    if (!hasDocument()) {
        return;
    }
    start<std::vector<fl::Location>>(
        m_navigationSlot,
        [&server = m_services.server, path = m_services.host.documentPath(),
         position = caretPosition()](std::stop_token token) {
            return fl::requestLocations(server, fl::NavigationTarget::TypeDefinition, path, position, std::move(token));
        },
        [this](Result<std::vector<fl::Location>> locations) {
            if (!locations) {
                report(locations.error(), "Go to type declaration");
                return;
            }
            openTargets(std::move(*locations), QStringLiteral("Choose Type Declaration"),
                        QStringLiteral("No type declaration found"));
        });
}

} // namespace frost::ui::lang
