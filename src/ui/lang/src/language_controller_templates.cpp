// SPDX-License-Identifier: Apache-2.0
// Live template picker and linked Tab stops use the shared Qt-free snippet/template engine.
#include "frost/ui/lang/language_controller.hpp"

#include "language_controller_internal.hpp"
#include <QDate>
#include <QInputDialog>
#include <QPointer>

namespace frost::ui::lang {
namespace fl = ::frost::lang;
namespace {
std::string indentSnippet(std::string_view raw, std::string_view base, FormattingSettings format) {
    const std::string unit = format.insertSpaces ? std::string(std::clamp(format.tabSize, 1U, 16U), ' ') : "\t";
    std::string result;
    for (std::size_t i = 0; i < raw.size();) {
        result += raw[i++];
        if (result.back() != '\n')
            continue;
        result += base;
        while (raw.substr(i, 4) == "    ") {
            result += unit;
            i += 4;
        }
    }
    return result;
}
} // namespace
void LanguageController::insertLiveTemplate() {
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot || !capabilities().liveTemplates)
        return;
    const auto identity = m_services.host.structureIdentity();
    const auto version = m_services.host.documentVersion();
    const auto language = capabilities().languageId;
    const auto caret = m_services.host.caretOffset();
    const auto target = m_services.host.selection().value_or(text::ByteRange{caret, caret});
    std::vector<QString> titles;
    m_choiceActions.clear();
    for (const auto& definition : fl::builtinTemplates()) {
        if (definition.kind != fl::TemplateKind::Live || definition.language != language)
            continue;
        titles.push_back(detail::qs(definition.label));
        m_choiceActions.emplace_back([this, definition, identity, version, target] {
            fl::TemplateVariables variables;
            if (definition.body.find("${AUTHOR}") != std::string_view::npos) {
                QPointer<LanguageController> owner(this);
                bool accepted = false;
                const auto author = QInputDialog::getText(
                    m_services.host.editorWidget(), QStringLiteral("Template author"),
                    QStringLiteral("Author (leave blank to omit a name):"), QLineEdit::Normal, {}, &accepted);
                if (!owner || !accepted)
                    return;
                variables["AUTHOR"] = author.toUtf8().toStdString();
                variables["DATE"] = QDate::currentDate().toString(Qt::ISODate).toStdString();
                if (variables["AUTHOR"].find("*/") != std::string::npos ||
                    variables["AUTHOR"].find_first_of("\r\n") != std::string::npos) {
                    unavailable(QStringLiteral("Author must be one comment-safe line"));
                    return;
                }
            }
            if (m_services.host.structureIdentity() != identity || m_services.host.documentVersion() != version) {
                unavailable(QStringLiteral("Document changed; choose the template again"));
                return;
            }
            insertAuthoredTemplateAt(target, definition.body, variables, "Insert Live Template");
        });
    }
    if (titles.empty()) {
        unavailable(QStringLiteral("No templates for this language"));
        return;
    }
    m_choice->start(QStringLiteral("Insert Live Template"), titles, m_services.host.caretScreenRect());
}
void LanguageController::insertAuthoredTemplateAt(text::ByteRange range, std::string_view body,
                                                  const fl::TemplateVariables& variables, std::string_view label) {
    auto expanded = fl::expandTemplate(body, variables);
    if (!expanded) {
        report(expanded.error(), label);
        return;
    }
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot)
        return;
    std::string indent;
    if (const auto line = snapshot->lineInfo(snapshot->lineAt(range.begin))) {
        const auto before = snapshot->text({line->content.begin, range.begin});
        indent = before.substr(0, before.find_first_not_of(" \t"));
    }
    insertSnippetAt(range, indentSnippet(expanded->snippet, indent, m_services.host.formattingOptions()), label);
}
void LanguageController::beginLiveTemplate(const fl::ParsedSnippet& parsed, ByteOffset base) {
    m_tabStops.clear();
    m_liveTemplate.reset();
    const auto snapshot = m_services.host.snapshot();
    if (!snapshot || parsed.tabStops.empty() || parsed.budgetExceeded)
        return;
    m_liveTemplate = fl::LiveTemplateSession{parsed, 0};
    m_templateBase = base;
    m_templateDocumentSize = snapshot->size().value();
    m_templateIdentity = m_services.host.structureIdentity();
    const auto& first = parsed.tabStops.front();
    const text::ByteRange range{ByteOffset{base.value() + first.begin}, ByteOffset{base.value() + first.end}};
    m_services.host.select(range);
    if (first.index == 0)
        m_liveTemplate.reset();
    else
        m_tabStops.push_back(range);
}
bool LanguageController::advanceLiveTemplate() {
    const auto snapshot = m_services.host.snapshot();
    auto cancel = [&] {
        m_liveTemplate.reset();
        m_tabStops.clear();
    };
    if (!snapshot || !m_liveTemplate || m_services.host.structureIdentity() != m_templateIdentity) {
        cancel();
        return false;
    }
    const auto delta =
        static_cast<std::int64_t>(snapshot->size().value()) - static_cast<std::int64_t>(m_templateDocumentSize);
    const auto length = static_cast<std::int64_t>(m_liveTemplate->expected.text.size()) + delta;
    const auto caret = m_services.host.caretOffset().value();
    if (length < 0 || length > 256 * 1024 || m_templateBase.value() + length > snapshot->size().value() ||
        caret < m_templateBase.value() || caret > m_templateBase.value() + length) {
        cancel();
        return false;
    }
    const auto edited =
        snapshot->text({m_templateBase, ByteOffset{m_templateBase.value() + static_cast<std::uint64_t>(length)}});
    auto advanced =
        fl::advanceTemplate(*m_liveTemplate, edited, static_cast<std::uint32_t>(caret - m_templateBase.value()));
    if (!advanced) {
        cancel();
        return false;
    }
    std::vector<fl::ByteEdit> edits;
    for (const auto& edit : advanced->edits)
        edits.push_back(
            {{ByteOffset{m_templateBase.value() + edit.begin}, ByteOffset{m_templateBase.value() + edit.end}},
             edit.text});
    if (!edits.empty()) {
        auto applied = m_services.host.applyEdits(edits, "Linked template placeholders", std::nullopt);
        if (!applied) {
            report(applied.error(), "Linked template placeholders");
            cancel();
            return true;
        }
    }
    if (!advanced->selection) {
        cancel();
        return true;
    }
    const auto next = *advanced->selection;
    const text::ByteRange range{ByteOffset{m_templateBase.value() + next.begin},
                                ByteOffset{m_templateBase.value() + next.end}};
    m_services.host.select(range);
    if (next.index == 0)
        cancel();
    else {
        m_liveTemplate = std::move(advanced->next);
        m_tabStops = {range};
        const auto after = m_services.host.snapshot();
        m_templateDocumentSize = after ? after->size().value() : 0;
    }
    return true;
}
} // namespace frost::ui::lang
