// SPDX-License-Identifier: Apache-2.0
// UX-5: bounded Markdown preview. Resource loading is denied; links require explicit activation.
#pragma once
#include <QTextBrowser>
#include <QString>
namespace frost::ui {
// UI thread only. Qt's existing Markdown reader is used with raw HTML disabled; no new grammar.
class MarkdownPreview final : public QTextBrowser {
public:
    explicit MarkdownPreview(QWidget* parent = nullptr);
    // Refuses input over 512 KiB rather than parsing arbitrarily large documents on the GUI thread.
    [[nodiscard]] bool setSourceMarkdown(const QString& text);
protected:
    QVariant loadResource(int type, const QUrl& name) override;
};
}
