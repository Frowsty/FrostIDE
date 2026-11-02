// SPDX-License-Identifier: Apache-2.0
//
// MergeView (frost/ui/workbench/merge_view.hpp).

#include "frost/ui/workbench/merge_view.hpp"

#include "frost/diff/merge.hpp"

#include "workbench_util.hpp"
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>

namespace frost::ui {

using workbench_detail::fromUtf8;
using workbench_detail::nativePath;
using workbench_detail::text;

namespace {

constexpr std::array kPanes{nav::MergePane::Yours, nav::MergePane::Base, nav::MergePane::Disk};

[[nodiscard]] QString kindText(diff::ChunkKind kind) {
    switch (kind) {
    case diff::ChunkKind::Stable: return text("unchanged");
    case diff::ChunkKind::Ours: return text("changed in yours");
    case diff::ChunkKind::Theirs: return text("changed on disk");
    case diff::ChunkKind::Both: return text("same change on both sides");
    case diff::ChunkKind::Conflict: return text("conflict");
    }
    return {};
}

[[nodiscard]] QString takeText(diff::Take take) {
    switch (take) {
    case diff::Take::Unresolved: return text("unresolved");
    case diff::Take::Ours: return text("yours");
    case diff::Take::Theirs: return text("disk");
    case diff::Take::Base: return text("base");
    case diff::Take::OursThenTheirs: return text("yours, then disk");
    case diff::Take::TheirsThenOurs: return text("disk, then yours");
    case diff::Take::Custom: return text("edited");
    }
    return {};
}

// The text of every display row of `map`: buffer rows as they are, filler rows empty.
[[nodiscard]] QString paneText(const text::DisplayMap& map) {
    QStringList rows;
    const text::Snapshot& snapshot = map.text();
    for (std::uint32_t row = 0; row < map.rowCount(); ++row) {
        const text::DisplayRowInfo info = map.rowInfo(text::DisplayRow{row});
        rows.push_back(info.kind == text::RowKind::Text ? fromUtf8(snapshot.text(info.buffer)) : QString());
    }
    return rows.join(QLatin1Char('\n'));
}

} // namespace

Result<MergeView*> MergeView::create(document::SaveManager& manager, document::FileDocument& document,
                                     QWidget* parent) {
    if (document.pendingMerge() == nullptr) {
        return fail(ErrorCode::NotFound, "no external change of '" + document.path() + "' is pending");
    }
    auto* view = new MergeView(manager, document, parent);
    if (auto built = view->rebuild(); !built) {
        delete view;
        return std::unexpected(std::move(built.error()));
    }
    return view;
}

MergeView::MergeView(document::SaveManager& manager, document::FileDocument& document, QWidget* parent)
    : QDialog(parent), m_manager(manager), m_document(document), m_path(document.path()) {
    setObjectName(QStringLiteral("mergeView"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(text("Merge External Changes: %1").arg(nativePath(m_path)));
    setModal(false);
    auto* layout = new QVBoxLayout(this);
    auto* headers = new QHBoxLayout();
    for (const QString& title : {text("Your version"), text("Base (last loaded)"), text("On disk")}) {
        headers->addWidget(new QLabel(title, this), 1);
    }
    layout->addLayout(headers);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const std::array names{QStringLiteral("mergeYours"), QStringLiteral("mergeBase"), QStringLiteral("mergeDisk")};
    for (std::size_t i = 0; i < m_panes.size(); ++i) {
        auto* pane = new QPlainTextEdit(splitter);
        pane->setObjectName(names[i]);
        pane->setReadOnly(true);
        pane->setLineWrapMode(QPlainTextEdit::NoWrap);
        pane->setFont(fixed);
        connect(pane->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) { syncScroll(value); });
        m_panes[i] = pane;
    }
    layout->addWidget(splitter, 1);
    m_chunks = new QListWidget(this);
    m_chunks->setObjectName(QStringLiteral("mergeChunks"));
    m_chunks->setMaximumHeight(140);
    connect(m_chunks, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || static_cast<std::size_t>(row) >= m_changes.size() || !m_layout) {
            return;
        }
        const int top = static_cast<int>(
            m_layout->chunkRow(nav::MergePane::Yours, m_changes[static_cast<std::size_t>(row)]).value());
        m_panes[0]->verticalScrollBar()->setValue(top);
    });
    layout->addWidget(m_chunks);
    auto* chunkButtons = new QHBoxLayout();
    auto* left = new QPushButton(text("Accept Left (yours)"), this);
    left->setObjectName(QStringLiteral("acceptLeft"));
    auto* right = new QPushButton(text("Accept Right (disk)"), this);
    right->setObjectName(QStringLiteral("acceptRight"));
    connect(left, &QPushButton::clicked, this, [this] {
        if (const auto chunk = selectedChunk(); chunk) {
            static_cast<void>(acceptLeft(*chunk));
        }
    });
    connect(right, &QPushButton::clicked, this, [this] {
        if (const auto chunk = selectedChunk(); chunk) {
            static_cast<void>(acceptRight(*chunk));
        }
    });
    chunkButtons->addWidget(left);
    chunkButtons->addWidget(right);
    chunkButtons->addStretch(1);
    layout->addLayout(chunkButtons);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("mergeStatus"));
    layout->addWidget(m_status);
    auto* decisions = new QHBoxLayout();
    m_apply = new QPushButton(text("Apply"), this);
    m_apply->setObjectName(QStringLiteral("mergeApply"));
    auto* mine = new QPushButton(text("Keep My Version"), this);
    mine->setObjectName(QStringLiteral("mergeKeepMine"));
    auto* disk = new QPushButton(text("Take Disk Version"), this);
    disk->setObjectName(QStringLiteral("mergeTakeDisk"));
    connect(m_apply, &QPushButton::clicked, this, [this] { static_cast<void>(apply()); });
    connect(mine, &QPushButton::clicked, this, [this] { static_cast<void>(keepMine()); });
    connect(disk, &QPushButton::clicked, this, [this] { static_cast<void>(takeDisk()); });
    decisions->addStretch(1);
    decisions->addWidget(mine);
    decisions->addWidget(disk);
    decisions->addWidget(m_apply);
    layout->addLayout(decisions);
    resize(1100, 640);
}

Result<void> MergeView::rebuild() {
    document::PendingMerge* merge = m_document.pendingMerge();
    if (merge == nullptr) {
        return fail(ErrorCode::NotFound, "no external change of '" + m_path + "' is pending");
    }
    auto built = nav::MergePanes::create(merge->model, m_shaper);
    if (!built) {
        return std::unexpected(std::move(built.error()));
    }
    m_layout.emplace(std::move(*built));
    m_changes.clear();
    for (std::size_t c = 0; c < m_layout->chunkCount(); ++c) {
        if (m_layout->chunk(c).kind != diff::ChunkKind::Stable) {
            m_changes.push_back(c);
        }
    }
    for (std::size_t i = 0; i < kPanes.size(); ++i) {
        m_panes[i]->setPlainText(paneText(m_layout->map(kPanes[i])));
    }
    refreshChunkList();
    return {};
}

void MergeView::refreshChunkList() {
    const int current = m_chunks->currentRow();
    m_chunks->clear();
    const document::PendingMerge* merge = m_document.pendingMerge();
    if (merge == nullptr || !m_layout) {
        return;
    }
    for (const std::size_t c : m_changes) {
        const diff::MergeChunk& chunk = m_layout->chunk(c);
        const auto lines = [](const diff::LineRange& range) {
            return range.empty() ? QStringLiteral("-")
                                 : QStringLiteral("%1-%2").arg(range.begin.value() + 1).arg(range.end.value());
        };
        m_chunks->addItem(
            text("Yours %1 | disk %2: %3 (%4)")
                .arg(lines(chunk.ours), lines(chunk.theirs), kindText(chunk.kind), takeText(merge->model.take(c))));
    }
    if (m_chunks->count() > 0) {
        m_chunks->setCurrentRow(std::clamp(current, 0, m_chunks->count() - 1));
    }
    const std::uint32_t unresolved = merge->model.unresolvedCount();
    m_apply->setEnabled(unresolved == 0);
    m_status->setText(unresolved == 0 ? text("All conflicts are resolved.")
                                      : text("%1 conflicts are unresolved.").arg(unresolved));
}

std::optional<std::size_t> MergeView::selectedChunk() const {
    const int row = m_chunks->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= m_changes.size()) {
        return std::nullopt;
    }
    return m_changes[static_cast<std::size_t>(row)];
}

Result<void> MergeView::take(std::size_t chunk, bool left) {
    document::PendingMerge* merge = m_document.pendingMerge();
    if (merge == nullptr) {
        return fail(ErrorCode::NotFound, "no external change of '" + m_path + "' is pending");
    }
    auto taken = merge->model.resolve(chunk, left ? diff::Take::Ours : diff::Take::Theirs);
    if (!taken) {
        m_status->setText(fromUtf8(taken.error().message()));
        return taken;
    }
    refreshChunkList();
    return {};
}

Result<void> MergeView::acceptLeft(std::size_t chunk) { return take(chunk, true); }

Result<void> MergeView::acceptRight(std::size_t chunk) { return take(chunk, false); }

std::uint32_t MergeView::unresolvedCount() const {
    const document::PendingMerge* merge = m_document.pendingMerge();
    return merge != nullptr ? merge->model.unresolvedCount() : 0;
}

Result<void> MergeView::decide(document::MergeDecision decision) {
    auto settled = m_manager.resolveExternalChange(m_document, decision);
    if (!settled) {
        m_status->setText(fromUtf8(settled.error().message()));
        if (m_document.pendingMerge() != nullptr) {
            static_cast<void>(rebuild());
        }
        return settled;
    }
    emit resolved(fromUtf8(m_path), static_cast<int>(decision));
    close();
    return {};
}

Result<void> MergeView::apply() { return decide(document::MergeDecision::ApplyMerge); }

Result<void> MergeView::keepMine() { return decide(document::MergeDecision::KeepBuffer); }

Result<void> MergeView::takeDisk() { return decide(document::MergeDecision::TakeDisk); }

void MergeView::syncScroll(int value) {
    if (m_syncing) {
        return;
    }
    m_syncing = true;
    for (QPlainTextEdit* pane : m_panes) {
        pane->verticalScrollBar()->setValue(value);
    }
    m_syncing = false;
}

} // namespace frost::ui
