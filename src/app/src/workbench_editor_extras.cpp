// SPDX-License-Identifier: Apache-2.0
// R15 editor extras: live bounded Markdown preview, registered in the production action system.
#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/ui/editor/markdown_preview.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/find/find_popup.hpp"
#include <QLineEdit>
#include <QToolButton>
#include <QDialog>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
namespace frost::app {
void Workbench::registerEditorExtrasActions() {
    const auto bookmark = [this](std::string id, std::string name, int direction) {
        input::ActionSpec spec;
        spec.id = std::move(id);
        spec.name = std::move(name);
        spec.group = "Navigate";
        spec.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
        spec.enabled = [this](const input::ActionContext&) { return currentView() != nullptr; };
        spec.handler = [this, direction](const input::ActionContext&, const input::InvokeInfo&) {
            if (auto* view = currentView()) {
                if (direction == 0) view->editor().toggleBookmark();
                else (void)view->editor().navigateBookmark(direction > 0);
            }
        };
        if (auto added = m_registry.add(std::move(spec)); !added)
            log::warn("workbench", "Bookmark action: {}", added.error().describe());
    };
    bookmark("editor.toggleBookmark", "Toggle Bookmark", 0);
    bookmark("editor.nextBookmark", "Next Bookmark", 1);
    bookmark("editor.previousBookmark", "Previous Bookmark", -1);
    input::ActionSpec todos;
    todos.id = "editor.showTodos";
    todos.name = "Scan TODO and FIXME in Project";
    todos.group = "View";
    todos.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
    todos.enabled = [this](const input::ActionContext&) { return m_find && !m_projectRoot.empty(); };
    todos.handler = [this](const input::ActionContext&, const input::InvokeInfo&) {
        if (!m_find || m_projectRoot.empty()) return;
        auto* popup = m_find->showPopup(false);
        if (!popup) return;
        popup->scopeButton(search::ScopeKind::Directory)->click();
        popup->directoryField()->setText(QString::fromStdString(m_projectRoot));
        popup->queryField()->setText(QStringLiteral("\\b(TODO|FIXME)\\b"));
        popup->regexToggle()->setChecked(true);
        popup->caseToggle()->setChecked(true);
        popup->wordsToggle()->setChecked(false);
        popup->fileMaskToggle()->setChecked(false);
        popup->searchNow();
        (void)m_find->openInFindWindow(*popup);
    };
    if (auto added = m_registry.add(std::move(todos)); !added)
        log::warn("workbench", "TODO scan action: {}", added.error().describe());
    input::ActionSpec freeze;
    freeze.id = "help.openFreezeReport";
    freeze.name = "Open Freeze Report...";
    freeze.group = "Help";
    freeze.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
    freeze.enabled = [this](const input::ActionContext&) { return m_window != nullptr; };
    freeze.handler = [this](const input::ActionContext&, const input::InvokeInfo&) {
        if (!m_window) return;
        const QPointer<Workbench> owner(this);
        const auto path = QFileDialog::getOpenFileName(m_window, QStringLiteral("Open Freeze Report"), {}, QStringLiteral("Freeze reports (*.json)"));
        if (!owner || path.isEmpty()) return;
        const auto status = m_services.fs.status(path.toStdString());
        if (!status || status->type != io::FileType::Regular || status->size > 8 * 1024 * 1024) {
            notify(QStringLiteral("Freeze report"), QStringLiteral("Choose a regular JSON report no larger than 8 MiB."), ui::NotificationKind::Error);
            return;
        }
        const auto bytes = m_services.fs.readFileSnapshot(path.toStdString(), 8 * 1024 * 1024);
        if (!bytes || bytes->size() > 8 * 1024 * 1024) {
            notify(QStringLiteral("Freeze report"), QStringLiteral("Cannot read the selected report."), ui::NotificationKind::Error);
            return;
        }
        const auto json = QJsonDocument::fromJson(QByteArray::fromStdString(*bytes));
        const auto report = json.object();
        if (!json.isObject() || report.value("schema").toString() != QStringLiteral("frost-freeze-report/1") ||
            !report.value("samples").isArray() || !report.value("modules").isArray()) {
            notify(QStringLiteral("Freeze report"), QStringLiteral("Unsupported or malformed freeze report."), ui::NotificationKind::Error);
            return;
        }
        auto* dialog = new QDialog(m_window);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("Freeze Report - ") + path);
        dialog->resize(900, 650);
        auto* layout = new QVBoxLayout(dialog);
        auto* summary = new QLabel(QStringLiteral("Duration: %1 ms - Samples: %2 - Modules: %3\nRaw recorded stacks and module identities below; no symbolizer or report-supplied path is executed.")
            .arg(report.value("duration_ms").toDouble()).arg(report.value("samples").toArray().size()).arg(report.value("modules").toArray().size()), dialog);
        summary->setTextFormat(Qt::PlainText);
        summary->setWordWrap(true);
        layout->addWidget(summary);
        auto* content = new QPlainTextEdit(dialog);
        content->setObjectName(QStringLiteral("freezeReportContent"));
        content->setReadOnly(true);
        content->setPlainText(QString::fromStdString(*bytes)); // Preserve uint64 addresses/unknown fields exactly.
        layout->addWidget(content);
        ui::installDialogChrome(dialog, true);
        dialog->show();
    };
    if (auto added = m_registry.add(std::move(freeze)); !added)
        log::warn("workbench", "Freeze report action: {}", added.error().describe());
    input::ActionSpec preview;
    preview.id = "editor.markdownPreview";
    preview.name = "Preview Markdown";
    preview.group = "View";
    preview.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
    preview.enabled = [this](const input::ActionContext&) {
        auto* view = currentView();
        return m_window && view && view->editor().syntaxLanguage() == syntax::LanguageId::Markdown;
    };
    preview.handler = [this](const input::ActionContext&, const input::InvokeInfo&) {
        auto* view = currentView();
        if (!m_window || !view)
            return;
        auto* dialog = new QDialog(m_window);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("Markdown Preview"));
        dialog->resize(800, 650);
        auto* layout = new QVBoxLayout(dialog);
        auto* notice = new QLabel(QStringLiteral("Embedded resources and raw HTML are disabled. Web links open only when clicked."), dialog);
        notice->setWordWrap(true);
        layout->addWidget(notice);
        auto* browser = new ui::MarkdownPreview(dialog);
        layout->addWidget(browser);
        const QPointer<DocumentEditorView> source(view);
        const auto refresh = [source, browser] {
            if (!source)
                return;
            const auto snapshot = source->editor().editor().text();
            if (snapshot.size().value() > 512 * 1024) {
                browser->setPlainText(QStringLiteral("Markdown preview is limited to 512 KiB. The source remains editable."));
                return;
            }
            (void)browser->setSourceMarkdown(QString::fromStdString(snapshot.text()));
        };
        auto* timer = new QTimer(dialog);
        timer->setSingleShot(true);
        timer->setInterval(150);
        connect(timer, &QTimer::timeout, dialog, refresh);
        connect(&view->editor(), &ui::EditorView::textChanged, dialog, [timer] { timer->start(); });
        connect(view, &QObject::destroyed, dialog, &QDialog::close);
        refresh();
        ui::installDialogChrome(dialog, true);
        dialog->show();
    };
    if (auto added = m_registry.add(std::move(preview)); !added)
        log::warn("workbench", "Markdown preview action: {}", added.error().describe());
}
}
