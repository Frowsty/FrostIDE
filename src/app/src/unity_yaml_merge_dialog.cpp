// SPDX-License-Identifier: Apache-2.0
// Runs Unity YAML merge preparation in the background and shows the diff preview.
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/vcs/diff_viewer.hpp"

#include "unity_yaml_merge.hpp"
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <thread>
namespace frost::app {
namespace {
class UnityMergePlanning final : public QDialog {
public:
    explicit UnityMergePlanning(std::stop_source& stop) : m_stop(stop) {
        setObjectName(QStringLiteral("unityMergePlanningDialog"));
        setWindowTitle(tr("UnityYAMLMerge"));
        setWindowModality(Qt::ApplicationModal);
        auto* layout = new QVBoxLayout(this);
        m_label = new QLabel(
            tr("Running the selected UnityYAMLMerge on isolated Git stage copies. Working files are unchanged."), this);
        m_label->setWordWrap(true);
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
        m_label->setText(tr("Cancelling UnityYAMLMerge…"));
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
QString qtext(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
} // namespace
Result<UnityMergePreview> prepareUnityMergeInBackground(QWidget* owner, UnityMergeWork work) {
    std::stop_source stop;
    UnityMergePlanning dialog(stop);
    QPointer<QWidget> alive(owner);
    if (owner)
        QObject::connect(owner, &QObject::destroyed, &dialog, [&] { dialog.reject(); });
    std::optional<Result<UnityMergePreview>> completed;
    std::jthread worker([&, work = std::move(work)] {
        auto result = prepareUnityMerge(work, stop.get_token());
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
        return fail(ErrorCode::Cancelled, "Unity merge cancelled; working files were not changed");
    return std::move(*completed);
}
Result<bool> showUnityMergePreview(QWidget* owner, const UnityMergePreview& preview,
                                   std::function<Result<void>(std::string_view)> apply) {
    QDialog dialog;
    QPointer<QWidget> alive(owner);
    if (owner)
        QObject::connect(owner, &QObject::destroyed, &dialog, &QDialog::reject);
    dialog.setObjectName(QStringLiteral("unityMergePreviewDialog"));
    dialog.setWindowTitle(QObject::tr("UnityYAMLMerge Preview"));
    dialog.resize(1150, 800);
    auto* layout = new QVBoxLayout(&dialog);
    auto* status = new QLabel(
        preview.clean
            ? QObject::tr("UnityYAMLMerge returned a result without reported conflicts. Review the output before "
                          "applying and staging.")
            : QObject::tr("UnityYAMLMerge reported unresolved conflicts, failed, or produced no admissible output. The "
                          "working file and index remain unchanged. Use Merge… to resolve manually."),
        &dialog);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* diagnostics = new QPlainTextEdit(qtext(preview.diagnostics), &dialog);
    diagnostics->setObjectName(QStringLiteral("unityMergeDiagnostics"));
    diagnostics->setReadOnly(true);
    diagnostics->setMaximumHeight(150);
    layout->addWidget(diagnostics);
    auto model = vcs::SideBySideDiff::compute(preview.ours, preview.output);
    if (!model)
        return std::unexpected(model.error());
    auto* diff = new ui::DiffViewer(&dialog);
    diff->setObjectName(QStringLiteral("unityMergeDiff"));
    diff->setDiff(std::move(*model), QObject::tr("Actual Git ours"), QObject::tr("Actual UnityYAMLMerge output"));
    layout->addWidget(diff, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* commit = buttons->addButton(QObject::tr("Apply and Stage"), QDialogButtonBox::AcceptRole);
    commit->setObjectName(QStringLiteral("unityMergeApply"));
    commit->setEnabled(preview.clean);
    layout->addWidget(buttons);
    bool applied = false;
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(commit, &QPushButton::clicked, &dialog, [&] {
        if (!preview.clean || (owner && !alive))
            return;
        auto result = apply(preview.output);
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
