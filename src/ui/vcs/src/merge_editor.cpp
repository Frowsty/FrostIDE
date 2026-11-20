// SPDX-License-Identifier: Apache-2.0
// Shared three-way merge UI for Git conflicts and command-line comparisons, using real editor undo.
#include "frost/ui/vcs/merge_editor.hpp"
#include "frost/ui/editor/editor_key_routing.hpp"
#include "frost/diff/merge.hpp"
#include "frost/text/text_shaper.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/nav/merge_layout.hpp"
#include "frost/ui/vcs/merge_guard.hpp"
#include <QCoreApplication>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
namespace frost::ui {
Result<bool> showMergeEditor(MergeEditorInputs inputs, MergeEditorApply apply, EditorServices services, QWidget* parent) {
    if (!apply) return fail(ErrorCode::InvalidArgument, "A merge result needs an admitted Apply callback");
    std::unique_ptr<EditorKeyRouting> standalone;
    if (!services.keys && !services.actionHost) {
        auto active = input::readActiveLayout();
        const auto layout = active ? *active : input::simulatedLayout(input::SimulatedLayoutId::EnUs);
#ifdef _WIN32
        constexpr auto platform = input::KeyPlatform::Windows;
#else
        constexpr auto platform = input::KeyPlatform::X11;
#endif
        auto routing = EditorKeyRouting::createStandalone(layout, input::currentKeymapPlatform(), platform);
        if (!routing) return std::unexpected(std::move(routing.error()));
        standalone = std::move(*routing);
        services.keys = standalone.get();
    }
    const auto tr = [](const char* text) { return QCoreApplication::translate("MergeEditor", text); };
    auto baseSeq = diff::LineSequence::fromBytes(inputs.base, diff::LineSplit::Lsp);
    auto oursSeq = diff::LineSequence::fromBytes(inputs.ours, diff::LineSplit::Lsp);
    auto theirsSeq = diff::LineSequence::fromBytes(inputs.theirs, diff::LineSplit::Lsp);
    if (!baseSeq) return std::unexpected(std::move(baseSeq.error()));
    if (!oursSeq) return std::unexpected(std::move(oursSeq.error()));
    if (!theirsSeq) return std::unexpected(std::move(theirsSeq.error()));
    auto modelRes = diff::MergeModel::create(std::move(*baseSeq), std::move(*oursSeq), std::move(*theirsSeq));
    if (!modelRes) return std::unexpected(std::move(modelRes.error()));
    diff::MergeModel model = std::move(*modelRes);
    text::MonospaceTextShaper shaper;
    auto panesRes = ui::nav::MergePanes::create(model, shaper);
    if (!panesRes) return std::unexpected(std::move(panesRes.error()));
    ui::nav::MergePanes panes = std::move(*panesRes);

    auto yoursDocument = text::Document::fromUtf8(inputs.ours);
    auto theirsDocument = text::Document::fromUtf8(inputs.theirs);
    text::Document resultDocument(text::TextBuffer{});
    if (!yoursDocument) return std::unexpected(std::move(yoursDocument.error()));
    if (!theirsDocument) return std::unexpected(std::move(theirsDocument.error()));
    QDialog mergeDialog(parent);
    mergeDialog.setObjectName(QStringLiteral("gitThreeWayMergeDialog"));
    mergeDialog.setWindowTitle(tr("Merge Conflicts — %1").arg(QString::fromStdString(inputs.path)));
    mergeDialog.resize(1100, 680);

    auto* mLayout = new QVBoxLayout(&mergeDialog);
    auto* headers = new QHBoxLayout();
    headers->addWidget(new QLabel(tr("Local (Yours)"), &mergeDialog), 1);
    headers->addWidget(new QLabel(tr("Merged Result"), &mergeDialog), 1);
    headers->addWidget(new QLabel(tr("Remote (Theirs)"), &mergeDialog), 1);
    mLayout->addLayout(headers);

    auto* splitter = new QSplitter(Qt::Horizontal, &mergeDialog);
    auto* yoursPane = new ui::EditorView(*yoursDocument, inputs.path, services, {}, splitter);
    yoursPane->setObjectName(QStringLiteral("mergeYours"));
    yoursPane->setFileInfo({.readOnly = true});
    auto* resultPane = new ui::EditorView(resultDocument, inputs.path, services, {}, splitter);
    resultPane->setObjectName(QStringLiteral("mergeResult"));
    auto* theirsPane = new ui::EditorView(*theirsDocument, inputs.path, services, {}, splitter);
    theirsPane->setObjectName(QStringLiteral("mergeTheirs"));
    theirsPane->setFileInfo({.readOnly = true});
    mLayout->addWidget(splitter, 1);
    const auto setResult = [&](std::string_view text) {
        auto changed = resultDocument.replace({ByteOffset{0}, resultDocument.text().size()}, text, {});
        if (changed) static_cast<void>(resultPane->followExternalChange(*changed));
    };
    bool manualResolved = false;
    bool syncingScroll = false;
    const auto syncScroll = [&](int val) {
        if (syncingScroll)
            return;
        syncingScroll = true;
        yoursPane->verticalScrollBar()->setValue(val);
        resultPane->verticalScrollBar()->setValue(val);
        theirsPane->verticalScrollBar()->setValue(val);
        syncingScroll = false;
    };
    connect(yoursPane->verticalScrollBar(), &QScrollBar::valueChanged, &mergeDialog, syncScroll);
    connect(resultPane->verticalScrollBar(), &QScrollBar::valueChanged, &mergeDialog, syncScroll);
    connect(theirsPane->verticalScrollBar(), &QScrollBar::valueChanged, &mergeDialog, syncScroll);

    std::vector<std::size_t> changedChunks;
    for (std::size_t c = 0; c < panes.chunkCount(); ++c) {
        if (panes.chunk(c).kind != diff::ChunkKind::Stable)
            changedChunks.push_back(c);
    }

    auto* chunkList = new QListWidget(&mergeDialog);
    chunkList->setObjectName(QStringLiteral("mergeChunks"));
    chunkList->setMaximumHeight(130);
    mLayout->addWidget(chunkList);

    auto* chunkBtns = new QHBoxLayout();
    auto* acceptLeftBtn = new QPushButton(tr("Accept Left (Yours)"), &mergeDialog);
    acceptLeftBtn->setObjectName(QStringLiteral("acceptLeft"));
    auto* acceptRightBtn = new QPushButton(tr("Accept Right (Theirs)"), &mergeDialog);
    acceptRightBtn->setObjectName(QStringLiteral("acceptRight"));
    auto* acceptAllLeftBtn = new QPushButton(tr("Accept All Yours"), &mergeDialog);
    acceptAllLeftBtn->setObjectName(QStringLiteral("acceptAllLeft"));
    auto* acceptAllRightBtn = new QPushButton(tr("Accept All Theirs"), &mergeDialog);
    acceptAllRightBtn->setObjectName(QStringLiteral("acceptAllRight"));
    chunkBtns->addWidget(acceptLeftBtn);
    chunkBtns->addWidget(acceptRightBtn);
    chunkBtns->addWidget(acceptAllLeftBtn);
    chunkBtns->addWidget(acceptAllRightBtn);
    auto* useEdited = new QPushButton(tr("Mark Edited Result Resolved"), &mergeDialog);
    useEdited->setObjectName(QStringLiteral("acceptEditedResult"));
    chunkBtns->addWidget(useEdited);
    chunkBtns->addStretch(1);
    mLayout->addLayout(chunkBtns);

    auto* statusLabel = new QLabel(&mergeDialog);
    statusLabel->setObjectName(QStringLiteral("mergeStatus"));
    mLayout->addWidget(statusLabel);

    auto* bottomBtns = new QHBoxLayout();
    auto* applyBtn = new QPushButton(tr("Apply & Mark Resolved"), &mergeDialog);
    applyBtn->setObjectName(QStringLiteral("applyMergeButton"));
    applyBtn->setDefault(true);
    auto* cancelBtn = new QPushButton(tr("Cancel"), &mergeDialog);
    cancelBtn->setObjectName(QStringLiteral("cancelMergeButton"));
    bottomBtns->addStretch(1);
    bottomBtns->addWidget(cancelBtn);
    bottomBtns->addWidget(applyBtn);
    mLayout->addLayout(bottomBtns);

    const auto refreshMergeState = [&] {
        const int curRow = chunkList->currentRow();
        chunkList->clear();
        for (const std::size_t c : changedChunks) {
            const diff::MergeChunk& chunk = panes.chunk(c);
            const auto fmtRange = [](const diff::LineRange& r) {
                return r.empty() ? QStringLiteral("-")
                                 : QStringLiteral("%1-%2").arg(r.begin.value() + 1).arg(r.end.value());
            };
            QString takeStr;
            switch (model.take(c)) {
            case diff::Take::Unresolved: takeStr = tr("unresolved"); break;
            case diff::Take::Ours: takeStr = tr("yours"); break;
            case diff::Take::Theirs: takeStr = tr("theirs"); break;
            case diff::Take::Base: takeStr = tr("base"); break;
            case diff::Take::OursThenTheirs: takeStr = tr("yours, then theirs"); break;
            case diff::Take::TheirsThenOurs: takeStr = tr("theirs, then yours"); break;
            case diff::Take::Custom: takeStr = tr("edited"); break;
            }
            chunkList->addItem(
                tr("Yours %1 | Theirs %2 (%3)").arg(fmtRange(chunk.ours), fmtRange(chunk.theirs), takeStr));
        }
        if (chunkList->count() > 0)
            chunkList->setCurrentRow(std::clamp(curRow, 0, chunkList->count() - 1));

        if (auto resolved = model.resolvedText()) {
            setResult(resolved->text);
        } else if (auto marked = model.renderWithMarkers()) {
            setResult(marked->text);
        } else {
            setResult(inputs.ours);
        }

        const std::uint32_t unresolved = model.unresolvedCount();
        statusLabel->setText(unresolved == 0 ? tr("All conflicts resolved.")
                                             : tr("%1 conflict(s) unresolved.").arg(unresolved));
    };
    // FND-3: Apply refuses while chunks are unresolved or conflict markers remain in the result.
    const auto applyBlocker = [&] {
        return QString::fromStdString(
            ui::mergeApplyBlocker(manualResolved ? 0U : model.unresolvedCount(), resultDocument.committedText().text()));
    };
    const auto refreshApply = [&] {
        const QString blocker = applyBlocker();
        applyBtn->setEnabled(blocker.isEmpty());
        applyBtn->setToolTip(blocker);
    };
    connect(resultPane, &ui::EditorView::textChanged, &mergeDialog, [&] { manualResolved = false; refreshApply(); });
    connect(useEdited, &QPushButton::clicked, &mergeDialog, [&] {
        if (ui::hasConflictMarkers(resultDocument.committedText().text())) {
            statusLabel->setText(tr("Remove conflict markers before accepting an edited result."));
            return;
        }
        manualResolved = true;
        statusLabel->setText(tr("Edited result marked resolved. Apply validates the file and Git stages before writing."));
        refreshApply();
    });
    refreshMergeState();
    refreshApply();

    connect(acceptLeftBtn, &QPushButton::clicked, &mergeDialog, [&] {
        const int row = chunkList->currentRow();
        if (row >= 0 && static_cast<std::size_t>(row) < changedChunks.size()) {
            static_cast<void>(model.resolve(changedChunks[static_cast<std::size_t>(row)], diff::Take::Ours));
            refreshMergeState();
        }
    });
    connect(acceptRightBtn, &QPushButton::clicked, &mergeDialog, [&] {
        const int row = chunkList->currentRow();
        if (row >= 0 && static_cast<std::size_t>(row) < changedChunks.size()) {
            static_cast<void>(model.resolve(changedChunks[static_cast<std::size_t>(row)], diff::Take::Theirs));
            refreshMergeState();
        }
    });
    connect(acceptAllLeftBtn, &QPushButton::clicked, &mergeDialog, [&] {
        for (const std::size_t c : changedChunks)
            static_cast<void>(model.resolve(c, diff::Take::Ours));
        refreshMergeState();
    });
    connect(acceptAllRightBtn, &QPushButton::clicked, &mergeDialog, [&] {
        for (const std::size_t c : changedChunks)
            static_cast<void>(model.resolve(c, diff::Take::Theirs));
        refreshMergeState();
    });
    connect(applyBtn, &QPushButton::clicked, &mergeDialog, [&] {
        if (const QString blocker = applyBlocker(); !blocker.isEmpty()) {
            statusLabel->setText(blocker);
            return;
        }
        const std::string mergedText = resultDocument.committedText().text();
        auto applied = apply(mergedText);
        if (applied) mergeDialog.accept();
        else statusLabel->setText(QString::fromStdString(applied.error().message()));
    });
    connect(cancelBtn, &QPushButton::clicked, &mergeDialog, &QDialog::reject);

    ui::installDialogChrome(&mergeDialog, true);
    return mergeDialog.exec() == QDialog::Accepted;
}
}
