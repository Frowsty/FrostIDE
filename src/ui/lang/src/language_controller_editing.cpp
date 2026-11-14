// SPDX-License-Identifier: Apache-2.0
//
// LanguageController (frost/ui/lang/language_controller.hpp): rename, workspace edits, server
// commands, snippet insertion, intentions (quick fixes) and reformat (split from
// language_controller.cpp, FND-12a).

#include "frost/lang/clangd_actions.hpp"
#include "frost/lang/document_features.hpp"
#include "frost/lang/snippet.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "language_controller_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace frost::ui::lang {

namespace fl = ::frost::lang;

namespace {

using detail::fileName;

using detail::qs;

} // namespace

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
                return;
            }
            if (result->isObject() && (result->find("changes") != nullptr ||
                                       result->find("documentChanges") != nullptr || result->find("edit") != nullptr)) {
                const fl::Json* editNode = result->find("edit");
                if (auto parsed = fl::parseWorkspaceEdit(editNode != nullptr ? *editNode : *result);
                    parsed && !parsed->empty()) {
                    applyWorkspaceEdit(*parsed, label);
                }
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
    beginLiveTemplate(parsed, base);
}

void LanguageController::showIntentions() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const std::optional<text::Snapshot> requestedSnapshot = m_services.host.snapshot();
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
        [this, path, all, requestedSnapshot](Result<std::vector<fl::CodeActionItem>> actions) {
            std::vector<QString> titles;
            std::vector<bool> preferred;
            m_choiceActions.clear();
            if (actions) {
                for (fl::CodeActionItem& action : *actions) {
                    titles.push_back(qs(action.title));
                    preferred.push_back(action.preferred);
                    m_choiceActions.emplace_back([this, path, requestedSnapshot, title = qs(action.title),
                                                  edit = std::move(action.edit), cmd = std::move(action.commandName),
                                                  args = std::move(action.commandArguments)]() mutable {
                        const auto currentSnapshot = m_services.host.snapshot();
                        if (!hasDocument() || m_services.host.documentPath() != path || !requestedSnapshot ||
                            !currentSnapshot || !currentSnapshot->sharesStorageWith(*requestedSnapshot) ||
                            currentSnapshot->version() != requestedSnapshot->version()) {
                            emit statusMessage(QStringLiteral("The document changed; request the action again"));
                            return;
                        }
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
            const std::optional<fl::ServerCapabilities> serverCaps = m_services.server.serverCapabilities();
            const bool clangdActive =
                capabilities().clangdTweaks && ((serverCaps && serverCaps->serverName == "clangd") ||
                                                std::ranges::any_of(all, fl::isIncludeCleanerDiagnostic));
            if (clangdActive) {
                addContextAction(QStringLiteral("Optimize Includes"), [this, path, all, requestedSnapshot]() {
                    start<fl::WorkspaceEdit>(
                        m_actionSlot,
                        [&server = m_services.server, path, all](std::stop_token token) {
                            return fl::requestOptimizeIncludes(server, path, all, std::move(token));
                        },
                        [this, path, requestedSnapshot](Result<fl::WorkspaceEdit> edit) {
                            if (!edit) {
                                report(edit.error(), "Optimize Includes");
                                return;
                            }
                            if (edit->empty()) {
                                emit statusMessage(QStringLiteral("Optimize Includes: no include changes needed"));
                                return;
                            }
                            const auto currentSnapshot = m_services.host.snapshot();
                            if (!hasDocument() || m_services.host.documentPath() != path || !requestedSnapshot ||
                                !currentSnapshot || !currentSnapshot->sharesStorageWith(*requestedSnapshot) ||
                                currentSnapshot->version() != requestedSnapshot->version()) {
                                emit statusMessage(
                                    QStringLiteral("The document changed; request Optimize Includes again"));
                                return;
                            }
                            applyWorkspaceEdit(*edit, QStringLiteral("Optimize Includes"));
                        });
                });
            }
            m_choice->start(QStringLiteral("Context Actions"), titles, m_services.host.caretScreenRect(),
                            std::move(preferred));
        });
}

void LanguageController::showAst() {
    if (!hasDocument()) {
        return;
    }
    if (!capabilities().clangdTweaks) {
        unavailable(QStringLiteral("Show AST is available for C and C++ files (clangd)"));
        return;
    }
    const std::optional<fl::ServerCapabilities> server = m_services.server.serverCapabilities();
    if (!server) {
        emit statusMessage(emptyResultMessage(QStringLiteral("No AST")));
        return;
    }
    if (!server->ast) {
        emit statusMessage(QStringLiteral("Show AST: the language server does not offer textDocument/ast"));
        return;
    }
    const std::string path = m_services.host.documentPath();
    fl::LspRange range{caretPosition(), caretPosition()};
    if (const auto selection = m_services.host.selection()) {
        range = fl::LspRange{positionAt(selection->begin), positionAt(selection->end)};
    }
    start<std::optional<fl::AstNode>>(
        m_astSlot,
        [&server = m_services.server, path, range](std::stop_token token) {
            return fl::requestAst(server, path, range, std::move(token));
        },
        [this, path, range](Result<std::optional<fl::AstNode>> node) {
            if (!node) {
                report(node.error(), "Show AST");
                return;
            }
            if (!node->has_value()) {
                emit statusMessage(
                    QStringLiteral("Show AST: clangd found no AST node at line %1").arg(range.start.line + 1));
                return;
            }
            m_ast->setAst(QStringLiteral("AST at %1:%2 (clangd)").arg(qs(fileName(path))).arg(range.start.line + 1),
                          **node);
            m_ast->popup(m_services.host.editorWidget());
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

void LanguageController::reformatFile() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    m_formatVersion = m_services.host.documentVersion();
    const FormattingSettings settings = m_services.host.formattingOptions();
    const fl::FormattingOptions options{.tabSize = settings.insertSpaces ? settings.indentSize : settings.tabSize,
                                        .insertSpaces = settings.insertSpaces};
    start<std::vector<fl::TextEdit>>(
        m_formatSlot,
        [&server = m_services.server, path, options](std::stop_token token) {
            return fl::requestFormatting(server, path, std::nullopt, options, std::move(token));
        },
        [this, path, settings](Result<std::vector<fl::TextEdit>> edits) {
            if (!edits) {
                report(edits.error(), "Reformat file");
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
                report(bytes.error(), "Reformat file");
                return;
            }
            if (bytes->empty()) {
                emit statusMessage(QStringLiteral("The code is already formatted"));
                return;
            }
            const ByteOffset caret = fl::mapThroughEdits(m_services.host.caretOffset(), *bytes);
            if (auto applied = m_services.host.applyEdits(*bytes, "Reformat File", caret); !applied) {
                report(applied.error(), "Reformat file");
            }
        });
}

} // namespace frost::ui::lang
