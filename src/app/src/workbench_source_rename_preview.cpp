// SPDX-License-Identifier: Apache-2.0
// ProjectHost preview work runs off the GUI thread. Only its immutable, no-write answer returns.
#include "workbench_source_rename_preview.hpp"

#include "frost/ui/tr.hpp"

#include <QCloseEvent>
#include <QDialog>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QVBoxLayout>

#include <optional>
#include <stop_token>
#include <thread>

namespace frost::app {
namespace {
QString text(const char* value) { return ui::qtr("frost::app::Workbench", value); }

class RenamePlanningDialog final : public QDialog {
public:
    // Deliberately no QObject parent: destroying the Workbench window must not delete a stack
    // dialog while its worker owns a queued completion. The worker is always joined first.
    explicit RenamePlanningDialog(std::stop_source& stop, bool solution = false) : m_stop(stop) {
        setObjectName(solution ? QStringLiteral("solutionLoadingDialog")
                               : QStringLiteral("sourceRenamePlanningDialog"));
        setWindowTitle(solution ? text("Open solution") : text("Prepare source rename"));
        setWindowModality(Qt::ApplicationModal);
        auto* layout = new QVBoxLayout(this);
        m_label = new QLabel(solution ? text("Reading solution projects…")
                                      : text("Checking project memberships. No files have been changed."),
                             this);
        layout->addWidget(m_label);
        m_cancel = new QPushButton(text("Cancel"), this);
        connect(m_cancel, &QPushButton::clicked, this, [this] { reject(); });
        layout->addWidget(m_cancel);
    }
    void progress(const project::SolutionLoadProgress& progress) {
        if (!m_stop.stop_requested())
            m_label->setText(text("Read %1 of %2 projects")
                                 .arg(static_cast<qulonglong>(progress.completedProjects))
                                 .arg(static_cast<qulonglong>(progress.totalProjects)));
    }
    void complete() {
        m_complete = true;
        done(QDialog::Accepted);
    }
    void reject() override {
        if (m_complete) {
            QDialog::reject();
            return;
        }
        m_stop.request_stop();
        m_label->setText(text("Cancelling project preparation…"));
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
    QLabel* m_label = nullptr;
    QPushButton* m_cancel = nullptr;
    bool m_complete = false;
};
} // namespace

Result<document::RefactoringPlan>
prepareSourceRenameInBackground(QWidget* ownerWindow,
                                std::function<Result<document::RefactoringPlan>(std::stop_token)> prepare,
                                std::string title) {
    std::stop_source cancellation;
    RenamePlanningDialog dialog(cancellation);
    if (!title.empty())
        dialog.setWindowTitle(QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size())));
    if (ownerWindow)
        QObject::connect(ownerWindow, &QObject::destroyed, &dialog, [&] { dialog.reject(); });
    std::optional<Result<document::RefactoringPlan>> completed;
    std::jthread worker([&, prepare = std::move(prepare)] {
        auto result = prepare(cancellation.get_token());
        QMetaObject::invokeMethod(
            &dialog,
            [&, result = std::move(result)]() mutable {
                completed = std::move(result);
                dialog.complete();
            },
            Qt::QueuedConnection);
    });
    dialog.exec();
    const bool cancelled = cancellation.stop_requested();
    cancellation.request_stop();
    worker.join();
    if (!completed || cancelled)
        return fail(ErrorCode::Cancelled, title.empty()
                                              ? "Project rename preparation was cancelled; no file was moved"
                                              : "Project template preparation was cancelled; no files were created");
    return std::move(*completed);
}

Result<project::SolutionModel> loadSolutionInBackground(QWidget* ownerWindow, io::IFileSystem& fs, std::string path,
                                                        project::SolutionLoadOptions options,
                                                        std::stop_source cancellation) {
    RenamePlanningDialog dialog(cancellation, true);
    if (ownerWindow)
        QObject::connect(ownerWindow, &QObject::destroyed, &dialog, [&] { dialog.reject(); });
    std::optional<Result<project::SolutionModel>> completed;
    options.stop = cancellation.get_token();
    options.progress = [&dialog](const project::SolutionLoadProgress& progress) {
        QMetaObject::invokeMethod(&dialog, [&dialog, progress] { dialog.progress(progress); }, Qt::QueuedConnection);
    };
    std::jthread worker([&, path = std::move(path), options = std::move(options)]() mutable {
        auto result = project::SolutionModel::load(fs, path, std::move(options));
        QMetaObject::invokeMethod(
            &dialog,
            [&, result = std::move(result)]() mutable {
                completed = std::move(result);
                dialog.complete();
            },
            Qt::QueuedConnection);
    });
    dialog.exec();
    const bool cancelled = cancellation.stop_requested();
    cancellation.request_stop();
    // complete() is posted after load has returned. Reject never exits the modal loop early.
    worker.join();
    if (!completed || cancelled)
        return fail(ErrorCode::Cancelled, "Solution loading cancelled; the previous project is unchanged");
    return std::move(*completed);
}

} // namespace frost::app
