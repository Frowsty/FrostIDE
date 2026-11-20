// SPDX-License-Identifier: Apache-2.0
// Editor-backed side-by-side comparison, with normal history/find/edit commands and guarded Apply.
#include "frost/ui/vcs/diff_viewer.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/vcs/git_controller.hpp"
#include <QGridLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QPointer>
#include <QScrollBar>
#include <algorithm>

namespace frost::ui {
namespace {
std::unique_ptr<EditorKeyRouting> standaloneRouting(const EditorServices& services) {
    if (services.keys || services.actionHost) return {};
    auto active = input::readActiveLayout();
    const auto layout = active ? *active : input::simulatedLayout(input::SimulatedLayoutId::EnUs);
#ifdef _WIN32
    constexpr auto platform = input::KeyPlatform::Windows;
#else
    constexpr auto platform = input::KeyPlatform::X11;
#endif
    auto routing = EditorKeyRouting::createStandalone(layout, input::currentKeymapPlatform(), platform);
    return routing ? std::move(*routing) : nullptr;
}
}
DiffColors DiffColors::current() {
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        return DiffColors{style->color(ColorKey::DiffInsertedBackground), style->color(ColorKey::DiffDeletedBackground),
                          style->color(ColorKey::DiffModifiedBackground),
                          style->color(ColorKey::DiffInnerChangeBackground)};
    }
    return DiffColors{QColor(0x2B, 0x3D, 0x30), QColor(0x40, 0x2F, 0x32), QColor(0x2A, 0x38, 0x50),
                      QColor(0x3F, 0x5A, 0x8A)};
}

DiffViewer::DiffViewer(QWidget* parent, EditorServices services)
    : QWidget(parent), m_leftTitle(new QLabel(this)), m_rightTitle(new QLabel(this)),
      m_keys(standaloneRouting(services)), m_leftDocument(std::make_unique<text::Document>(text::TextBuffer{})),
      m_rightDocument(std::make_unique<text::Document>(text::TextBuffer{})),
      m_left(new EditorView(*m_leftDocument, "diff-left.cpp", [&] { if (m_keys) services.keys = m_keys.get(); return services; }(), {}, this)),
      m_right(new EditorView(*m_rightDocument, "diff-right.cpp", services, {}, this)),
      m_apply(new QPushButton(tr("Apply Working Copy"), this)), m_status(new QLabel(this)) {
    m_services = services;
    setObjectName(QStringLiteral("diffViewer"));
    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_leftTitle, 0, 0);
    layout->addWidget(m_rightTitle, 0, 1);
    layout->addWidget(m_left, 1, 0);
    layout->addWidget(m_right, 1, 1);
    layout->addWidget(m_status, 2, 0);
    layout->addWidget(m_apply, 2, 1);
    m_left->setFileInfo({.readOnly = true});
    m_right->setFileInfo({.readOnly = true});
    m_apply->hide();
    m_left->installEventFilter(this);
    m_right->installEventFilter(this);
    connect(m_left->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { syncFrom(vcs::DiffSide::Left); });
    connect(m_right->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { syncFrom(vcs::DiffSide::Right); });
    connect(m_right, &EditorView::textChanged, this, [this] { workingEdited(); });
    connect(m_apply, &QPushButton::clicked, this, [this] {
        if (auto applied = applyWorkingCopy(); !applied) m_status->setText(QString::fromStdString(applied.error().message()));
    });
}
DiffViewer::~DiffViewer() {
    // EditorViews borrow their Documents; destroy the child views before the models.
    delete m_left;
    delete m_right;
}
void DiffViewer::setDiff(vcs::SideBySideDiff diff, const QString& leftTitle, const QString& rightTitle) {
    if (hasWorkingEdits()) {
        m_status->setText(tr("Working copy changed elsewhere. Apply validates the source before writing; your comparison edits were retained."));
        return;
    }
    m_syncing = true;
    auto left = text::Document::fromUtf8(std::string(diff.left().bytes()));
    auto right = text::Document::fromUtf8(std::string(diff.right().bytes()));
    if (!left || !right) { m_syncing = false; return; }
    delete m_left;
    delete m_right;
    *m_leftDocument = std::move(*left);
    *m_rightDocument = std::move(*right);
    m_left = new EditorView(*m_leftDocument, "diff-left.cpp", m_services, {}, this);
    m_right = new EditorView(*m_rightDocument, "diff-right.cpp", m_services, {}, this);
    static_cast<QGridLayout*>(layout())->addWidget(m_left, 1, 0);
    static_cast<QGridLayout*>(layout())->addWidget(m_right, 1, 1);
    m_left->setFileInfo({.readOnly = true});
    m_right->setFileInfo({.readOnly = !m_applyHandler});
    m_left->installEventFilter(this);
    m_right->installEventFilter(this);
    connect(m_left->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { syncFrom(vcs::DiffSide::Left); });
    connect(m_right->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { syncFrom(vcs::DiffSide::Right); });
    connect(m_right, &EditorView::textChanged, this, [this] { workingEdited(); });
    m_originalWorking = std::string(diff.right().bytes());
    m_left->reloadFromDocument();
    m_right->reloadFromDocument();
    m_left->markSaved();
    m_right->markSaved();
    m_diff = std::move(diff);
    m_leftTitle->setText(leftTitle);
    m_rightTitle->setText(rightTitle);
    m_syncing = false;
    m_status->clear();
    applyHighlights();
    m_current.reset();
    if (!m_diff.blocks().empty()) goToBlock(0);
}
QString DiffViewer::leftTitle() const { return m_leftTitle->text(); }
QString DiffViewer::rightTitle() const { return m_rightTitle->text(); }
QString DiffViewer::leftText() const { return QString::fromStdString(m_leftDocument->committedText().text()); }
QString DiffViewer::rightText() const { return QString::fromStdString(m_rightDocument->committedText().text()); }
void DiffViewer::applyHighlights() {
    const auto colors = DiffColors::current();
    std::vector<EditorDecoration> left, right;
    for (const auto& block : m_diff.blocks()) {
        const bool modified = block.kind() == vcs::LineChangeKind::Modified;
        for (auto line = block.hunk.a.begin; line < block.hunk.a.end; ++line)
            left.push_back({.range = m_diff.left().lineRange(line), .style = DecorationStyle::Background,
                .color = modified ? colors.modified : colors.deleted});
        for (auto line = block.hunk.b.begin; line < block.hunk.b.end; ++line)
            right.push_back({.range = m_diff.right().lineRange(line), .style = DecorationStyle::Background,
                .color = modified ? colors.modified : colors.inserted});
        for (const auto& inner : block.inner) {
            left.push_back({.range = inner.a, .style = DecorationStyle::Background, .color = colors.innerChange});
            right.push_back({.range = inner.b, .style = DecorationStyle::Background, .color = colors.innerChange});
        }
    }
    m_left->setDecorations("vcs-diff", std::move(left));
    m_right->setDecorations("vcs-diff", std::move(right));
}
int DiffViewer::highlightCount(vcs::DiffSide side) const {
    return static_cast<int>((side == vcs::DiffSide::Left ? m_left : m_right)->decorations().size());
}
bool DiffViewer::nextChange() {
    auto next = m_diff.blockAfter(vcs::DiffSide::Right, LineIndex{static_cast<std::uint32_t>(m_right->status().line - 1)});
    if (m_current && (!next || *next <= *m_current) && *m_current + 1 < m_diff.blocks().size()) next = *m_current + 1;
    if (!next) return false;
    goToBlock(*next); return true;
}
bool DiffViewer::previousChange() {
    auto previous = m_diff.blockBefore(vcs::DiffSide::Right, LineIndex{static_cast<std::uint32_t>(m_right->status().line - 1)});
    if (m_current && (!previous || *previous >= *m_current)) previous = *m_current > 0 ? std::optional<std::size_t>(*m_current - 1) : std::nullopt;
    if (!previous) return false;
    goToBlock(*previous); return true;
}
void DiffViewer::goToBlock(std::size_t index) {
    m_current = index;
    m_syncing = true;
    m_left->goToLine(static_cast<int>(m_diff.blocks()[index].begin(vcs::DiffSide::Left).value()) + 1, 1);
    m_right->goToLine(static_cast<int>(m_diff.blocks()[index].begin(vcs::DiffSide::Right).value()) + 1, 1);
    m_syncing = false;
}
int DiffViewer::topLine(vcs::DiffSide side) const {
    return static_cast<int>((side == vcs::DiffSide::Left ? m_left : m_right)->firstVisibleRow());
}
void DiffViewer::scrollToLine(vcs::DiffSide side, int line) {
    (side == vcs::DiffSide::Left ? m_left : m_right)->verticalScrollBar()->setValue(line);
}
void DiffViewer::syncFrom(vcs::DiffSide side) {
    if (m_syncing) return;
    m_syncing = true;
    const auto other = side == vcs::DiffSide::Left ? m_right : m_left;
    other->verticalScrollBar()->setValue(static_cast<int>(m_diff.mapLine(side, LineIndex{static_cast<std::uint32_t>(std::max(0, topLine(side)))}).value()));
    m_syncing = false;
}
void DiffViewer::setApplyHandler(ApplyHandler handler) {
    m_applyHandler = std::move(handler);
    m_right->setFileInfo({.readOnly = !m_applyHandler});
    m_apply->setVisible(static_cast<bool>(m_applyHandler));
    m_apply->setEnabled(hasWorkingEdits());
}
bool DiffViewer::hasWorkingEdits() const {
    return m_applyHandler && m_rightDocument->committedText().text() != m_originalWorking;
}
void DiffViewer::workingEdited() {
    if (m_syncing) return;
    auto recomputed = vcs::SideBySideDiff::compute(std::string(m_diff.left().bytes()), m_rightDocument->committedText().text());
    if (!recomputed) { m_status->setText(QString::fromStdString(recomputed.error().message())); return; }
    m_diff = std::move(*recomputed);
    applyHighlights();
    m_current.reset();
    m_apply->setEnabled(hasWorkingEdits());
}
Result<void> DiffViewer::applyWorkingCopy() {
    if (!m_applyHandler) return fail(ErrorCode::Unsupported, "This comparison is read-only");
    const auto current = m_rightDocument->committedText().text();
    auto applied = m_applyHandler(m_originalWorking, current);
    if (!applied) return applied;
    m_originalWorking = current;
    m_right->markSaved();
    m_apply->setEnabled(false);
    m_status->setText(tr("Working copy applied."));
    return {};
}
bool DiffViewer::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == m_left || watched == m_right) && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_F7 && key->modifiers() == Qt::NoModifier) { nextChange(); return true; }
        if (key->key() == Qt::Key_F7 && key->modifiers() == Qt::ShiftModifier) { previousChange(); return true; }
        if (key->matches(QKeySequence::Save) && m_applyHandler) {
            if (auto saved = applyWorkingCopy(); !saved) m_status->setText(QString::fromStdString(saved.error().message()));
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
