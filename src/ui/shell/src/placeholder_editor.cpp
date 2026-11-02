// SPDX-License-Identifier: Apache-2.0
//
// The placeholder editor (frost/ui/shell/placeholder_editor.hpp): a read-only QPlainTextEdit over
// a document loaded by frost::text::loadTextFile.

#include "frost/ui/shell/placeholder_editor.hpp"

#include "frost/text/text_file.hpp"

#include <QFont>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>

#include <algorithm>
#include <string>
#include <utility>

namespace frost::ui {

namespace {

// The status bar's encoding token: "UTF-8", "UTF-8 BOM", "windows-1252".
std::string encodingToken(const text::FormatStatus& status) {
    std::string token(status.encodingName);
    if (status.bom) {
        token += " BOM";
    }
    return token;
}

class PlaceholderEditor final : public IEditorView {
public:
    PlaceholderEditor(std::string path, const text::LoadedText& loaded, IEditorHost& host)
        : m_path(std::move(path)), m_host(host), m_view(new QPlainTextEdit()) {
        const text::FormatStatus format = text::formatStatus(loaded.encoding, loaded.eol);
        m_status.encoding = encodingToken(format);
        m_status.lineSeparator = std::string(format.lineSeparatorName);
        m_status.readOnly = true;
        m_view->setObjectName(QStringLiteral("placeholderEditor"));
        m_view->setReadOnly(true);
        // Keep the caret visible and movable in a read-only view (Go to Line, the status bar).
        m_view->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
        m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        const std::string utf8 = loaded.buffer.snapshot().text();
        m_view->setPlainText(QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())));
        QObject::connect(m_view, &QPlainTextEdit::cursorPositionChanged, m_view, [this] { onCursorMoved(); });
        QObject::connect(m_view, &QPlainTextEdit::selectionChanged, m_view, [this] { onCursorMoved(); });
    }

    PlaceholderEditor(const PlaceholderEditor&) = delete ("an editor view has identity");
    PlaceholderEditor& operator=(const PlaceholderEditor&) = delete ("an editor view has identity");

    ~PlaceholderEditor() override {
        if (m_view != nullptr) {
            // Disconnect first: the lambdas capture this.
            QObject::disconnect(m_view, nullptr, nullptr, nullptr);
            delete m_view.data();
        }
    }

    [[nodiscard]] QWidget* widget() override { return m_view; }
    [[nodiscard]] const std::string& path() const override { return m_path; }
    [[nodiscard]] EditorStatus status() const override { return m_status; }

    void goToLine(int line, int column) override {
        QTextDocument* document = m_view->document();
        const int blocks = document->blockCount();
        const QTextBlock block = document->findBlockByNumber(std::clamp(line, 1, std::max(blocks, 1)) - 1);
        QTextCursor cursor(block);
        const int length = std::max(block.length() - 1, 0);
        cursor.setPosition(block.position() + std::clamp(column, 1, length + 1) - 1);
        m_view->setTextCursor(cursor);
        m_view->centerCursor();
    }

    [[nodiscard]] EditorViewState viewState() const override {
        // Without wrapping, QPlainTextEdit scrolls by blocks: the value is the first visible line.
        return EditorViewState{m_status.line, m_status.column, m_view->verticalScrollBar()->value()};
    }

    void restoreViewState(const EditorViewState& state) override {
        goToLine(state.line, state.column);
        m_view->verticalScrollBar()->setValue(state.topLine);
    }

    [[nodiscard]] input::ContextKeys contextKeys() const override {
        input::ContextKeys keys{input::ContextKey::EditorReadOnly};
        keys.set(input::ContextKey::EditorHasSelection, m_view->textCursor().hasSelection());
        return keys;
    }

private:
    void onCursorMoved() {
        const QTextCursor cursor = m_view->textCursor();
        m_status.line = cursor.blockNumber() + 1;
        m_status.column = cursor.positionInBlock() + 1;
        m_status.selectedCharacters = static_cast<int>(cursor.selectionEnd() - cursor.selectionStart());
        m_host.editorStatusChanged(*this);
    }

    std::string m_path;
    IEditorHost& m_host;
    QPointer<QPlainTextEdit> m_view;
    EditorStatus m_status;
};

} // namespace

Result<std::unique_ptr<IEditorView>> PlaceholderEditorFactory::open(const std::string& path, IEditorHost& host) {
    text::LoadOptions options;
    options.maxFileBytes = kMaxBytes;
    auto loaded = text::loadTextFile(m_fs, path, options);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    return std::unique_ptr<IEditorView>(std::make_unique<PlaceholderEditor>(path, *loaded, host));
}

} // namespace frost::ui
