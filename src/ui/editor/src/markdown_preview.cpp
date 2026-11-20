// SPDX-License-Identifier: Apache-2.0
// UX-5: preview existing Markdown without implicit local file, image or network access.
#include "frost/ui/editor/markdown_preview.hpp"
#include <QDesktopServices>
#include <QTextDocument>
#include <QUrl>
namespace frost::ui {
MarkdownPreview::MarkdownPreview(QWidget* parent) : QTextBrowser(parent) {
    setObjectName(QStringLiteral("markdownPreview"));
    setOpenLinks(false);
    setOpenExternalLinks(false);
    connect(this, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.isRelative() && url.path().isEmpty() && url.hasFragment())
            scrollToAnchor(url.fragment());
        else if (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http"))
            QDesktopServices::openUrl(url); // Only this explicit user activation opens a browser.
    });
}
bool MarkdownPreview::setSourceMarkdown(const QString& text) {
    if (text.size() > 512 * 1024) {
        setPlainText(tr("Markdown preview is limited to 512 KiB. The source remains editable."));
        return false;
    }
    document()->clear();
    document()->setBaseUrl(QUrl());
    document()->setMarkdown(text, QTextDocument::MarkdownFeatures{QTextDocument::MarkdownDialectGitHub,
                                                                  QTextDocument::MarkdownNoHTML});
    return true;
}
QVariant MarkdownPreview::loadResource(int, const QUrl&) { return {}; }
}
