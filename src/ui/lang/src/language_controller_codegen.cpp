// SPDX-License-Identifier: Apache-2.0
//
// LanguageController code generation and editing assistance (Milestone 2): Generate popup
// (Constructor, Getters/Setters, Override/Implement Methods, Equality, Formatting), Surround With,
// Complete Current Statement, Optimize Imports/Includes, Live Templates, and Postfix Completion.

#include "frost/lang/code_assist.hpp"
#include "frost/lang/symbols.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "language_controller_internal.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::ui::lang {

namespace fl = ::frost::lang;
using detail::isCSharpPath;
using detail::qs;

namespace {

struct ClassField {
    std::string type;
    std::string name;
    std::string cleanName;
};

struct ScannedClass {
    std::string name = "MyClass";
    std::string indent = "    ";
    std::vector<ClassField> fields;
};

std::string stripFieldPrefix(std::string_view raw) {
    if (raw.starts_with("m_") && raw.size() > 2) {
        return std::string(raw.substr(2));
    }
    if (raw.starts_with("_") && raw.size() > 1) {
        return std::string(raw.substr(1));
    }
    return std::string(raw);
}

std::string capitalizeFirst(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    std::string out(text);
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

std::string_view trimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<ClassField> parseFieldDeclarationLine(std::string_view rawLine) {
    std::string_view line = trimView(rawLine);
    if (line.empty() || line.starts_with("//") || line.starts_with("#") || line.starts_with("public:") ||
        line.starts_with("private:") || line.starts_with("protected:") || line.find('(') != std::string_view::npos ||
        !line.ends_with(';')) {
        return std::nullopt;
    }
    line.remove_suffix(1);
    if (const std::size_t eq = line.find('='); eq != std::string_view::npos) {
        line = trimView(line.substr(0, eq));
    }
    if (line.ends_with('}') && line.find('{') != std::string_view::npos) {
        line = trimView(line.substr(0, line.find('{')));
    }
    for (std::string_view mod : {"private ", "protected ", "public ", "internal ", "readonly ", "mutable "}) {
        if (line.starts_with(mod)) {
            line = trimView(line.substr(mod.size()));
        }
    }
    if (line.starts_with("static ") || line.starts_with("constexpr ") || line.starts_with("using ") ||
        line.starts_with("typedef ") || line.starts_with("return ")) {
        return std::nullopt;
    }
    std::size_t nameStart = line.size();
    while (nameStart > 0 && fl::isIdentifierByte(line[nameStart - 1])) {
        --nameStart;
    }
    if (nameStart == 0 || nameStart == line.size()) {
        return std::nullopt;
    }
    const std::string_view nameView = line.substr(nameStart);
    const std::string_view typeView = trimView(line.substr(0, nameStart));
    if (typeView.empty() || typeView == "class" || typeView == "struct" || typeView == "namespace") {
        return std::nullopt;
    }
    return ClassField{
        .type = std::string(typeView), .name = std::string(nameView), .cleanName = stripFieldPrefix(nameView)};
}

ScannedClass scanEnclosingClass(const text::Snapshot& snapshot, ByteOffset caret) {
    ScannedClass result;
    const std::uint32_t caretLine = snapshot.lineAt(snapshot.clip(caret)).value();
    if (const auto caretInfo = snapshot.lineInfo(LineIndex{caretLine})) {
        const std::string caretText = snapshot.text(caretInfo->content);
        const std::size_t firstNonWs = caretText.find_first_not_of(" \t");
        if (firstNonWs != std::string::npos && firstNonWs > 0) {
            result.indent = caretText.substr(0, firstNonWs);
        }
    }
    std::uint32_t classStartLine = 0;
    for (std::uint32_t line = caretLine + 1; line > 0; --line) {
        const auto info = snapshot.lineInfo(LineIndex{line - 1});
        if (!info) {
            continue;
        }
        const std::string text = snapshot.text(info->content);
        const std::string_view trimmed = trimView(text);
        for (std::string_view kw : {"class ", "struct "}) {
            if (const std::size_t pos = trimmed.find(kw); pos != std::string_view::npos) {
                std::string_view after = trimView(trimmed.substr(pos + kw.size()));
                std::size_t len = 0;
                while (len < after.size() && fl::isIdentifierByte(after[len])) {
                    ++len;
                }
                if (len > 0 && trimmed.find(';') == std::string_view::npos) {
                    result.name = std::string(after.substr(0, len));
                    classStartLine = line - 1;
                    line = 1;
                    break;
                }
            }
        }
    }
    const std::uint32_t maxLine = std::min<std::uint32_t>(snapshot.lineCount(), classStartLine + 120);
    for (std::uint32_t line = classStartLine + 1; line < maxLine; ++line) {
        if (const auto info = snapshot.lineInfo(LineIndex{line})) {
            const std::string text = snapshot.text(info->content);
            if (trimView(text).starts_with("};")) {
                break;
            }
            if (auto field = parseFieldDeclarationLine(text)) {
                result.fields.push_back(std::move(*field));
            }
        }
    }
    return result;
}

std::optional<std::vector<ClassField>> pickFieldsFromDialog(QWidget* parent, const QString& title,
                                                            const std::vector<ClassField>& fields) {
    if (fields.empty() || parent == nullptr || !parent->isVisible()) {
        return fields;
    }
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("pickFieldsDialog"));
    dialog.setWindowTitle(title);
    dialog.setModal(true);
    dialog.setMinimumWidth(360);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Select fields:"), &dialog));
    auto* list = new QListWidget(&dialog);
    for (const ClassField& f : fields) {
        auto* item = new QListWidgetItem(qs(f.type + " " + f.name), list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    layout->addWidget(list);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    frost::ui::installDialogChrome(&dialog, true);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    std::vector<ClassField> chosen;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->checkState() == Qt::Checked) {
            chosen.push_back(fields[static_cast<std::size_t>(i)]);
        }
    }
    return chosen;
}

} // namespace

void LanguageController::generateCode() {
    if (!hasDocument()) {
        return;
    }
    m_choiceActions = {
        [this] { generateConstructor(); },     [this] { generateGettersSetters(); },
        [this] { overrideMethods(); },         [this] { implementMethods(); },
        [this] { generateEqualityMembers(); }, [this] { generateFormattingMembers(); },
    };
    m_choice->start(QStringLiteral("Generate"),
                    {QStringLiteral("Constructor..."), QStringLiteral("Getters and Setters / Properties..."),
                     QStringLiteral("Override Methods..."), QStringLiteral("Implement Methods..."),
                     QStringLiteral("Equality Members (operator== / Equals & GetHashCode)"),
                     QStringLiteral("Formatting Members (operator<< / ToString)")},
                    m_services.host.caretScreenRect(), {true, false, false, false, false, false});
}

void LanguageController::generateConstructor() {
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot) {
        return;
    }
    const ScannedClass cls = scanEnclosingClass(*snapshot, m_services.host.caretOffset());
    const auto chosen =
        pickFieldsFromDialog(m_services.host.editorWidget(), QStringLiteral("Generate Constructor"), cls.fields);
    if (!chosen) {
        return;
    }
    const bool csharp = isCSharpPath(m_services.host.documentPath());
    std::string code = cls.indent;
    if (chosen->empty()) {
        code += csharp ? ("public " + cls.name + "()\n" + cls.indent + "{\n" + cls.indent + "}\n")
                       : (cls.name + "() = default;\n");
    } else if (csharp) {
        code += "public " + cls.name + "(";
        for (std::size_t i = 0; i < chosen->size(); ++i) {
            if (i > 0) {
                code += ", ";
            }
            code += (*chosen)[i].type + " " + (*chosen)[i].cleanName;
        }
        code += ")\n" + cls.indent + "{\n";
        for (const ClassField& f : *chosen) {
            code += cls.indent + "    this." + f.name + " = " + f.cleanName + ";\n";
        }
        code += cls.indent + "}\n";
    } else {
        if (chosen->size() == 1) {
            code += "explicit ";
        }
        code += cls.name + "(";
        for (std::size_t i = 0; i < chosen->size(); ++i) {
            if (i > 0) {
                code += ", ";
            }
            code += (*chosen)[i].type + " " + (*chosen)[i].cleanName;
        }
        code += ") : ";
        for (std::size_t i = 0; i < chosen->size(); ++i) {
            if (i > 0) {
                code += ", ";
            }
            code += (*chosen)[i].name + "(" + (*chosen)[i].cleanName + ")";
        }
        code += " {}\n";
    }
    const ByteOffset caret = snapshot->clip(m_services.host.caretOffset());
    const ByteOffset lineStart = snapshot->lineInfo(snapshot->lineAt(caret))->content.begin;
    insertSnippetAt(text::ByteRange{lineStart, lineStart}, code, "Generate Constructor");
}

void LanguageController::generateGettersSetters() {
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot) {
        return;
    }
    const ScannedClass cls = scanEnclosingClass(*snapshot, m_services.host.caretOffset());
    if (cls.fields.empty()) {
        emit statusMessage(QStringLiteral("No fields found in enclosing class/struct"));
        return;
    }
    const auto chosen = pickFieldsFromDialog(m_services.host.editorWidget(),
                                             QStringLiteral("Generate Getters and Setters"), cls.fields);
    if (!chosen || chosen->empty()) {
        return;
    }
    const bool csharp = isCSharpPath(m_services.host.documentPath());
    std::string code;
    for (const ClassField& f : *chosen) {
        if (csharp) {
            code += cls.indent + "public " + f.type + " " + capitalizeFirst(f.cleanName) + " { get => " + f.name +
                    "; set => " + f.name + " = value; }\n";
        } else {
            code += cls.indent + "[[nodiscard]] const " + f.type + "& get_" + f.cleanName +
                    "() const noexcept { return " + f.name + "; }\n";
            code +=
                cls.indent + "void set_" + f.cleanName + "(const " + f.type + "& value) { " + f.name + " = value; }\n";
        }
    }
    const ByteOffset caret = snapshot->clip(m_services.host.caretOffset());
    const ByteOffset lineStart = snapshot->lineInfo(snapshot->lineAt(caret))->content.begin;
    insertSnippetAt(text::ByteRange{lineStart, lineStart}, code, "Generate Getters and Setters");
}

void LanguageController::overrideMethods() {
    if (!hasDocument()) {
        return;
    }
    const bool csharp = isCSharpPath(m_services.host.documentPath());
    const ByteOffset caret = m_services.host.caretOffset();
    m_choiceActions = {
        [this, caret, csharp] {
            const std::string stub = csharp
                                         ? "public override string ToString()\n{\n    return ${1:base.ToString()};$0\n}"
                                         : "void ${1:methodName}() override {\n    $0\n}";
            insertSnippetAt(text::ByteRange{caret, caret}, stub, "Override Method");
        },
        [this, caret, csharp] {
            const std::string stub =
                csharp ? "public override bool Equals(object? obj)\n{\n    return ${1:base.Equals(obj)};$0\n}"
                       : "~${1:ClassName}() override = default;$0";
            insertSnippetAt(text::ByteRange{caret, caret}, stub, "Override Method");
        },
    };
    m_choice->start(QStringLiteral("Override Methods"),
                    {csharp ? QStringLiteral("ToString()") : QStringLiteral("Virtual member function..."),
                     csharp ? QStringLiteral("Equals(object? obj)") : QStringLiteral("Virtual destructor...")},
                    m_services.host.caretScreenRect(), {true, false});
}

void LanguageController::implementMethods() {
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
        [this](Result<std::vector<fl::CodeActionItem>> actions) {
            if (actions) {
                for (fl::CodeActionItem& item : *actions) {
                    if (item.title.find("implement") != std::string::npos ||
                        item.title.find("Implement") != std::string::npos) {
                        if (!item.edit.empty()) {
                            applyWorkspaceEdit(item.edit, qs(item.title));
                            return;
                        }
                    }
                }
            }
            const ByteOffset caret = m_services.host.caretOffset();
            insertSnippetAt(text::ByteRange{caret, caret}, "${1:void} ${2:implementMethod}() override {\n    $0\n}",
                            "Implement Method");
        });
}

void LanguageController::generateEqualityMembers() {
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot) {
        return;
    }
    const ScannedClass cls = scanEnclosingClass(*snapshot, m_services.host.caretOffset());
    const bool csharp = isCSharpPath(m_services.host.documentPath());
    std::string code;
    if (csharp) {
        code = cls.indent + "public override bool Equals(object? obj) => obj is " + cls.name + " other;\n" +
               cls.indent + "public override int GetHashCode() => base.GetHashCode();\n";
    } else {
        code = cls.indent + "bool operator==(const " + cls.name + "&) const = default;\n" + cls.indent +
               "bool operator!=(const " + cls.name + "&) const = default;\n";
    }
    const ByteOffset caret = snapshot->clip(m_services.host.caretOffset());
    const ByteOffset lineStart = snapshot->lineInfo(snapshot->lineAt(caret))->content.begin;
    insertSnippetAt(text::ByteRange{lineStart, lineStart}, code, "Generate Equality Members");
}

void LanguageController::generateFormattingMembers() {
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot) {
        return;
    }
    const ScannedClass cls = scanEnclosingClass(*snapshot, m_services.host.caretOffset());
    const bool csharp = isCSharpPath(m_services.host.documentPath());
    std::string code;
    if (csharp) {
        code = cls.indent + "public override string ToString() => \"" + cls.name + "\";\n";
    } else {
        code = cls.indent + "friend std::ostream& operator<<(std::ostream& os, const " + cls.name + "& obj) {\n" +
               cls.indent + "    (void)obj;\n" + cls.indent + "    return os << \"" + cls.name + "{}\";\n" +
               cls.indent + "}\n";
    }
    const ByteOffset caret = snapshot->clip(m_services.host.caretOffset());
    const ByteOffset lineStart = snapshot->lineInfo(snapshot->lineAt(caret))->content.begin;
    insertSnippetAt(text::ByteRange{lineStart, lineStart}, code, "Generate Formatting Members");
}

void LanguageController::surroundWith() {
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot) {
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
    struct Wrapper {
        QString label;
        std::string before;
        std::string after;
    };
    const std::vector<Wrapper> wrappers = {
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
    std::vector<QString> titles;
    m_choiceActions.clear();
    for (const Wrapper& w : wrappers) {
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
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot) {
        return;
    }
    const bool csharp = isCSharpPath(m_services.host.documentPath());
    const std::string_view prefix = csharp ? "using " : "#include";
    std::vector<fl::ByteEdit> edits;
    std::uint32_t lineIdx = 0;
    const std::uint32_t totalLines = snapshot->lineCount();
    while (lineIdx < totalLines) {
        const auto info = snapshot->lineInfo(LineIndex{lineIdx});
        if (!info || !trimView(snapshot->text(info->content)).starts_with(prefix)) {
            ++lineIdx;
            continue;
        }
        const ByteOffset blockStart = info->content.begin;
        ByteOffset blockEnd = info->content.end;
        std::vector<std::string> directives;
        while (lineIdx < totalLines) {
            const auto cur = snapshot->lineInfo(LineIndex{lineIdx});
            if (!cur) {
                break;
            }
            const std::string lineText = snapshot->text(cur->content);
            if (!trimView(lineText).starts_with(prefix)) {
                break;
            }
            directives.push_back(std::string(trimView(lineText)));
            blockEnd = cur->content.end;
            ++lineIdx;
        }
        if (directives.size() > 1) {
            std::ranges::sort(directives);
            const auto uniqueEnd = std::ranges::unique(directives);
            directives.erase(uniqueEnd.begin(), uniqueEnd.end());
            std::string joined;
            for (std::size_t i = 0; i < directives.size(); ++i) {
                if (i > 0) {
                    joined += '\n';
                }
                joined += directives[i];
            }
            if (joined != snapshot->text(text::ByteRange{blockStart, blockEnd})) {
                edits.push_back(fl::ByteEdit{.range = {blockStart, blockEnd}, .text = std::move(joined)});
            }
        }
    }
    if (edits.empty()) {
        emit statusMessage(QStringLiteral("Imports / includes are already optimized"));
        return;
    }
    const ByteOffset caret = fl::mapThroughEdits(m_services.host.caretOffset(), edits);
    if (auto applied = m_services.host.applyEdits(edits, "Optimize Imports", caret); !applied) {
        report(applied.error(), "Optimize Imports");
    }
}

void LanguageController::insertLiveTemplate() {
    if (!hasDocument()) {
        return;
    }
    const bool csharp = isCSharpPath(m_services.host.documentPath());
    const ByteOffset caret = m_services.host.caretOffset();
    const text::ByteRange target = m_services.host.selection().value_or(text::ByteRange{caret, caret});
    struct TemplateEntry {
        QString label;
        std::string body;
    };
    const std::vector<TemplateEntry> templates = {
        {QStringLiteral("for - index loop"),
         "for (std::size_t ${1:i} = 0; ${1:i} < ${2:count}; ++${1:i}) {\n    $0\n}"},
        {QStringLiteral("foreach - range loop"), csharp ? "foreach (var ${1:item} in ${2:collection})\n{\n    $0\n}"
                                                        : "for (auto&& ${1:item} : ${2:collection}) {\n    $0\n}"},
        {QStringLiteral("rfor - reverse loop"), "for (std::size_t ${1:i} = ${2:count}; ${1:i}-- > 0;) {\n    $0\n}"},
        {QStringLiteral("if - condition"), "if (${1:condition}) {\n    $0\n}"},
        {QStringLiteral("ife - if/else"), "if (${1:condition}) {\n    $2\n} else {\n    $0\n}"},
        {QStringLiteral("try - try/catch block"), "try {\n    $1\n} catch (const std::exception& ${2:e}) {\n    $0\n}"},
        {QStringLiteral("class - class definition"), "class ${1:ClassName} {\npublic:\n    $0\n};"},
        {QStringLiteral("struct - struct definition"), "struct ${1:StructName} {\n    $0\n};"},
        {QStringLiteral("ctor - default constructor"), "${1:ClassName}() = default;$0"},
        {QStringLiteral("prop - auto property"), "public ${1:int} ${2:PropertyName} { get; set; }$0"},
        {QStringLiteral("cw - console/stdout print"),
         csharp ? "Console.WriteLine(${1:value});$0" : "std::cout << ${1:value} << '\\n';$0"},
        {QStringLiteral("todo - TODO comment"), "// TODO(M2): $0"},
        {QStringLiteral("main - entry point"), "int main(int argc, char* argv[]) {\n    $0\n    return 0;\n}"},
        {QStringLiteral("guard - #pragma once"), "#pragma once\n$0"},
    };
    std::vector<QString> titles;
    m_choiceActions.clear();
    for (const TemplateEntry& t : templates) {
        titles.push_back(t.label);
        m_choiceActions.emplace_back(
            [this, target, body = t.body] { insertSnippetAt(target, body, "Insert Live Template"); });
    }
    m_choice->start(QStringLiteral("Insert Live Template"), titles, m_services.host.caretScreenRect());
}

bool LanguageController::tryExpandPostfixCompletion() {
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot) {
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
    const std::size_t exprStart = std::min(beforeCaret.find_first_not_of(" \t"), dotPos);
    const std::string expr = std::string(trimView(std::string_view(beforeCaret).substr(exprStart, dotPos - exprStart)));
    if (expr.empty()) {
        return false;
    }
    const bool csharp = isCSharpPath(m_services.host.documentPath());
    std::string snippet;
    if (suffix == "var") {
        snippet = (csharp ? "var " : "auto ") + std::string("${1:value} = ") + expr + ";$0";
    } else if (suffix == "const") {
        snippet = (csharp ? "const var " : "const auto ") + std::string("${1:value} = ") + expr + ";$0";
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
