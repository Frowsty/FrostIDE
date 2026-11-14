// SPDX-License-Identifier: Apache-2.0
//
// LanguageController code generation and editing assistance (FND-3, ADR 0078): Generate (clangd's
// MemberwiseConstructor, SpecialMembers and OverridePureVirtuals tweaks, plus captured AST member
// generators), Optimize Imports/Includes (clangd's include cleaner, or the server's
// organize-imports action), and the editor templates where language_capabilities.hpp allows them:
// Surround With, Complete Current Statement, Live Templates and Postfix Templates, each with a set
// per language. Nothing here generates members from line text.

#include "frost/lang/clangd_actions.hpp"
#include "frost/lang/code_assist.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "language_controller_internal.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::ui::lang {

namespace fl = ::frost::lang;
using detail::qs;

namespace {

std::string_view trimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/*
 * The start of the expression a postfix template applies to: the operand that ends at `dot` in
 * `line` (identifiers joined by '.', "->" and "::", with balanced (...) and [...] groups), e.g.
 * "items.size()" in "total += items.size().if". npos when there is none or a group is unbalanced.
 * O(dot).
 */
std::size_t postfixOperandStart(std::string_view line, std::size_t dot) {
    std::size_t i = dot;
    int depth = 0;
    while (i > 0) {
        const char c = line[i - 1];
        if (depth > 0) {
            if (c == ')' || c == ']') {
                ++depth;
            } else if (c == '(' || c == '[') {
                --depth;
            }
            --i;
            continue;
        }
        if (fl::isIdentifierByte(c) || c == '.') {
            --i;
        } else if (c == ')' || c == ']') {
            ++depth;
            --i;
        } else if (i >= 2 && ((c == ':' && line[i - 2] == ':') || (c == '>' && line[i - 2] == '-'))) {
            i -= 2;
        } else {
            break;
        }
    }
    if (depth != 0 || i == dot || line[i] == '.' || line[i] == ':' || line[i] == '-') {
        return std::string_view::npos;
    }
    return i;
}

struct Template {
    QString label;
    std::string body;
};

// Insert Live Template's set for a language (language_capabilities.hpp: cpp and csharp).
std::vector<Template> liveTemplatesFor(std::string_view languageId) {
    if (languageId == "csharp") {
        return {
            {QStringLiteral("for - index loop"), "for (int ${1:i} = 0; ${1:i} < ${2:count}; ${1:i}++)\n{\n    $0\n}"},
            {QStringLiteral("foreach - collection loop"), "foreach (var ${1:item} in ${2:collection})\n{\n    $0\n}"},
            {QStringLiteral("if - condition"), "if (${1:condition})\n{\n    $0\n}"},
            {QStringLiteral("ife - if/else"), "if (${1:condition})\n{\n    $2\n}\nelse\n{\n    $0\n}"},
            {QStringLiteral("try - try/catch block"), "try\n{\n    $1\n}\ncatch (${2:Exception} ${3:e})\n{\n    $0\n}"},
            {QStringLiteral("class - class definition"), "public class ${1:ClassName}\n{\n    $0\n}"},
            {QStringLiteral("prop - auto property"), "public ${1:int} ${2:PropertyName} { get; set; }$0"},
            {QStringLiteral("cw - Console.WriteLine"), "Console.WriteLine(${1:value});$0"},
            {QStringLiteral("todo - TODO comment"), "// TODO: $0"},
        };
    }
    if (languageId == "cpp") {
        return {
            {QStringLiteral("for - index loop"),
             "for (std::size_t ${1:i} = 0; ${1:i} < ${2:count}; ++${1:i}) {\n    $0\n}"},
            {QStringLiteral("foreach - range loop"), "for (auto&& ${1:item} : ${2:collection}) {\n    $0\n}"},
            {QStringLiteral("rfor - reverse loop"),
             "for (std::size_t ${1:i} = ${2:count}; ${1:i}-- > 0;) {\n    $0\n}"},
            {QStringLiteral("if - condition"), "if (${1:condition}) {\n    $0\n}"},
            {QStringLiteral("ife - if/else"), "if (${1:condition}) {\n    $2\n} else {\n    $0\n}"},
            {QStringLiteral("try - try/catch block"),
             "try {\n    $1\n} catch (const std::exception& ${2:e}) {\n    $0\n}"},
            {QStringLiteral("class - class definition"), "class ${1:ClassName} {\npublic:\n    $0\n};"},
            {QStringLiteral("struct - struct definition"), "struct ${1:StructName} {\n    $0\n};"},
            {QStringLiteral("cout - print to stdout"), "std::cout << ${1:value} << '\\n';$0"},
            {QStringLiteral("todo - TODO comment"), "// TODO: $0"},
            {QStringLiteral("main - entry point"), "int main(int argc, char* argv[]) {\n    $0\n    return 0;\n}"},
            {QStringLiteral("guard - #pragma once"), "#pragma once\n$0"},
        };
    }
    return {};
}

struct Wrapper {
    QString label;
    std::string before;
    std::string after;
};

// Surround With's set for a language.
std::vector<Wrapper> surroundTemplatesFor(std::string_view languageId) {
    if (languageId == "csharp") {
        return {
            {QStringLiteral("if (...) { ... }"), "if (${1:condition})\n{\n    ", "\n}$0"},
            {QStringLiteral("if (...) { ... } else { ... }"), "if (${1:condition})\n{\n    ",
             "\n}\nelse\n{\n    $0\n}"},
            {QStringLiteral("while (...) { ... }"), "while (${1:condition})\n{\n    ", "\n}$0"},
            {QStringLiteral("try { ... } catch { ... }"), "try\n{\n    ",
             "\n}\ncatch (${1:Exception} ${2:e})\n{\n    $0\n}"},
            {QStringLiteral("#region ... #endregion"), "#region ${1:Region}\n", "\n#endregion$0"},
        };
    }
    if (languageId == "cpp") {
        return {
            {QStringLiteral("if (...) { ... }"), "if (${1:condition}) {\n    ", "\n}$0"},
            {QStringLiteral("if (...) { ... } else { ... }"), "if (${1:condition}) {\n    ", "\n} else {\n    $0\n}"},
            {QStringLiteral("for (...) { ... }"), "for (${1:std::size_t i = 0; i < n; ++i}) {\n    ", "\n}$0"},
            {QStringLiteral("while (...) { ... }"), "while (${1:condition}) {\n    ", "\n}$0"},
            {QStringLiteral("try { ... } catch (...) { ... }"), "try {\n    ",
             "\n} catch (${1:const std::exception& e}) {\n    $0\n}"},
            {QStringLiteral("#ifdef ... / #endif"), "#ifdef ${1:MACRO}\n", "\n#endif$0"},
            {QStringLiteral("#pragma region ... / #pragma endregion"), "#pragma region ${1:Region}\n",
             "\n#pragma endregion$0"},
            {QStringLiteral("namespace { ... }"), "namespace ${1:detail} {\n", "\n} // namespace ${1:detail}$0"},
        };
    }
    return {};
}

} // namespace

void LanguageController::generateCode() {
    if (!hasDocument()) {
        return;
    }
    if (m_cppSemanticCommand) {
        unavailable(QStringLiteral("Wait for the active semantic command"));
        return;
    }
    std::shared_ptr<SemanticApply> semantic;
    if (capabilities().clangdTweaks && m_cppSemanticBegin) {
        auto captured = m_cppSemanticBegin();
        if (!captured) {
            report(captured.error(), "Generate");
            return;
        }
        semantic = std::make_shared<SemanticApply>(std::move(*captured));
    }
    if (!capabilities().clangdTweaks) {
        unavailable(QStringLiteral("Generate requires the generate engine for this language (REF-9)"));
        return;
    }
    const std::string path = m_services.host.documentPath();
    const fl::LspRange range{caretPosition(), caretPosition()};
    start<std::vector<fl::CodeActionItem>>(
        m_actionSlot,
        [&server = m_services.server, path, range](std::stop_token token) {
            return fl::requestCodeActions(server, path, range, {}, std::move(token));
        },
        [this, semantic](Result<std::vector<fl::CodeActionItem>> actions) {
            if (!actions) {
                report(actions.error(), "Generate");
                return;
            }
            std::vector<QString> titles;
            std::vector<QString> disabled;
            m_choiceActions.clear();
            // What clangd offers here, under its own titles.
            for (std::string_view id : {"MemberwiseConstructor", "SpecialMembers", "OverridePureVirtuals"}) {
                if (const fl::CodeActionItem* tweak = fl::findTweak(*actions, id)) {
                    titles.push_back(qs(tweak->title));
                    disabled.emplace_back();
                    m_choiceActions.emplace_back(
                        [this, action = *tweak, semantic]() mutable { runCodeAction(std::move(action), semantic); });
                }
            }
            // App-owned AST generators remain visible and name the missing composition when absent.
            const QString reason = m_cppAstRefactoring && capabilities().clangdTweaks
                                       ? QString{}
                                       : QStringLiteral("Requires the generate engine (REF-9)");
            const std::array generators{
                std::pair{QStringLiteral("Getters and Setters..."), &LanguageController::generateGettersSetters},
                std::pair{QStringLiteral("Equality Members..."), &LanguageController::generateEqualityMembers},
                std::pair{QStringLiteral("Formatting Members..."), &LanguageController::generateFormattingMembers},
                std::pair{QStringLiteral("Override Methods..."), &LanguageController::overrideMethods}};
            for (const auto& [label, handler] : generators) {
                titles.push_back(label);
                disabled.push_back(reason);
                m_choiceActions.emplace_back([this, label, reason, handler] {
                    if (reason.isEmpty())
                        (this->*handler)();
                    else
                        unavailable(label + QStringLiteral(": ") + reason);
                });
            }
            m_choice->start(QStringLiteral("Generate"), titles, m_services.host.caretScreenRect(), {},
                            std::move(disabled));
        });
}

void LanguageController::generateConstructor() {
    runClangdTweak("MemberwiseConstructor", QStringLiteral("Generate Constructor"),
                   QStringLiteral("Generate Constructor: clangd offers no constructor here (place the caret on a "
                                  "class with fields)"));
}

void LanguageController::implementMethods() {
    runClangdTweak("OverridePureVirtuals", QStringLiteral("Implement Methods"),
                   QStringLiteral("Implement Methods: clangd finds no pure virtual methods to override here"));
}

void LanguageController::overrideMethods() {
    if (capabilities().clangdTweaks && m_cppAstRefactoring) {
        m_cppAstRefactoring("generate-overrides");
        return;
    }
    unavailable(QStringLiteral("Override Methods requires the generate engine (REF-9); Implement Methods overrides "
                               "pure virtual methods"));
}

void LanguageController::generateGettersSetters() {
    if (capabilities().clangdTweaks && m_cppAstRefactoring) {
        m_cppAstRefactoring("generate-accessors");
        return;
    }
    unavailable(QStringLiteral("Getters and Setters require the generate engine (REF-9)"));
}

void LanguageController::generateEqualityMembers() {
    if (capabilities().clangdTweaks && m_cppAstRefactoring) {
        m_cppAstRefactoring("generate-equality");
        return;
    }
    unavailable(QStringLiteral("Equality Members require the generate engine (REF-9)"));
}

void LanguageController::generateFormattingMembers() {
    if (capabilities().clangdTweaks && m_cppAstRefactoring) {
        m_cppAstRefactoring("generate-formatting");
        return;
    }
    unavailable(QStringLiteral("Formatting Members require the generate engine (REF-9)"));
}

void LanguageController::surroundWith() {
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot) {
        return;
    }
    const LanguageCapabilities& caps = capabilities();
    if (!caps.surround) {
        unavailable(QStringLiteral("Surround With has no templates for this language yet (REF-13)"));
        return;
    }
    text::ByteRange targetRange;
    if (const auto sel = m_services.host.selection(); sel && !sel->empty()) {
        targetRange = *sel;
    } else {
        const auto lineInfo = snapshot->lineInfo(snapshot->lineAt(snapshot->clip(m_services.host.caretOffset())));
        if (!lineInfo) {
            return;
        }
        targetRange = lineInfo->content;
    }
    const std::string body = snapshot->text(targetRange);
    std::vector<QString> titles;
    m_choiceActions.clear();
    for (const Wrapper& w : surroundTemplatesFor(caps.languageId)) {
        titles.push_back(w.label);
        m_choiceActions.emplace_back([this, targetRange, snippet = w.before + body + w.after] {
            insertSnippetAt(targetRange, snippet, "Surround With");
        });
    }
    m_choice->start(QStringLiteral("Surround With"), titles, m_services.host.caretScreenRect());
}

void LanguageController::completeStatement() {
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot) {
        return;
    }
    if (!capabilities().completeStatement) {
        unavailable(QStringLiteral("Complete Current Statement is not available for this language yet"));
        return;
    }
    const ByteOffset caret = snapshot->clip(m_services.host.caretOffset());
    const auto lineInfo = snapshot->lineInfo(snapshot->lineAt(caret));
    if (!lineInfo) {
        return;
    }
    const std::string line = snapshot->text(lineInfo->content);
    const std::size_t indentEnd = std::min(line.find_first_not_of(" \t"), line.size());
    const std::string indent = line.substr(0, indentEnd);
    const std::string_view trimmed = trimView(line);
    const bool isTypeDecl = trimmed.starts_with("class ") || trimmed.starts_with("struct ");
    const bool isControl = trimmed.starts_with("if ") || trimmed.starts_with("if(") || trimmed.starts_with("for ") ||
                           trimmed.starts_with("for(") || trimmed.starts_with("while ") ||
                           trimmed.starts_with("while(") || trimmed.starts_with("switch ") ||
                           trimmed.starts_with("switch(") || trimmed.starts_with("else") ||
                           trimmed.starts_with("namespace ");
    const ByteOffset lineEnd = lineInfo->content.end;
    if ((isControl || isTypeDecl) && !trimmed.ends_with('{') && !trimmed.ends_with(';')) {
        const std::string closing = isTypeDecl ? (indent + "};") : (indent + "}");
        const std::string snippet = " {\n" + indent + "    $0\n" + closing;
        insertSnippetAt(text::ByteRange{lineEnd, lineEnd}, snippet, "Complete Statement");
        return;
    }
    if (!trimmed.empty() && !trimmed.starts_with('#') && !trimmed.ends_with(';') && !trimmed.ends_with('{') &&
        !trimmed.ends_with('}')) {
        insertSnippetAt(text::ByteRange{lineEnd, lineEnd}, ";\n" + indent + "$0", "Complete Statement");
        return;
    }
    insertSnippetAt(text::ByteRange{lineEnd, lineEnd}, "\n" + indent + "$0", "Complete Statement");
}

void LanguageController::optimizeImports() {
    if (!hasDocument()) {
        return;
    }
    const LanguageCapabilities& caps = capabilities();
    if (!caps.optimizeImports) {
        unavailable(QStringLiteral("Optimize Imports is not available for this language yet"));
        return;
    }
    const std::string path = m_services.host.documentPath();
    if (caps.clangdTweaks) {
        // clangd's include cleaner: the fixes of its unused/missing-includes diagnostics (REF-11).
        std::vector<fl::LiveDiagnostic> cleaner;
        if (m_services.diagnostics) {
            for (fl::LiveDiagnostic& diagnostic : m_services.diagnostics(path)) {
                if (fl::isIncludeCleanerDiagnostic(diagnostic)) {
                    cleaner.push_back(std::move(diagnostic));
                }
            }
        }
        if (cleaner.empty()) {
            emit statusMessage(QStringLiteral("Optimize Includes: clangd reports no unused or missing includes"));
            return;
        }
        fl::LspRange range = cleaner.front().lspRange;
        for (const fl::LiveDiagnostic& diagnostic : cleaner) {
            range.start = std::min(range.start, diagnostic.lspRange.start);
            range.end = std::max(range.end, diagnostic.lspRange.end);
        }
        start<std::vector<fl::CodeActionItem>>(
            m_actionSlot,
            [&server = m_services.server, path, range, cleaner](std::stop_token token) {
                return fl::requestCodeActions(server, path, range, cleaner, std::move(token));
            },
            [this](Result<std::vector<fl::CodeActionItem>> actions) {
                if (!actions) {
                    report(actions.error(), "Optimize Includes");
                    return;
                }
                if (auto fix = fl::pickIncludeCleanerFix(*actions)) {
                    applyWorkspaceEdit(fix->edit, QStringLiteral("Optimize Includes"));
                    return;
                }
                emit statusMessage(QStringLiteral("Optimize Includes: clangd offered no include fix"));
            });
        return;
    }
    // Other servers: their organize-imports source action for the whole document.
    const auto snapshot = m_services.host.snapshot();
    const fl::LspRange whole{{0, 0}, fl::toLspPosition(*snapshot, snapshot->size(), m_services.server.encoding())};
    start<std::vector<fl::CodeActionItem>>(
        m_actionSlot,
        [&server = m_services.server, path, whole](std::stop_token token) {
            return fl::requestCodeActions(server, path, whole, {}, std::move(token));
        },
        [this, server = qs(caps.serverName)](Result<std::vector<fl::CodeActionItem>> actions) {
            if (!actions) {
                report(actions.error(), "Optimize Imports");
                return;
            }
            for (fl::CodeActionItem& action : *actions) {
                if (action.kind.starts_with("source.organizeImports")) {
                    runCodeAction(std::move(action));
                    return;
                }
            }
            emit statusMessage(
                QStringLiteral("Optimize Imports: %1 offers no organize-imports action (LTR-5)").arg(server));
        });
}

void LanguageController::insertLiveTemplate() {
    if (!hasDocument()) {
        return;
    }
    const LanguageCapabilities& caps = capabilities();
    const std::vector<Template> templates =
        caps.liveTemplates ? liveTemplatesFor(caps.languageId) : std::vector<Template>{};
    if (templates.empty()) {
        unavailable(QStringLiteral("No live templates for this language yet (UX-6, REF-13)"));
        return;
    }
    const ByteOffset caret = m_services.host.caretOffset();
    const text::ByteRange target = m_services.host.selection().value_or(text::ByteRange{caret, caret});
    std::vector<QString> titles;
    m_choiceActions.clear();
    for (const Template& t : templates) {
        titles.push_back(t.label);
        m_choiceActions.emplace_back(
            [this, target, body = t.body] { insertSnippetAt(target, body, "Insert Live Template"); });
    }
    m_choice->start(QStringLiteral("Insert Live Template"), titles, m_services.host.caretScreenRect());
}

bool LanguageController::tryExpandPostfixCompletion() {
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot || !capabilities().postfix) {
        return false;
    }
    const ByteOffset caret = snapshot->clip(m_services.host.caretOffset());
    const auto lineInfo = snapshot->lineInfo(snapshot->lineAt(caret));
    if (!lineInfo) {
        return false;
    }
    const std::string beforeCaret = snapshot->text(text::ByteRange{lineInfo->content.begin, caret});
    const std::size_t dotPos = beforeCaret.rfind('.');
    if (dotPos == std::string::npos) {
        return false;
    }
    const std::string_view suffix = std::string_view(beforeCaret).substr(dotPos + 1);
    // Only the operand that ends at the dot, never the rest of the line.
    const std::size_t exprStart = postfixOperandStart(beforeCaret, dotPos);
    if (exprStart == std::string_view::npos) {
        return false;
    }
    const std::string expr = beforeCaret.substr(exprStart, dotPos - exprStart);
    const bool csharp = capabilities().languageId == "csharp";
    // Statement templates replace the operand with a statement: only when it starts the statement.
    const bool statementStart = trimView(std::string_view(beforeCaret).substr(0, exprStart)).empty();
    const bool statementSuffix = suffix == "var" || suffix == "const" || suffix == "if" || suffix == "notnull" ||
                                 suffix == "null" || suffix == "return" || suffix == "foreach";
    if (statementSuffix && !statementStart) {
        return false;
    }
    std::string snippet;
    if (suffix == "var") {
        snippet = (csharp ? "var " : "auto ") + std::string("${1:value} = ") + expr + ";$0";
    } else if (suffix == "const" && !csharp) {
        snippet = "const auto ${1:value} = " + expr + ";$0";
    } else if (suffix == "if") {
        snippet = "if (" + expr + ") {\n    $0\n}";
    } else if (suffix == "notnull") {
        snippet = "if (" + expr + (csharp ? " != null" : " != nullptr") + ") {\n    $0\n}";
    } else if (suffix == "null") {
        snippet = "if (" + expr + (csharp ? " == null" : " == nullptr") + ") {\n    $0\n}";
    } else if (suffix == "return") {
        snippet = "return " + expr + ";$0";
    } else if (suffix == "foreach") {
        snippet = csharp ? ("foreach (var ${1:item} in " + expr + ") {\n    $0\n}")
                         : ("for (auto&& ${1:item} : " + expr + ") {\n    $0\n}");
    } else if (suffix == "await") {
        snippet = (csharp ? "await " : "co_await ") + expr + "$0";
    } else {
        return false;
    }
    closeLookup();
    const ByteOffset replaceBegin{lineInfo->content.begin.value() + exprStart};
    insertSnippetAt(text::ByteRange{replaceBegin, caret}, snippet, "Postfix Completion");
    return true;
}

} // namespace frost::ui::lang
