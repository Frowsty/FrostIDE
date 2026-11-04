// SPDX-License-Identifier: Apache-2.0
//
// LanguageController refactorings (Milestone 2): Refactor This popup, Rename with comment/string
// search and preview, Extract Variable/Constant/Field/Parameter/Method, Inline, Change Signature
// dialog, Safe Delete, and Move to related or dedicated file.

#include "frost/lang/code_assist.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/refactoring.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "language_controller_internal.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::ui::lang {

namespace fl = ::frost::lang;
using detail::fileName;
using detail::isCSharpPath;
using detail::qs;

namespace {

enum class ExtractMode : int { Variable = 0, Constant = 1, Field = 2, Parameter = 3, Method = 4 };

struct ExpressionSpan {
    text::ByteRange range;
    std::string text;
    ByteOffset lineBegin{};
    std::string lineIndent;
};

std::optional<ExpressionSpan> findTargetExpression(const text::Snapshot& snapshot, ByteOffset caret,
                                                   std::optional<text::ByteRange> selection) {
    const ByteOffset clampedCaret = snapshot.clip(caret);
    const auto lineInfo = snapshot.lineInfo(snapshot.lineAt(clampedCaret));
    if (!lineInfo) {
        return std::nullopt;
    }
    const std::string lineStr = snapshot.text(lineInfo->content);
    const std::size_t indentLen = std::min(lineStr.find_first_not_of(" \t"), lineStr.size());
    const std::string indent = lineStr.substr(0, indentLen);

    if (selection && !selection->empty()) {
        std::string selected = snapshot.text(*selection);
        while (!selected.empty() && (selected.back() == ';' || selected.back() == ' ' || selected.back() == '\t' ||
                                     selected.back() == '\r' || selected.back() == '\n')) {
            selected.pop_back();
        }
        if (!selected.empty()) {
            return ExpressionSpan{*selection, std::move(selected), lineInfo->content.begin, indent};
        }
    }
    ByteOffset begin = clampedCaret;
    ByteOffset end = clampedCaret;
    while (begin > lineInfo->content.begin && fl::isIdentifierByte(snapshot.byteAt(ByteOffset{begin.value() - 1}))) {
        begin = ByteOffset{begin.value() - 1};
    }
    while (end < lineInfo->content.end && fl::isIdentifierByte(snapshot.byteAt(end))) {
        end = ByteOffset{end.value() + 1};
    }
    if (end < lineInfo->content.end && snapshot.byteAt(end) == '(') {
        int depth = 0;
        while (end < lineInfo->content.end) {
            const char ch = snapshot.byteAt(end);
            end = ByteOffset{end.value() + 1};
            if (ch == '(') {
                ++depth;
            } else if (ch == ')' && --depth == 0) {
                break;
            }
        }
    }
    if (begin == end) {
        std::size_t exprStart = indentLen;
        if (const std::size_t eq = lineStr.find('='); eq != std::string::npos && eq + 1 < lineStr.size()) {
            exprStart = eq + 1;
            while (exprStart < lineStr.size() && (lineStr[exprStart] == ' ' || lineStr[exprStart] == '\t')) {
                ++exprStart;
            }
        }
        std::size_t exprEnd = lineStr.size();
        while (exprEnd > exprStart &&
               (lineStr[exprEnd - 1] == ';' || lineStr[exprEnd - 1] == ' ' || lineStr[exprEnd - 1] == '\t')) {
            --exprEnd;
        }
        if (exprEnd > exprStart) {
            begin = ByteOffset{lineInfo->content.begin.value() + exprStart};
            end = ByteOffset{lineInfo->content.begin.value() + exprEnd};
        }
    }
    if (begin >= end) {
        return std::nullopt;
    }
    return ExpressionSpan{text::ByteRange{begin, end}, snapshot.text(text::ByteRange{begin, end}),
                          lineInfo->content.begin, indent};
}

std::optional<ByteOffset> findEnclosingFunctionLineStart(const text::Snapshot& snapshot, ByteOffset caret) {
    const std::uint32_t currentLine = snapshot.lineAt(snapshot.clip(caret)).value();
    int braceBalance = 0;
    for (std::uint32_t line = currentLine; line > 0; --line) {
        const auto info = snapshot.lineInfo(LineIndex{line - 1});
        if (!info) {
            continue;
        }
        const std::string text = snapshot.text(info->content);
        for (char ch : text) {
            if (ch == '}') {
                ++braceBalance;
            } else if (ch == '{') {
                --braceBalance;
            }
        }
        if (braceBalance < 0 && text.find('(') != std::string::npos) {
            return info->content.begin;
        }
    }
    return std::nullopt;
}

std::optional<ByteOffset> findEnclosingParameterListEnd(const text::Snapshot& snapshot, ByteOffset caret) {
    const std::uint32_t currentLine = snapshot.lineAt(snapshot.clip(caret)).value();
    for (std::uint32_t line = currentLine + 1; line > 0; --line) {
        const auto info = snapshot.lineInfo(LineIndex{line - 1});
        if (!info) {
            continue;
        }
        const std::string text = snapshot.text(info->content);
        const std::size_t closeParen = text.rfind(')');
        const std::size_t openParen = text.find('(');
        if (openParen != std::string::npos && closeParen != std::string::npos && closeParen > openParen &&
            (text.find('{', closeParen) != std::string::npos || line - 1 < currentLine)) {
            const std::string_view trimCheck = std::string_view(text).substr(0, openParen);
            if (trimCheck.find("if ") == std::string_view::npos && trimCheck.find("for ") == std::string_view::npos &&
                trimCheck.find("while ") == std::string_view::npos &&
                trimCheck.find("switch ") == std::string_view::npos) {
                return ByteOffset{info->content.begin.value() + closeParen};
            }
        }
    }
    return std::nullopt;
}

void addCommentAndStringOccurrences(fl::FileEdit& fileEdit, const text::Snapshot& snap, std::string_view oldName,
                                    std::string_view newName, fl::PositionEncoding encoding) {
    const std::string fullText = snap.text();
    std::size_t pos = 0;
    while ((pos = fullText.find(oldName, pos)) != std::string::npos) {
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

struct ParsedSignature {
    text::ByteRange fullRange;
    std::string prefixIndent;
    std::string returnType;
    std::string functionName;
    std::vector<std::string> parameters;
    std::string suffixAfterCloseParen;
};

std::optional<ParsedSignature> parseSignatureAtLine(const text::Snapshot& snapshot, ByteOffset caret) {
    const std::uint32_t startLine = snapshot.lineAt(snapshot.clip(caret)).value();
    for (std::uint32_t attempt = 0; attempt <= startLine && attempt < 15; ++attempt) {
        const std::uint32_t lineIdx = startLine - attempt;
        const auto info = snapshot.lineInfo(LineIndex{lineIdx});
        if (!info) {
            continue;
        }
        const std::string line = snapshot.text(info->content);
        const std::size_t openP = line.find('(');
        const std::size_t closeP = line.rfind(')');
        if (openP == std::string::npos || closeP == std::string::npos || closeP < openP) {
            continue;
        }
        const std::size_t indentEnd = std::min(line.find_first_not_of(" \t"), openP);
        const std::string_view header = std::string_view(line).substr(indentEnd, openP - indentEnd);
        std::size_t nameEnd = header.size();
        while (nameEnd > 0 && (header[nameEnd - 1] == ' ' || header[nameEnd - 1] == '\t')) {
            --nameEnd;
        }
        std::size_t nameStart = nameEnd;
        while (nameStart > 0 && (fl::isIdentifierByte(header[nameStart - 1]) || header[nameStart - 1] == ':' ||
                                 header[nameStart - 1] == '~')) {
            --nameStart;
        }
        if (nameStart == nameEnd) {
            continue;
        }
        const std::string fnName(header.substr(nameStart, nameEnd - nameStart));
        if (fnName == "if" || fnName == "for" || fnName == "while" || fnName == "switch" || fnName == "catch") {
            continue;
        }
        std::string retType(header.substr(0, nameStart));
        while (!retType.empty() && (retType.back() == ' ' || retType.back() == '\t')) {
            retType.pop_back();
        }
        ParsedSignature out;
        out.fullRange = info->content;
        out.prefixIndent = line.substr(0, indentEnd);
        out.returnType = std::move(retType);
        out.functionName = fnName;
        out.suffixAfterCloseParen = line.substr(closeP + 1);
        const std::string_view paramStr = std::string_view(line).substr(openP + 1, closeP - openP - 1);
        std::size_t pStart = 0;
        int angleDepth = 0;
        for (std::size_t i = 0; i <= paramStr.size(); ++i) {
            if (i < paramStr.size()) {
                if (paramStr[i] == '<') {
                    ++angleDepth;
                } else if (paramStr[i] == '>' && angleDepth > 0) {
                    --angleDepth;
                }
            }
            if ((i == paramStr.size() || (paramStr[i] == ',' && angleDepth == 0))) {
                std::string_view part = paramStr.substr(pStart, i - pStart);
                while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) {
                    part.remove_prefix(1);
                }
                while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) {
                    part.remove_suffix(1);
                }
                if (!part.empty()) {
                    out.parameters.emplace_back(part);
                }
                pStart = i + 1;
            }
        }
        return out;
    }
    return std::nullopt;
}

} // namespace

void LanguageController::onRenameAccepted(const QString& name) {
    if (m_customRenameCallback) {
        auto cb = std::move(m_customRenameCallback);
        m_customRenameCallback = nullptr;
        if (!name.isEmpty()) {
            cb(name);
        }
        return;
    }
    if (name.isEmpty() || m_renamePath.empty()) {
        return;
    }
    const bool searchComments = m_rename->searchInCommentsAndStrings();
    const bool preview = m_rename->previewUsages();
    const std::string oldName = m_renameOldName;
    const std::string newName = name.toStdString();
    start<fl::WorkspaceEdit>(
        m_renameSlot,
        [&server = m_services.server, path = m_renamePath, position = m_renamePosition, newName](
            std::stop_token token) { return fl::requestRename(server, path, position, newName, std::move(token)); },
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
            const auto add = [&](QString title, std::function<void()> fn, bool pref = false) {
                titles.push_back(std::move(title));
                preferred.push_back(pref);
                m_choiceActions.push_back(std::move(fn));
            };
            add(QStringLiteral("Rename..."), [this] { rename(); }, true);
            add(QStringLiteral("Extract Variable..."), [this] { extractVariable(); });
            add(QStringLiteral("Extract Constant..."), [this] { extractConstant(); });
            add(QStringLiteral("Extract Field..."), [this] { extractField(); });
            add(QStringLiteral("Extract Parameter..."), [this] { extractParameter(); });
            add(QStringLiteral("Extract Method..."), [this] { extractMethod(); });
            add(QStringLiteral("Inline..."), [this] { inlineSymbol(); });
            add(QStringLiteral("Change Signature..."), [this] { changeSignature(); });
            add(QStringLiteral("Move..."), [this] { moveSymbol(); });
            add(QStringLiteral("Safe Delete..."), [this] { safeDelete(); });
            if (serverActions) {
                for (fl::CodeActionItem& item : *serverActions) {
                    if (!item.kind.empty() && !item.kind.starts_with("refactor")) {
                        continue;
                    }
                    add(
                        qs(item.title),
                        [this, title = qs(item.title), edit = std::move(item.edit), cmd = std::move(item.commandName),
                         args = std::move(item.commandArguments)]() mutable {
                            if (!edit.empty()) {
                                applyWorkspaceEdit(edit, title);
                            }
                            if (!cmd.empty()) {
                                executeServerCommand(std::move(cmd), std::move(args), title);
                            }
                        },
                        item.preferred);
                }
            }
            m_choice->start(QStringLiteral("Refactor This"), titles, m_services.host.caretScreenRect(),
                            std::move(preferred));
        });
}

void LanguageController::extractVariable() {
    extractWithKind("refactor.extract.variable", QStringLiteral("Extract variable:"), QStringLiteral("extracted"),
                    static_cast<int>(ExtractMode::Variable));
}

void LanguageController::extractConstant() {
    extractWithKind("refactor.extract.constant", QStringLiteral("Extract constant:"), QStringLiteral("kExtracted"),
                    static_cast<int>(ExtractMode::Constant));
}

void LanguageController::extractField() {
    extractWithKind("refactor.extract.field", QStringLiteral("Extract field:"), QStringLiteral("m_extracted"),
                    static_cast<int>(ExtractMode::Field));
}

void LanguageController::extractParameter() {
    extractWithKind("refactor.extract.parameter", QStringLiteral("Extract parameter:"), QStringLiteral("param"),
                    static_cast<int>(ExtractMode::Parameter));
}

void LanguageController::extractMethod() {
    extractWithKind("refactor.extract.function", QStringLiteral("Extract method:"), QStringLiteral("extractedMethod"),
                    static_cast<int>(ExtractMode::Method));
}

void LanguageController::extractWithKind(std::string_view lspKindPrefix, const QString& promptLabel,
                                         const QString& defaultName, int modeInt) {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    fl::LspRange range{caretPosition(), caretPosition()};
    if (const auto sel = m_services.host.selection()) {
        range = fl::LspRange{positionAt(sel->begin), positionAt(sel->end)};
    }
    const std::string kindPrefix(lspKindPrefix);
    start<std::vector<fl::CodeActionItem>>(
        m_actionSlot,
        [&server = m_services.server, path, range](std::stop_token token) {
            return fl::requestCodeActions(server, path, range, {}, std::move(token));
        },
        [this, path, kindPrefix, promptLabel, defaultName, modeInt](Result<std::vector<fl::CodeActionItem>> actions) {
            if (actions) {
                for (fl::CodeActionItem& action : *actions) {
                    if (action.kind.starts_with(kindPrefix)) {
                        if (!action.edit.empty()) {
                            applyWorkspaceEdit(action.edit, qs(action.title));
                        }
                        if (!action.commandName.empty()) {
                            executeServerCommand(std::move(action.commandName), std::move(action.commandArguments),
                                                 qs(action.title));
                        }
                        return;
                    }
                }
            }
            const auto snapshot = m_services.host.snapshot();
            if (!snapshot) {
                return;
            }
            const auto expr =
                findTargetExpression(*snapshot, m_services.host.caretOffset(), m_services.host.selection());
            if (!expr) {
                emit statusMessage(QStringLiteral("Select an expression to extract"));
                return;
            }
            const ExpressionSpan target = *expr;
            m_customRenameCallback = [this, path, target, modeInt](const QString& chosenName) {
                const auto snap = m_services.host.snapshot();
                if (!snap || m_services.host.documentPath() != path) {
                    return;
                }
                const std::string ident = chosenName.trimmed().toStdString();
                if (ident.empty()) {
                    return;
                }
                const bool csharp = isCSharpPath(path);
                const auto mode = static_cast<ExtractMode>(modeInt);
                std::vector<fl::ByteEdit> edits;
                if (mode == ExtractMode::Variable || mode == ExtractMode::Constant) {
                    const std::string kw = mode == ExtractMode::Constant ? (csharp ? "const var " : "constexpr auto ")
                                                                         : (csharp ? "var " : "auto ");
                    const std::string decl = kw + ident + " = " + target.text + ";\n" + target.lineIndent;
                    edits.push_back(fl::ByteEdit{.range = {target.lineBegin, target.lineBegin}, .text = decl});
                    edits.push_back(fl::ByteEdit{.range = target.range, .text = ident});
                } else if (mode == ExtractMode::Field) {
                    const ByteOffset insertAt =
                        findEnclosingFunctionLineStart(*snap, target.range.begin).value_or(target.lineBegin);
                    const std::string decl =
                        (csharp ? "private var " : "auto ") + ident + " = " + target.text + ";\n" + target.lineIndent;
                    edits.push_back(fl::ByteEdit{.range = {insertAt, insertAt}, .text = decl});
                    edits.push_back(fl::ByteEdit{.range = target.range, .text = ident});
                } else if (mode == ExtractMode::Parameter) {
                    if (const auto closeParen = findEnclosingParameterListEnd(*snap, target.range.begin)) {
                        const bool hasExistingParams =
                            closeParen->value() > 0 && snap->byteAt(ByteOffset{closeParen->value() - 1}) != '(';
                        const std::string paramDecl =
                            (hasExistingParams ? ", " : "") + std::string(csharp ? "object " : "auto ") + ident;
                        edits.push_back(fl::ByteEdit{.range = {*closeParen, *closeParen}, .text = paramDecl});
                        edits.push_back(fl::ByteEdit{.range = target.range, .text = ident});
                    } else {
                        emit statusMessage(QStringLiteral("No enclosing function parameter list found"));
                        return;
                    }
                } else if (mode == ExtractMode::Method) {
                    const ByteOffset fnStart =
                        findEnclosingFunctionLineStart(*snap, target.range.begin).value_or(target.lineBegin);
                    const bool isStatement = target.text.find(';') != std::string::npos;
                    const std::string helper = (csharp ? "private auto " : "auto ") + ident + "() {\n    " +
                                               (isStatement ? target.text : ("return " + target.text + ";")) +
                                               "\n}\n\n";
                    edits.push_back(fl::ByteEdit{.range = {fnStart, fnStart}, .text = helper});
                    edits.push_back(fl::ByteEdit{.range = target.range, .text = ident + "()"});
                }
                std::ranges::stable_sort(
                    edits, [](const fl::ByteEdit& a, const fl::ByteEdit& b) { return a.range.begin < b.range.begin; });
                const ByteOffset nextCaret = fl::mapThroughEdits(target.range.begin, edits);
                if (auto applied = m_services.host.applyEdits(edits, "Extract", nextCaret); !applied) {
                    report(applied.error(), "Extract");
                }
            };
            m_rename->startWithPrompt(promptLabel, defaultName, m_services.host.caretScreenRect(), false);
        });
}

void LanguageController::inlineSymbol() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const fl::LspRange range{caretPosition(), caretPosition()};
    start<std::vector<fl::CodeActionItem>>(
        m_actionSlot,
        [&server = m_services.server, path, range](std::stop_token token) {
            return fl::requestCodeActions(server, path, range, {}, std::move(token));
        },
        [this, path](Result<std::vector<fl::CodeActionItem>> actions) {
            if (actions) {
                for (fl::CodeActionItem& action : *actions) {
                    if (action.kind.starts_with("refactor.inline")) {
                        if (!action.edit.empty()) {
                            applyWorkspaceEdit(action.edit, qs(action.title));
                        }
                        if (!action.commandName.empty()) {
                            executeServerCommand(std::move(action.commandName), std::move(action.commandArguments),
                                                 qs(action.title));
                        }
                        return;
                    }
                }
            }
            const auto snapshot = m_services.host.snapshot();
            if (!snapshot || m_services.host.documentPath() != path) {
                return;
            }
            const ByteOffset caret = snapshot->clip(m_services.host.caretOffset());
            const auto lineInfo = snapshot->lineInfo(snapshot->lineAt(caret));
            if (!lineInfo) {
                return;
            }
            const std::string line = snapshot->text(lineInfo->content);
            const std::size_t eqPos = line.find('=');
            if (eqPos == std::string::npos || (eqPos + 1 < line.size() && line[eqPos + 1] == '=')) {
                emit statusMessage(QStringLiteral("Place caret on a variable declaration line to inline"));
                return;
            }
            std::size_t nameEnd = eqPos;
            while (nameEnd > 0 && (line[nameEnd - 1] == ' ' || line[nameEnd - 1] == '\t')) {
                --nameEnd;
            }
            std::size_t nameStart = nameEnd;
            while (nameStart > 0 && fl::isIdentifierByte(line[nameStart - 1])) {
                --nameStart;
            }
            const std::string varName = line.substr(nameStart, nameEnd - nameStart);
            std::size_t rhsStart = eqPos + 1;
            while (rhsStart < line.size() && (line[rhsStart] == ' ' || line[rhsStart] == '\t')) {
                ++rhsStart;
            }
            std::size_t rhsEnd = line.size();
            while (rhsEnd > rhsStart &&
                   (line[rhsEnd - 1] == ';' || line[rhsEnd - 1] == ' ' || line[rhsEnd - 1] == '\t')) {
                --rhsEnd;
            }
            const std::string rhs = line.substr(rhsStart, rhsEnd - rhsStart);
            if (varName.empty() || rhs.empty()) {
                emit statusMessage(QStringLiteral("Cannot parse variable declaration to inline"));
                return;
            }
            const auto fullLineRange = [](const text::LineInfo& info) {
                const std::uint64_t brk = info.lineBreak == text::LineBreak::CrLf   ? 2U
                                          : info.lineBreak == text::LineBreak::None ? 0U
                                                                                    : 1U;
                return text::ByteRange{info.content.begin, ByteOffset{info.content.end.value() + brk}};
            };
            const text::ByteRange withBreak = fullLineRange(*lineInfo);
            std::vector<fl::ByteEdit> edits;
            edits.push_back(fl::ByteEdit{.range = withBreak, .text = {}});
            const std::string restText = snapshot->text(text::ByteRange{withBreak.end, snapshot->size()});
            std::size_t pos = 0;
            while ((pos = restText.find(varName, pos)) != std::string::npos) {
                const bool beforeOk = pos == 0 || !fl::isIdentifierByte(restText[pos - 1]);
                const std::size_t afterIdx = pos + varName.size();
                const bool afterOk = afterIdx >= restText.size() || !fl::isIdentifierByte(restText[afterIdx]);
                if (beforeOk && afterOk) {
                    const ByteOffset bStart{withBreak.end.value() + pos};
                    const ByteOffset bEnd{withBreak.end.value() + afterIdx};
                    edits.push_back(fl::ByteEdit{.range = {bStart, bEnd}, .text = rhs});
                }
                pos = afterIdx;
            }
            if (auto applied = m_services.host.applyEdits(edits, "Inline", lineInfo->content.begin); !applied) {
                report(applied.error(), "Inline");
            }
        });
}

void LanguageController::changeSignature() {
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot) {
        return;
    }
    auto parsed = parseSignatureAtLine(*snapshot, m_services.host.caretOffset());
    if (!parsed) {
        emit statusMessage(QStringLiteral("No function signature found at caret"));
        return;
    }
    QDialog dialog(m_services.host.editorWidget());
    dialog.setObjectName(QStringLiteral("changeSignatureDialog"));
    dialog.setWindowTitle(QStringLiteral("Change Signature"));
    dialog.setModal(true);
    dialog.setMinimumWidth(460);

    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* returnEdit = new QLineEdit(qs(parsed->returnType), &dialog);
    auto* nameEdit = new QLineEdit(qs(parsed->functionName), &dialog);
    form->addRow(QStringLiteral("Return type:"), returnEdit);
    form->addRow(QStringLiteral("Name:"), nameEdit);
    layout->addLayout(form);

    auto* paramList = new QListWidget(&dialog);
    for (const std::string& p : parsed->parameters) {
        auto* item = new QListWidgetItem(qs(p), paramList);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    layout->addWidget(new QLabel(QStringLiteral("Parameters (double-click to edit):"), &dialog));
    layout->addWidget(paramList);

    auto* btnRow = new QHBoxLayout();
    auto* addBtn = new QPushButton(QStringLiteral("Add"), &dialog);
    auto* removeBtn = new QPushButton(QStringLiteral("Remove"), &dialog);
    auto* upBtn = new QPushButton(QStringLiteral("Move Up"), &dialog);
    auto* downBtn = new QPushButton(QStringLiteral("Move Down"), &dialog);
    btnRow->addWidget(addBtn);
    btnRow->addWidget(removeBtn);
    btnRow->addWidget(upBtn);
    btnRow->addWidget(downBtn);
    layout->addLayout(btnRow);

    connect(addBtn, &QPushButton::clicked, &dialog, [paramList] {
        auto* item = new QListWidgetItem(QStringLiteral("int newParam"), paramList);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        paramList->setCurrentItem(item);
    });
    connect(removeBtn, &QPushButton::clicked, &dialog,
            [paramList] { delete paramList->takeItem(paramList->currentRow()); });
    connect(upBtn, &QPushButton::clicked, &dialog, [paramList] {
        const int row = paramList->currentRow();
        if (row > 0) {
            QListWidgetItem* item = paramList->takeItem(row);
            paramList->insertItem(row - 1, item);
            paramList->setCurrentItem(item);
        }
    });
    connect(downBtn, &QPushButton::clicked, &dialog, [paramList] {
        const int row = paramList->currentRow();
        if (row >= 0 && row + 1 < paramList->count()) {
            QListWidgetItem* item = paramList->takeItem(row);
            paramList->insertItem(row + 1, item);
            paramList->setCurrentItem(item);
        }
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    frost::ui::installDialogChrome(&dialog, true);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    QStringList newParams;
    for (int i = 0; i < paramList->count(); ++i) {
        const QString text = paramList->item(i)->text().trimmed();
        if (!text.isEmpty()) {
            newParams.push_back(text);
        }
    }
    const std::string retStr = returnEdit->text().trimmed().toStdString();
    const std::string nameStr = nameEdit->text().trimmed().toStdString();
    const std::string joinedParams = newParams.join(QStringLiteral(", ")).toStdString();
    const std::string replacement = parsed->prefixIndent + (retStr.empty() ? "" : (retStr + " ")) + nameStr + "(" +
                                    joinedParams + ")" + parsed->suffixAfterCloseParen;
    const fl::ByteEdit edit{.range = parsed->fullRange, .text = replacement};
    if (auto applied = m_services.host.applyEdits(std::span<const fl::ByteEdit>(&edit, 1), "Change Signature",
                                                  parsed->fullRange.begin);
        !applied) {
        report(applied.error(), "Change Signature");
    }
}

void LanguageController::safeDelete() {
    if (!hasDocument()) {
        return;
    }
    const std::string path = m_services.host.documentPath();
    const fl::LspPosition position = caretPosition();
    start<std::vector<fl::Location>>(
        m_actionSlot,
        [&server = m_services.server, path, position](std::stop_token token) {
            return fl::requestReferences(server, path, position, false, std::move(token));
        },
        [this, path, position](Result<std::vector<fl::Location>> refs) {
            const auto snapshot = m_services.host.snapshot();
            if (!snapshot || m_services.host.documentPath() != path) {
                return;
            }
            const auto lineInfo = snapshot->lineInfo(LineIndex{position.line});
            if (!lineInfo) {
                return;
            }
            const std::uint64_t brk = lineInfo->lineBreak == text::LineBreak::CrLf   ? 2U
                                      : lineInfo->lineBreak == text::LineBreak::None ? 0U
                                                                                     : 1U;
            const text::ByteRange deleteRange{lineInfo->content.begin, ByteOffset{lineInfo->content.end.value() + brk}};
            std::vector<fl::Location> externalUsages;
            if (refs) {
                for (const fl::Location& loc : *refs) {
                    if (fl::documentKey(loc.path) != fl::documentKey(path) || loc.range.start.line != position.line) {
                        externalUsages.push_back(loc);
                    }
                }
            }
            const auto performDelete = [this, path, deleteRange] {
                if (m_services.host.documentPath() != path) {
                    return;
                }
                const fl::ByteEdit edit{.range = deleteRange, .text = {}};
                if (auto applied = m_services.host.applyEdits(std::span<const fl::ByteEdit>(&edit, 1), "Safe Delete",
                                                              deleteRange.begin);
                    !applied) {
                    report(applied.error(), "Safe Delete");
                }
            };
            if (externalUsages.empty()) {
                performDelete();
                return;
            }
            m_choiceActions = {[this, path, position] { findUsagesAt(path, position); }, performDelete, [] {}};
            m_choice->start(QStringLiteral("Safe Delete: %1 usage(s) found").arg(externalUsages.size()),
                            {QStringLiteral("View %1 Usages").arg(externalUsages.size()),
                             QStringLiteral("Delete Anyway"), QStringLiteral("Cancel")},
                            m_services.host.caretScreenRect(), {true, false, false});
        });
}

void LanguageController::moveSymbol() {
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot) {
        return;
    }
    const std::string currentPath = m_services.host.documentPath();
    const ByteOffset caret = snapshot->clip(m_services.host.caretOffset());
    const auto lineInfo = snapshot->lineInfo(snapshot->lineAt(caret));
    if (!lineInfo) {
        return;
    }
    const std::string line = snapshot->text(lineInfo->content);
    m_choiceActions = {[this, currentPath, lineInfo = *lineInfo, line] {
                           const std::size_t slash = currentPath.find_last_of("/\\");
                           const std::string dir = slash == std::string::npos ? "." : currentPath.substr(0, slash + 1);
                           const std::string ext = isCSharpPath(currentPath) ? ".cs" : ".hpp";
                           const std::string targetFile = dir + "MovedSymbol" + ext;
                           const std::string content = (ext == ".hpp" ? "#pragma once\n\n" : "") + line + "\n";
                           if (auto written = m_services.fs.writeFileAtomic(targetFile, content); !written) {
                               report(written.error(), "Move symbol");
                               return;
                           }
                           const std::uint64_t brk = lineInfo.lineBreak == text::LineBreak::CrLf   ? 2U
                                                     : lineInfo.lineBreak == text::LineBreak::None ? 0U
                                                                                                   : 1U;
                           const fl::ByteEdit removeLine{
                               .range = {lineInfo.content.begin, ByteOffset{lineInfo.content.end.value() + brk}},
                               .text = {}};
                           (void)m_services.host.applyEdits(std::span<const fl::ByteEdit>(&removeLine, 1),
                                                            "Move Symbol", lineInfo.content.begin);
                           m_services.host.openLocation(targetFile, fl::LspPosition{});
                       },
                       [this] { gotoRelatedFile(); }};
    m_choice->start(QStringLiteral("Move"),
                    {QStringLiteral("Move Line/Symbol to Dedicated File"),
                     QStringLiteral("Switch to Related File (Header / Source)")},
                    m_services.host.caretScreenRect(), {true, false});
}

} // namespace frost::ui::lang
