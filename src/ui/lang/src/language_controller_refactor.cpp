// SPDX-License-Identifier: Apache-2.0
//
// LanguageController refactorings (FND-3, ADR 0078): Rename with comment/string search and preview,
// Refactor This (Rename plus the server's refactor actions), Extract Variable and Extract Method
// through clangd's tweaks, and the refactorings without a correct engine yet, which only report
// the epic that owns them. No refactoring here computes an edit from line text.

#include "frost/lang/clangd_actions.hpp"
#include "frost/lang/code_assist.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/refactoring.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "language_controller_internal.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::ui::lang {

namespace fl = ::frost::lang;
using detail::qs;

namespace {

struct TextSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
};

/*
 * The comments and string literals of `text` for `languageId` ("cpp", "csharp" or "lua"; any other
 * id has none), for Rename's "search in comments and strings" option. A lexical scan: C-family line
 * and block comments, "..." and '...' with backslash escapes, C++ raw strings R"d(...)d" and C#
 * verbatim strings @"..." ("" escapes); Lua -- and --[[ ]] comments, quoted strings and long
 * brackets [[ ]] / [=[ ]=]. O(text).
 */
std::vector<TextSpan> commentAndStringSpans(std::string_view text, std::string_view languageId) {
    std::vector<TextSpan> spans;
    const bool cFamily = languageId == "cpp" || languageId == "csharp";
    const bool lua = languageId == "lua";
    if (!cFamily && !lua) {
        return spans;
    }
    const auto quoted = [&](std::size_t i, char quote) {
        std::size_t j = i + 1;
        while (j < text.size() && text[j] != quote && text[j] != '\n') {
            j += text[j] == '\\' ? 2 : 1;
        }
        return std::min(j + 1, text.size());
    };
    // A Lua long bracket at `i` ("[[" or "[=*["): its level, or -1.
    const auto longLevel = [&](std::size_t i) -> int {
        if (i >= text.size() || text[i] != '[') {
            return -1;
        }
        std::size_t j = i + 1;
        while (j < text.size() && text[j] == '=') {
            ++j;
        }
        return j < text.size() && text[j] == '[' ? static_cast<int>(j - i - 1) : -1;
    };
    const auto longEnd = [&](std::size_t i, int level) {
        const std::string close = "]" + std::string(static_cast<std::size_t>(level), '=') + "]";
        const std::size_t at = text.find(close, i + static_cast<std::size_t>(level) + 2);
        return at == std::string_view::npos ? text.size() : at + close.size();
    };
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        std::size_t end = i;
        if (cFamily && c == '/' && next == '/') {
            end = std::min(text.find('\n', i), text.size());
        } else if (cFamily && c == '/' && next == '*') {
            const std::size_t close = text.find("*/", i + 2);
            end = close == std::string_view::npos ? text.size() : close + 2;
        } else if (cFamily && languageId == "cpp" && c == 'R' && next == '"' &&
                   (i == 0 || !fl::isIdentifierByte(text[i - 1]) || text[i - 1] == 'u' || text[i - 1] == 'U' ||
                    text[i - 1] == 'L' || text[i - 1] == '8')) {
            const std::size_t open = text.find('(', i + 2);
            if (open == std::string_view::npos) {
                break;
            }
            const std::string close = ")" + std::string(text.substr(i + 2, open - i - 2)) + "\"";
            const std::size_t at = text.find(close, open + 1);
            end = at == std::string_view::npos ? text.size() : at + close.size();
        } else if (cFamily && languageId == "csharp" && c == '@' && next == '"') {
            std::size_t j = i + 2;
            while (j < text.size() && !(text[j] == '"' && (j + 1 >= text.size() || text[j + 1] != '"'))) {
                j += text[j] == '"' ? 2 : 1;
            }
            end = std::min(j + 1, text.size());
        } else if (lua && c == '-' && next == '-') {
            const int level = longLevel(i + 2);
            end = level >= 0 ? longEnd(i + 2, level) : std::min(text.find('\n', i), text.size());
        } else if (lua && longLevel(i) >= 0) {
            end = longEnd(i, longLevel(i));
        } else if (c == '"' || c == '\'') {
            end = quoted(i, c);
        } else {
            ++i;
            continue;
        }
        spans.push_back(TextSpan{i, end});
        i = std::max(end, i + 1);
    }
    return spans;
}

// Rename's "search in comments and strings" option: the whole-word occurrences of `oldName` inside
// comments and string literals (commentAndStringSpans) that the server's rename did not cover.
// Occurrences in code are left to the server, so the option never renames an unrelated symbol.
void addCommentAndStringOccurrences(fl::FileEdit& fileEdit, const text::Snapshot& snap, std::string_view oldName,
                                    std::string_view newName, fl::PositionEncoding encoding) {
    const std::string fullText = snap.text();
    const std::vector<TextSpan> spans = commentAndStringSpans(fullText, languageIdForPath(fileEdit.path));
    for (const TextSpan& span : spans) {
        std::size_t pos = span.begin;
        while ((pos = fullText.find(oldName, pos)) != std::string::npos && pos + oldName.size() <= span.end) {
            const bool wordBefore = pos == 0 || !fl::isIdentifierByte(fullText[pos - 1]);
            const std::size_t endPos = pos + oldName.size();
            const bool wordAfter = endPos >= fullText.size() || !fl::isIdentifierByte(fullText[endPos]);
            if (wordBefore && wordAfter) {
                const fl::LspRange candidate{fl::toLspPosition(snap, ByteOffset{pos}, encoding),
                                             fl::toLspPosition(snap, ByteOffset{endPos}, encoding)};
                const bool overlaps = std::ranges::any_of(fileEdit.edits, [&](const fl::TextEdit& existing) {
                    return !(existing.range.end < candidate.start) && !(candidate.end < existing.range.start);
                });
                if (!overlaps) {
                    fileEdit.edits.push_back(fl::TextEdit{.range = candidate, .newText = std::string(newName)});
                }
            }
            pos = endPos;
        }
    }
}

} // namespace

void LanguageController::onRenameAccepted(const QString& name) {
    if (name.isEmpty() || m_renamePath.empty())
        return;
    const auto path = m_renamePath;
    const auto position = m_renamePosition;
    const auto oldName = m_renameOldName;
    const auto newName = name.toStdString();
    const bool searchComments = m_rename->searchInCommentsAndStrings();
    const bool preview = m_rename->previewUsages();
    const auto captured = m_services.host.snapshot();
    const auto version = m_services.host.documentVersion();
    const auto identity = m_services.host.structureIdentity();
    QPointer<LanguageController> self(this);
    const auto ordinary = [self, path, position, oldName, newName, searchComments, preview, captured, version,
                           identity] {
        if (!self || !captured || fl::documentKey(self->m_services.host.documentPath()) != fl::documentKey(path) ||
            self->m_services.host.documentVersion() != version || self->m_services.host.structureIdentity() != identity)
            return;
        const auto current = self->m_services.host.snapshot();
        if (!current || !current->sharesStorageWith(*captured) || current->version() != captured->version())
            return;
        self->performAcceptedRename(path, position, oldName, newName, searchComments, preview);
    };
    if (m_services.combinedBindingRename && m_services.combinedBindingRename(path, position, newName, ordinary))
        return;
    ordinary();
}

void LanguageController::performAcceptedRename(std::string path, fl::LspPosition position, std::string oldName,
                                               std::string newName, bool searchComments, bool preview) {
    start<fl::WorkspaceEdit>(
        m_renameSlot,
        [&server = m_services.server, path, position, newName](std::stop_token token) {
            return fl::requestRename(server, path, position, newName, std::move(token));
        },
        [this, searchComments, preview, oldName, newName](Result<fl::WorkspaceEdit> edit) {
            if (!edit) {
                report(edit.error(), "Rename failed");
                return;
            }
            if (edit->empty()) {
                emit statusMessage(QStringLiteral("Nothing to rename"));
                return;
            }
            if (searchComments && !oldName.empty() && oldName != newName) {
                for (fl::FileEdit& fileEdit : edit->files) {
                    std::optional<text::Snapshot> snap;
                    if (fl::documentKey(m_services.host.documentPath()) == fl::documentKey(fileEdit.path)) {
                        snap = m_services.host.snapshot();
                    }
                    if (!snap) {
                        for (const auto& [openPath, openSnap] : m_services.host.openDocuments()) {
                            if (fl::documentKey(openPath) == fl::documentKey(fileEdit.path)) {
                                snap = openSnap;
                                break;
                            }
                        }
                    }
                    if (snap) {
                        addCommentAndStringOccurrences(fileEdit, *snap, oldName, newName, m_services.server.encoding());
                    }
                }
            }
            if (preview) {
                std::vector<fl::Location> locs;
                for (const fl::FileEdit& fe : edit->files) {
                    for (const fl::TextEdit& te : fe.edits) {
                        locs.push_back(fl::Location{.path = fe.path, .uri = fe.uri, .range = te.range});
                    }
                }
                m_usages->setUsages(QStringLiteral("Rename preview '%1' -> '%2'").arg(qs(oldName), qs(newName)),
                                    fl::groupUsages(locs, {}, m_services.server.encoding()));
                emit usagesReady();
                m_choiceActions = {
                    [this, finalEdit = *edit] { applyWorkspaceEdit(finalEdit, QStringLiteral("Rename")); }, [] {}};
                m_choice->start(
                    QStringLiteral("Preview Rename"),
                    {QStringLiteral("Apply Rename (%1 occurrences)").arg(edit->editCount()), QStringLiteral("Cancel")},
                    m_services.host.caretScreenRect(), {true, false});
                return;
            }
            applyWorkspaceEdit(*edit, QStringLiteral("Rename"));
        });
}

void LanguageController::refactorThis() {
    if (!hasDocument()) {
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
        [this](Result<std::vector<fl::CodeActionItem>> serverActions) {
            std::vector<QString> titles;
            std::vector<bool> preferred;
            m_choiceActions.clear();
            titles.push_back(QStringLiteral("Rename..."));
            preferred.push_back(true);
            m_choiceActions.emplace_back([this] { rename(); });
            if (!serverActions) {
                report(serverActions.error(), "Refactor This");
            } else {
                // clangd_actions.hpp's order: the known tweaks, then the server's other refactorings.
                for (fl::CodeActionItem& item : fl::orderRefactorActions(std::move(*serverActions))) {
                    titles.push_back(qs(item.title));
                    preferred.push_back(false);
                    m_choiceActions.emplace_back(
                        [this, action = std::move(item)]() mutable { runCodeAction(std::move(action)); });
                }
            }
            m_choice->start(QStringLiteral("Refactor This"), titles, m_services.host.caretScreenRect(),
                            std::move(preferred));
        });
}

void LanguageController::extractVariable() {
    runClangdTweak("ExtractVariable", QStringLiteral("Extract Variable"),
                   QStringLiteral("Extract Variable: select an expression clangd can extract"));
}

void LanguageController::extractMethod() {
    runClangdTweak("ExtractFunction", QStringLiteral("Extract Method"),
                   QStringLiteral("Extract Method: select whole statements clangd can extract to a function"));
}

void LanguageController::extractConstant() {
    unavailable(QStringLiteral("Extract Constant requires the extract refactorings (REF-7)"));
}

void LanguageController::extractField() {
    unavailable(QStringLiteral("Extract Field requires the extract refactorings (REF-7)"));
}

void LanguageController::extractParameter() {
    unavailable(QStringLiteral("Extract Parameter requires the extract refactorings (REF-7)"));
}

void LanguageController::inlineSymbol() {
    unavailable(QStringLiteral("Inline requires the inline refactoring (REF-6)"));
}

void LanguageController::changeSignature() {
    unavailable(QStringLiteral("Change Signature requires the change signature refactoring (REF-6)"));
}

void LanguageController::safeDelete() {
    unavailable(QStringLiteral("Safe Delete requires the safe delete refactoring (REF-8)"));
}

void LanguageController::moveSymbol() { unavailable(QStringLiteral("Move requires the move refactoring (REF-8)")); }

void LanguageController::copySymbol() { unavailable(QStringLiteral("Copy requires the copy refactoring (REF-8)")); }

void LanguageController::smartCompleteCode() {
    unavailable(QStringLiteral("Smart Completion requires type-matching completion (CPP-14)"));
}

} // namespace frost::ui::lang
