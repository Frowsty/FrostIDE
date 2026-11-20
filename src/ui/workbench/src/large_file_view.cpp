// SPDX-License-Identifier: Apache-2.0
// A paged UTF-8 preview with byte navigation and cancellable background line seeking.
#include "frost/ui/workbench/large_file_view.hpp"

#include "frost/text/large_file.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringDecoder>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <mutex>
#include <optional>
#include <stop_token>
namespace frost::ui {
namespace {
// Budget: B15. A first page never requires a whole-file line index. Runtime B15 acceptance
// remains deferred; the fixed page also stays below the existing long-line shaping guard.
constexpr std::uint64_t kPageBytes = io::IRandomAccessReader::kMaximumReadBytes;
static_assert(kPageBytes <= text::kLongLineCapBytes);
struct Page {
    std::uint64_t generation = 0, offset = 0, size = 0;
    int line = 0;
    std::string bytes;
    std::optional<Error> error;
};
struct Worker {
    std::mutex readerMutex, resultMutex;
    std::unique_ptr<io::IRandomAccessReader> reader;
    std::optional<Page> result;
};
class LargeFileView final : public IEditorView {
public:
    LargeFileView(io::IFileSystem& fs, std::string path, IEditorHost& host)
        : m_open(fs.randomAccessOpener()), m_path(std::move(path)), m_host(host), m_widget(new QWidget),
          m_label(new QLabel(m_widget)), m_text(new QPlainTextEdit(m_widget)), m_worker(std::make_shared<Worker>()) {
        m_widget->setObjectName(QStringLiteral("largeFileView"));
        auto* layout = new QVBoxLayout(m_widget);
        m_label->setObjectName(QStringLiteral("largeFileState"));
        m_label->setWordWrap(true);
        layout->addWidget(m_label);
        auto* controls = new QHBoxLayout;
        auto* previous = new QPushButton(QStringLiteral("Previous page"), m_widget);
        auto* next = new QPushButton(QStringLiteral("Next page"), m_widget);
        auto* offset = new QLineEdit(m_widget);
        offset->setObjectName(QStringLiteral("largeFileByteOffset"));
        offset->setPlaceholderText(QStringLiteral("Go to byte offset (0 based)"));
        auto* line = new QLineEdit(m_widget);
        line->setObjectName(QStringLiteral("largeFileLine"));
        line->setPlaceholderText(QStringLiteral("Go to line (LF/CRLF; scans in background)"));
        auto* cancel = new QPushButton(QStringLiteral("Cancel"), m_widget);
        cancel->setObjectName(QStringLiteral("largeFileCancel"));
        controls->addWidget(previous);
        controls->addWidget(next);
        controls->addWidget(offset);
        controls->addWidget(line);
        controls->addWidget(cancel);
        layout->addLayout(controls);
        m_text->setObjectName(QStringLiteral("largeFilePage"));
        m_text->setReadOnly(true);
        m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_text->setMaximumBlockCount(65537);
        layout->addWidget(m_text, 1);
        m_widget->setFocusProxy(m_text);
        QObject::connect(previous, &QPushButton::clicked, m_widget,
                         [this] { request(m_offset < kPageBytes ? 0 : m_offset - kPageBytes, 0); });
        QObject::connect(next, &QPushButton::clicked, m_widget, [this] {
            if (m_size > m_offset && m_size - m_offset > kPageBytes)
                request(m_offset + kPageBytes, 0);
        });
        QObject::connect(offset, &QLineEdit::returnPressed, m_widget, [this, offset] {
            bool ok = false;
            const auto value = offset->text().toULongLong(&ok);
            if (ok)
                request(value, 0);
        });
        QObject::connect(line, &QLineEdit::returnPressed, m_widget, [this, line] {
            bool ok = false;
            const int value = line->text().toInt(&ok);
            if (ok && value > 0)
                goToLine(value, 1);
        });
        QObject::connect(cancel, &QPushButton::clicked, m_widget, [this] {
            m_stop.request_stop();
            ++m_generation;
            m_label->setText(QStringLiteral("Large-file read cancelled. Choose a page or line to resume."));
        });
        auto* timer = new QTimer(m_widget);
        QObject::connect(timer, &QTimer::timeout, m_widget, [this] { receive(); });
        timer->start(20);
        request(0, 1);
    }
    ~LargeFileView() override {
        m_stop.request_stop();
        delete m_widget;
    }
    QWidget* widget() override { return m_widget; }
    const std::string& path() const override { return m_path; }
    EditorStatus status() const override {
        EditorStatus result;
        result.readOnly = true;
        result.line = std::max(1, m_line);
        result.encoding = "UTF-8 page preview";
        return result;
    }
    input::ContextKeys contextKeys() const override { return {input::ContextKey::EditorReadOnly}; }
    void goToLine(int line, int /*column*/) override { request(0, std::max(1, line)); }

private:
    void request(std::uint64_t offset, int targetLine) {
        m_stop.request_stop();
        m_stop = std::stop_source{};
        const auto token = m_stop.get_token();
        const auto generation = ++m_generation;
        m_text->clear();
        m_label->setText(QStringLiteral("Loading read-only large-file page... UTF-8 only; no highlighting or editing. "
                                        "Byte pages may begin/end within a line or UTF-8 sequence. Line seek supports "
                                        "LF/CRLF and scans on a worker."));
        auto worker = m_worker;
        const auto open = m_open;
        const auto path = m_path;
        QThreadPool::globalInstance()->start([worker, open, path, token, generation, offset, targetLine] {
            Page page;
            page.generation = generation;
            page.offset = offset;
            std::lock_guard readerLock(worker->readerMutex);
            if (token.stop_requested())
                return;
            if (!worker->reader) {
                auto opened = open(path);
                if (!opened)
                    page.error = opened.error();
                else
                    worker->reader = std::move(*opened);
            }
            if (worker->reader && !page.error) {
                page.size = worker->reader->size();
                if (targetLine > 0) {
                    page.offset = 0;
                    int currentLine = 1;
                    while (currentLine < targetLine && page.offset < page.size) {
                        auto bytes = worker->reader->read(page.offset, kPageBytes, token);
                        if (!bytes) {
                            page.error = bytes.error();
                            break;
                        }
                        std::size_t consumed = 0;
                        for (char c : *bytes) {
                            ++consumed;
                            if (c == '\n' && ++currentLine == targetLine)
                                break;
                        }
                        page.offset += consumed;
                    }
                    page.line = currentLine;
                }
                if (!page.error) {
                    auto bytes = worker->reader->read(page.offset, kPageBytes, token);
                    if (bytes)
                        page.bytes = std::move(*bytes);
                    else
                        page.error = bytes.error();
                }
            }
            if (token.stop_requested())
                return;
            std::lock_guard resultLock(worker->resultMutex);
            worker->result = std::move(page);
        });
    }
    void receive() {
        std::optional<Page> page;
        {
            std::lock_guard lock(m_worker->resultMutex);
            page = std::move(m_worker->result);
            m_worker->result.reset();
        }
        if (!page || page->generation != m_generation)
            return;
        if (page->error) {
            m_label->setText(QStringLiteral("Large-file page unavailable: ") +
                             QString::fromStdString(page->error->message()));
            return;
        }
        m_offset = page->offset;
        m_size = page->size;
        m_line = page->line;
        // Do not feed binary/UTF-16 data into the text widget. UTF-8 pages intentionally use
        // replacement glyphs at byte boundaries, as the persistent banner makes explicit.
        if (page->bytes.find('\0') != std::string::npos || (page->offset == 0 && page->bytes.size() >= 2 &&
                                                            ((static_cast<unsigned char>(page->bytes[0]) == 0xff &&
                                                              static_cast<unsigned char>(page->bytes[1]) == 0xfe) ||
                                                             (static_cast<unsigned char>(page->bytes[0]) == 0xfe &&
                                                              static_cast<unsigned char>(page->bytes[1]) == 0xff)))) {
            m_label->setText(
                QStringLiteral("Read-only large-file preview supports UTF-8 text only. Binary/UTF-16 page refused."));
            return;
        }
        // Validate the complete interior, permitting incomplete code points only at actual page
        // boundaries. Stateless decoding rejects a dangling sequence at EOF as well.
        std::string_view interior = page->bytes;
        std::size_t prefix = 0;
        if (page->offset > 0) {
            while (prefix < interior.size() && prefix < 3 &&
                   (static_cast<unsigned char>(interior[prefix]) & 0xc0) == 0x80)
                ++prefix;
            interior.remove_prefix(prefix);
        }
        QStringDecoder partialDecoder(QStringDecoder::Utf8);
        const QString partial =
            partialDecoder(QByteArrayView(interior.data(), static_cast<qsizetype>(interior.size())));
        static_cast<void>(partial);
        if (partialDecoder.hasError()) {
            m_label->setText(
                QStringLiteral("Read-only large-file preview: invalid UTF-8 inside page; encoding unsupported."));
            return;
        }
        if (page->offset + page->bytes.size() < page->size && !interior.empty()) {
            auto start = interior.size() - 1;
            while (start > 0 && (static_cast<unsigned char>(interior[start]) & 0xc0) == 0x80)
                --start;
            const auto lead = static_cast<unsigned char>(interior[start]);
            const std::size_t expected = lead >= 0xc2 && lead <= 0xdf   ? 2
                                         : lead >= 0xe0 && lead <= 0xef ? 3
                                         : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                                        : 1;
            if (expected > interior.size() - start)
                interior = interior.substr(0, start);
        }
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        const QString validated = decoder(QByteArrayView(interior.data(), static_cast<qsizetype>(interior.size())));
        if (decoder.hasError()) {
            m_label->setText(
                QStringLiteral("Read-only large-file preview: invalid UTF-8 inside page; encoding unsupported."));
            return;
        }
        static_cast<void>(validated);
        m_text->setPlainText(QString::fromUtf8(page->bytes.data(), static_cast<qsizetype>(page->bytes.size())));
        m_label->setText(
            QStringLiteral("Read-only UTF-8 byte page %1-%2 of %3 bytes. "
                           "Page edges may split lines/UTF-8 sequences; replacement glyphs are preview only. "
                           "No editing, highlighting or whole-file index. %4")
                .arg(m_offset)
                .arg(m_offset + page->bytes.size())
                .arg(m_size)
                .arg(m_line > 0 ? QStringLiteral("Starts at line %1.").arg(m_line)
                                : QStringLiteral("Global line unknown; use Go to line.")));
        m_host.editorStatusChanged(*this);
    }
    io::RandomAccessOpener m_open;
    std::string m_path;
    IEditorHost& m_host;
    QWidget* m_widget;
    QLabel* m_label;
    QPlainTextEdit* m_text;
    std::shared_ptr<Worker> m_worker;
    std::stop_source m_stop;
    std::uint64_t m_generation = 0, m_offset = 0, m_size = 0;
    int m_line = 1;
};
} // namespace
std::unique_ptr<IEditorView> makeLargeFileView(io::IFileSystem& fs, std::string path, IEditorHost& host) {
    return std::make_unique<LargeFileView>(fs, std::move(path), host);
}
} // namespace frost::ui
