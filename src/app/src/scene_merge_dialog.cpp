// SPDX-License-Identifier: Apache-2.0
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/vcs/diff_viewer.hpp"

#include "scene_merge_work.hpp"
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <thread>
namespace frost::app {
namespace {
class MergePlanning final : public QDialog {
public:
    explicit MergePlanning(std::stop_source& stop) : m_stop(stop) {
        setObjectName(QStringLiteral("sceneMergePlanningDialog"));
        setWindowTitle(tr("Prepare semantic scene merge"));
        setWindowModality(Qt::ApplicationModal);
        auto* layout = new QVBoxLayout(this);
        m_label =
            new QLabel(tr("Reading actual Git stages and validating with FrostEngine. No files have changed."), this);
        layout->addWidget(m_label);
        m_cancel = new QPushButton(tr("Cancel"), this);
        layout->addWidget(m_cancel);
        connect(m_cancel, &QPushButton::clicked, this, [this] { reject(); });
    }
    void complete() {
        m_complete = true;
        done(Accepted);
    }
    void reject() override {
        if (m_complete) {
            QDialog::reject();
            return;
        }
        m_stop.request_stop();
        m_label->setText(tr("Cancelling engine merge…"));
        m_cancel->setEnabled(false);
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        if (m_complete) {
            event->accept();
            return;
        }
        event->ignore();
        reject();
    }

private:
    std::stop_source& m_stop;
    QLabel* m_label;
    QPushButton* m_cancel;
    bool m_complete = false;
};
QString qtext(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
} // namespace
Result<SceneMergeSnapshot> sceneMergeInBackground(QWidget* owner,
                                                  std::function<Result<SceneMergeSnapshot>(std::stop_token)> prepare) {
    std::stop_source stop;
    MergePlanning dialog(stop);
    QPointer<QWidget> alive(owner);
    if (owner)
        QObject::connect(owner, &QObject::destroyed, &dialog, [&] { dialog.reject(); });
    std::optional<Result<SceneMergeSnapshot>> completed;
    std::jthread worker([&, prepare = std::move(prepare)] {
        auto result = prepare(stop.get_token());
        QMetaObject::invokeMethod(
            &dialog,
            [&, result = std::move(result)]() mutable {
                completed = std::move(result);
                dialog.complete();
            },
            Qt::QueuedConnection);
    });
    dialog.exec();
    const bool cancelled = stop.stop_requested();
    stop.request_stop();
    worker.join();
    if (cancelled || !completed || (owner && !alive))
        return fail(ErrorCode::Cancelled, "Semantic merge cancelled; no files changed");
    return std::move(*completed);
}
Result<bool> showSceneMerge(QWidget* owner, SceneMergeSnapshot snapshot,
                            std::function<Result<SceneMergeSnapshot>(SceneMergeSnapshot, std::stop_token)> refresh,
                            std::function<Result<void>(const SceneMergeSnapshot&)> apply) {
    // Parentless stack modal: owner destruction rejects it without deleting borrowed stack state.
    QDialog dialog;
    QPointer<QWidget> alive(owner);
    if (owner)
        QObject::connect(owner, &QObject::destroyed, &dialog, &QDialog::reject);
    dialog.setObjectName(QStringLiteral("semanticSceneMergeDialog"));
    dialog.setWindowTitle(QObject::tr("Semantic Scene Merge — %1").arg(qtext(snapshot.request.images.path)));
    dialog.resize(1200, 850);
    auto* layout = new QVBoxLayout(&dialog);
    auto* status = new QLabel(&dialog);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* tree = new QTreeWidget(&dialog);
    tree->setObjectName(QStringLiteral("sceneMergeConflicts"));
    tree->setHeaderLabels({QObject::tr("Entity / component / field"), QObject::tr("Resolution")});
    layout->addWidget(tree);
    auto* diff = new ui::DiffViewer(&dialog);
    diff->setObjectName(QStringLiteral("sceneMergeDiff"));
    layout->addWidget(diff, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* update = buttons->addButton(QObject::tr("Validate Choices"), QDialogButtonBox::ActionRole);
    update->setObjectName(QStringLiteral("sceneMergeValidate"));
    auto* commit = buttons->addButton(QObject::tr("Apply and Stage"), QDialogButtonBox::AcceptRole);
    commit->setObjectName(QStringLiteral("sceneMergeApply"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    bool changed = false, applied = false;
    std::vector<QComboBox*> selections;
    const auto render = [&]() -> Result<void> {
        selections.clear();
        tree->clear();
        for (const auto& conflict : snapshot.report.conflicts) {
            auto* item = new QTreeWidgetItem(tree, {qtext(conflict.path)});
            const auto sideText = [](const std::optional<rpc::Json>& value) {
                return value ? qtext(value->dump()).left(4096) : QObject::tr("(absent)");
            };
            item->setToolTip(0, QObject::tr("%1\\nBase: %2\\nOurs: %3\\nTheirs: %4")
                                    .arg(qtext(conflict.kind), sideText(conflict.base), sideText(conflict.ours),
                                         sideText(conflict.theirs)));
            auto* choice = new QComboBox(tree);
            choice->addItem(QObject::tr("Unresolved"), QString{});
            choice->addItem(QObject::tr("Ours%1").arg(conflict.ours ? QString{} : QObject::tr(" (delete)")),
                            QStringLiteral("ours"));
            choice->addItem(QObject::tr("Theirs%1").arg(conflict.theirs ? QString{} : QObject::tr(" (delete)")),
                            QStringLiteral("theirs"));
            choice->addItem(QObject::tr("Base%1").arg(conflict.base ? QString{} : QObject::tr(" (absent)")),
                            QStringLiteral("base"));
            choice->setCurrentIndex(choice->findData(qtext(conflict.choice)));
            tree->setItemWidget(item, 1, choice);
            selections.push_back(choice);
            QObject::connect(choice, &QComboBox::currentIndexChanged, &dialog, [&](int) {
                changed = true;
                commit->setEnabled(false);
                status->setText(QObject::tr("Choices changed. Validate them with FrostEngine before applying."));
            });
        }
        auto model = vcs::SideBySideDiff::compute(snapshot.request.images.ours, snapshot.report.result);
        if (!model)
            return std::unexpected(model.error());
        diff->setDiff(std::move(*model), QObject::tr("Actual Git ours"), QObject::tr("Engine merge preview"));
        QString message =
            QObject::tr(
                "%1 unresolved conflicts. %2 File changes support global Undo; Git staging is a separate operation.")
                .arg(snapshot.report.unresolved)
                .arg(snapshot.report.valid
                         ? QObject::tr("Engine validation passed.")
                         : QObject::tr("Engine validation failed; resolve in the text merge editor."));
        for (const auto& issue : snapshot.report.issues)
            message += QStringLiteral("\n") + qtext(issue.location) + QStringLiteral(": ") + qtext(issue.message);
        status->setText(message);
        changed = false;
        commit->setEnabled(snapshot.report.clean);
        return {};
    };
    if (auto rendered = render(); !rendered)
        return std::unexpected(rendered.error());
    QObject::connect(update, &QPushButton::clicked, &dialog, [&] {
        auto request = snapshot;
        request.request.choices.clear();
        for (std::size_t i = 0; i < selections.size(); ++i) {
            const auto side = selections[i]->currentData().toString().toStdString();
            if (!side.empty())
                request.request.choices.push_back({snapshot.report.conflicts[i].path, side});
        }
        auto result =
            sceneMergeInBackground(&dialog, [refresh, request = std::move(request)](std::stop_token stop) mutable {
                return refresh(std::move(request), stop);
            });
        if (owner && !alive) {
            dialog.reject();
            return;
        }
        if (!result) {
            status->setText(qtext(result.error().message()));
            commit->setEnabled(false);
            return;
        }
        snapshot = std::move(*result);
        if (auto rendered = render(); !rendered) {
            status->setText(qtext(rendered.error().message()));
            commit->setEnabled(false);
        }
    });
    QObject::connect(commit, &QPushButton::clicked, &dialog, [&] {
        if (changed || !snapshot.report.clean || (owner && !alive))
            return;
        auto result = apply(snapshot);
        if (!result) {
            status->setText(qtext(result.error().message()));
            commit->setEnabled(false);
            return;
        }
        applied = true;
        dialog.accept();
    });
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
    return applied;
}
} // namespace frost::app
