// SPDX-License-Identifier: Apache-2.0
//
// DiffViewer (frost/ui/vcs/diff_viewer.hpp): the panes, the highlights, the scroll sync and F7.

#include "frost/ui/vcs/diff_viewer.hpp"

#include "frost/ui/frost_style.hpp"
#include "frost/ui/vcs/git_controller.hpp"

#include <QFontDatabase>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QTextBlock>

#include <algorithm>

namespace frost::ui {
namespace {

// Line `i` of `text` as shown: without its break, a lone '\r' as a space.
QString displayLine(std::string_view line) {
    if (!line.empty() && line.back() == '\n') {
        line.remove_suffix(1);
    }
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    QString out = QString::fromUtf8(line.data(), static_cast<qsizetype>(line.size()));
    out.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return out;
}

QString paneText(const diff::LineSequence& text) {
    QStringList lines;
    lines.reserve(static_cast<qsizetype>(text.size()));
    for (std::uint32_t i = 0; i < text.size(); ++i) {
        lines.push_back(displayLine(text.line(LineIndex{i})));
    }
    return lines.join(QLatin1Char('\n'));
}

// The UTF-16 column of byte `offset` on line `line` (clamped to the shown text).
int columnOf(const diff::LineSequence& text, LineIndex line, std::uint64_t offset) {
    const text::ByteRange range = text.lineRange(line);
    const std::uint64_t begin = range.begin.value();
    const std::uint64_t end = std::clamp<std::uint64_t>(offset, begin, range.end.value());
    const std::string_view head = text.bytes().substr(begin, end - begin);
    return static_cast<int>(std::min(displayLine(head).size(), displayLine(text.line(line)).size()));
}

QTextEdit::ExtraSelection lineTint(QPlainTextEdit& pane, int line, const QColor& color) {
    QTextEdit::ExtraSelection selection;
    selection.cursor = QTextCursor(pane.document()->findBlockByNumber(line));
    selection.format.setBackground(color);
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    return selection;
}

QPlainTextEdit* makePane(QWidget* parent) {
    auto* pane = new QPlainTextEdit(parent);
    pane->setReadOnly(true);
    pane->setLineWrapMode(QPlainTextEdit::NoWrap);
    pane->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    pane->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    return pane;
}

} // namespace

DiffColors DiffColors::current() {
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        return DiffColors{style->color(ColorKey::DiffInsertedBackground), style->color(ColorKey::DiffDeletedBackground),
                          style->color(ColorKey::DiffModifiedBackground),
                          style->color(ColorKey::DiffInnerChangeBackground)};
    }
    return DiffColors{QColor(0x2B, 0x3D, 0x30), QColor(0x40, 0x2F, 0x32), QColor(0x2A, 0x38, 0x50),
                      QColor(0x3F, 0x5A, 0x8A)};
}

DiffViewer::DiffViewer(QWidget* parent)
    : QWidget(parent), m_leftTitle(new QLabel(this)), m_rightTitle(new QLabel(this)), m_left(makePane(this)),
      m_right(makePane(this)) {
    setObjectName(QStringLiteral("diffViewer"));
    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_leftTitle, 0, 0);
    layout->addWidget(m_rightTitle, 0, 1);
    layout->addWidget(m_left, 1, 0);
    layout->addWidget(m_right, 1, 1);
    m_left->installEventFilter(this);
    m_right->installEventFilter(this);
    connect(m_left->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { syncFrom(vcs::DiffSide::Left); });
    connect(m_right->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { syncFrom(vcs::DiffSide::Right); });
}

DiffViewer::~DiffViewer() = default;

void DiffViewer::setDiff(vcs::SideBySideDiff diff, const QString& leftTitle, const QString& rightTitle) {
    m_diff = std::move(diff);
    m_leftTitle->setText(leftTitle);
    m_rightTitle->setText(rightTitle);
    m_syncing = true;
    m_left->setPlainText(paneText(m_diff.left()));
    m_right->setPlainText(paneText(m_diff.right()));
    m_syncing = false;
    applyHighlights();
    m_current.reset();
    if (!m_diff.blocks().empty()) {
        goToBlock(0);
    }
}

QString DiffViewer::leftTitle() const { return m_leftTitle->text(); }

QString DiffViewer::rightTitle() const { return m_rightTitle->text(); }

void DiffViewer::applyHighlights() {
    const DiffColors colors = DiffColors::current();
    QList<QTextEdit::ExtraSelection> left;
    QList<QTextEdit::ExtraSelection> right;
    for (const vcs::DiffBlock& block : m_diff.blocks()) {
        const bool replaced = block.kind() == vcs::LineChangeKind::Modified;
        for (LineIndex line = block.hunk.a.begin; line < block.hunk.a.end; ++line) {
            left.push_back(
                lineTint(*m_left, static_cast<int>(line.value()), replaced ? colors.modified : colors.deleted));
        }
        for (LineIndex line = block.hunk.b.begin; line < block.hunk.b.end; ++line) {
            right.push_back(
                lineTint(*m_right, static_cast<int>(line.value()), replaced ? colors.modified : colors.inserted));
        }
        // The changed words, cut at line ends.
        for (const diff::RangePair& pair : block.inner) {
            const auto words = [&](vcs::DiffSide side, const text::ByteRange& range,
                                   QList<QTextEdit::ExtraSelection>& out) {
                QPlainTextEdit& pane = side == vcs::DiffSide::Left ? *m_left : *m_right;
                const diff::LineSequence& text = m_diff.text(side);
                const diff::LineRange lines = block.lines(side);
                for (LineIndex line = lines.begin; line < lines.end; ++line) {
                    const text::ByteRange span = text.lineRange(line);
                    const std::uint64_t from = std::max(range.begin.value(), span.begin.value());
                    const std::uint64_t to = std::min(range.end.value(), span.end.value());
                    if (from >= to) {
                        continue;
                    }
                    const int c0 = columnOf(text, line, from);
                    const int c1 = columnOf(text, line, to);
                    if (c1 <= c0) {
                        continue;
                    }
                    const QTextBlock textBlock = pane.document()->findBlockByNumber(static_cast<int>(line.value()));
                    QTextEdit::ExtraSelection selection;
                    selection.cursor = QTextCursor(textBlock);
                    selection.cursor.setPosition(textBlock.position() + c0);
                    selection.cursor.setPosition(textBlock.position() + c1, QTextCursor::KeepAnchor);
                    selection.format.setBackground(colors.innerChange);
                    out.push_back(selection);
                }
            };
            words(vcs::DiffSide::Left, pair.a, left);
            words(vcs::DiffSide::Right, pair.b, right);
        }
    }
    m_left->setExtraSelections(left);
    m_right->setExtraSelections(right);
}

int DiffViewer::highlightCount(vcs::DiffSide side) const {
    return static_cast<int>((side == vcs::DiffSide::Left ? m_left : m_right)->extraSelections().size());
}

bool DiffViewer::nextChange() {
    const auto line = static_cast<std::uint32_t>(m_right->textCursor().blockNumber());
    std::optional<std::size_t> next = m_diff.blockAfter(vcs::DiffSide::Right, LineIndex{line});
    if (m_current && (!next || *next <= *m_current) && *m_current + 1 < m_diff.blocks().size()) {
        next = *m_current + 1; // Pure deletions share their right-side line with the next block's start.
    }
    if (!next) {
        return false;
    }
    goToBlock(*next);
    return true;
}

bool DiffViewer::previousChange() {
    const auto line = static_cast<std::uint32_t>(m_right->textCursor().blockNumber());
    std::optional<std::size_t> previous = m_diff.blockBefore(vcs::DiffSide::Right, LineIndex{line});
    if (m_current && (!previous || *previous >= *m_current)) {
        previous = *m_current > 0 ? std::optional<std::size_t>(*m_current - 1) : std::nullopt;
    }
    if (!previous) {
        return false;
    }
    goToBlock(*previous);
    return true;
}

void DiffViewer::goToBlock(std::size_t index) {
    const vcs::DiffBlock& block = m_diff.blocks()[index];
    m_current = index;
    const auto place = [](QPlainTextEdit& pane, LineIndex line) {
        const int last = std::max(0, pane.document()->blockCount() - 1);
        const QTextBlock target = pane.document()->findBlockByNumber(std::min(static_cast<int>(line.value()), last));
        QTextCursor cursor(target);
        pane.setTextCursor(cursor);
        pane.centerCursor();
    };
    m_syncing = true;
    place(*m_left, block.begin(vcs::DiffSide::Left));
    place(*m_right, block.begin(vcs::DiffSide::Right));
    m_syncing = false;
}

int DiffViewer::topLine(vcs::DiffSide side) const {
    return (side == vcs::DiffSide::Left ? m_left : m_right)->verticalScrollBar()->value();
}

void DiffViewer::scrollToLine(vcs::DiffSide side, int line) {
    (side == vcs::DiffSide::Left ? m_left : m_right)->verticalScrollBar()->setValue(line);
}

void DiffViewer::syncFrom(vcs::DiffSide side) {
    if (m_syncing) {
        return;
    }
    m_syncing = true;
    const vcs::DiffSide other = side == vcs::DiffSide::Left ? vcs::DiffSide::Right : vcs::DiffSide::Left;
    const auto top = static_cast<std::uint32_t>(std::max(0, topLine(side)));
    const LineIndex mapped = m_diff.mapLine(side, LineIndex{top});
    (other == vcs::DiffSide::Left ? m_left : m_right)->verticalScrollBar()->setValue(static_cast<int>(mapped.value()));
    m_syncing = false;
}

bool DiffViewer::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == m_left || watched == m_right) && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_F7 && (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
            nextChange();
            return true;
        }
        if (key->key() == Qt::Key_F7 && (key->modifiers() & ~Qt::KeypadModifier) == Qt::ShiftModifier) {
            previousChange();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void showDiffAgainst(DiffViewer& viewer, GitController& controller, DiffBase base, const std::string& absolutePath,
                     std::string workingCopy, std::function<void(Result<void>)> done) {
    const QPointer<DiffViewer> target(&viewer);
    auto shown = [target, base, working = std::move(workingCopy),
                  done = std::move(done)](Result<std::optional<std::string>> version) mutable {
        if (!target) {
            return;
        }
        const auto finish = [&done](Result<void> outcome) {
            if (done) {
                done(std::move(outcome));
            }
        };
        if (!version) {
            finish(std::unexpected(std::move(version).error()));
            return;
        }
        auto diff = vcs::SideBySideDiff::compute(version->value_or(std::string()), std::move(working));
        if (!diff) {
            finish(std::unexpected(std::move(diff).error()));
            return;
        }
        const QString left = base == DiffBase::Head ? QObject::tr("HEAD") : QObject::tr("Index");
        target->setDiff(std::move(*diff), version->has_value() ? left : left + QObject::tr(" (no such version)"),
                        QObject::tr("Working copy"));
        finish({});
    };
    if (base == DiffBase::Head) {
        controller.readHeadContent(absolutePath, std::move(shown));
    } else {
        controller.readIndexContent(absolutePath, std::move(shown));
    }
}

} // namespace frost::ui
