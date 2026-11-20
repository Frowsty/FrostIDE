// SPDX-License-Identifier: Apache-2.0
//
// The tool dialogs and windows of project_properties_dialog.hpp: Add Project Reference (edits by
// SLN-6), the NuGet window and dialog (LTR-7), the ASM/IL viewer (CPP-3, LTR-3) and Local History
// (split from project_properties_dialog.cpp, FND-12a).

#include "frost/text/text_file.hpp"
#include "frost/ui/dialog_chrome.hpp"

#include "project_properties_dialog.hpp"
#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace frost::app {

namespace {

QString toQs(std::string_view sv) { return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size())); }

/*
 * Reasons shown where a feature has no real tool behind it yet (ADR 0078 truth rule, FND-3).
 * Each names the epic that will provide it; tests match on the epic identifiers.
 */
QString referenceEditsUnavailable() {
    return QStringLiteral("Read-only: adding or removing project references requires safe solution and project "
                          "edits (SLN-6).");
}

} // namespace

void showAddProjectReferenceDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel& solution,
                                   const std::string& projectPath, const std::function<void()>& onSaved) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("addProjectReferenceDialog"));
    dialog.setWindowTitle(QStringLiteral("Add Project Reference"));
    dialog.setModal(true);
    dialog.resize(480, 360);

    auto* outer = new QVBoxLayout(&dialog);
    outer->addWidget(new QLabel(QStringLiteral("Projects in the solution this project references:"), &dialog));
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("projectReferenceList"));

    const auto selfId = solution.findProjectByPath(projectPath);
    const auto projects = solution.projects();
    for (std::uint32_t i = 0; i < projects.size(); ++i) {
        if ((selfId && selfId->value() == i) || projects[i].nodeKind != project::NodeKind::Project)
            continue;
        auto* item = new QListWidgetItem(toQs(projects[i].name), list);
        item->setData(Qt::UserRole, static_cast<int>(i));
        // Shown as loaded, not editable: reference edits are refused until SLN-6.
        item->setFlags(Qt::ItemIsUserCheckable);
        bool checked = false;
        if (selfId) {
            const auto& deps = projects[selfId->value()].dependencies;
            checked = std::ranges::find(deps, project::ProjectId{i}) != deps.end();
        }
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    outer->addWidget(list, 1);
    auto* note = new QLabel(referenceEditsUnavailable(), &dialog);
    note->setObjectName(QStringLiteral("projectReferenceReadOnlyNote"));
    note->setWordWrap(true);
    outer->addWidget(note);

    // Nothing is written until SLN-6.
    static_cast<void>(fs);
    static_cast<void>(onSaved);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}

QWidget* createNuGetToolWindowWidget(QWidget* parent, io::IFileSystem& fs,
                                     std::function<const project::SolutionModel*()> solutionProvider,
                                     std::string initialCsprojPath, const std::function<void()>&) {
    // Read-only until LTR-7: nothing here writes a project file.
    auto* root = new QWidget(parent);
    root->setObjectName(QStringLiteral("nugetToolWindowWidget"));
    auto* outer = new QVBoxLayout(root);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(6);

    auto* topBar = new QHBoxLayout();
    auto* projCombo = new QComboBox(root);
    projCombo->setObjectName(QStringLiteral("nugetProjectCombo"));
    projCombo->setMinimumWidth(180);

    auto* searchEdit = new QLineEdit(root);
    searchEdit->setObjectName(QStringLiteral("nugetSearchEdit"));
    searchEdit->setPlaceholderText(QStringLiteral("Filter installed packages..."));
    searchEdit->setClearButtonEnabled(true);

    topBar->addWidget(new QLabel(QStringLiteral("Project:"), root));
    topBar->addWidget(projCombo);
    topBar->addWidget(searchEdit, 1);
    outer->addLayout(topBar);

    auto* note = new QLabel(QStringLiteral("Installed packages as listed in the project file (read-only). Searching, "
                                           "installing and updating packages requires the NuGet client (LTR-7)."),
                            root);
    note->setObjectName(QStringLiteral("nugetReadOnlyNote"));
    note->setWordWrap(true);
    outer->addWidget(note);

    auto* installedTable = new QTableWidget(root);
    installedTable->setObjectName(QStringLiteral("nugetPackagesTable"));
    installedTable->setColumnCount(2);
    installedTable->setHorizontalHeaderLabels(
        {QStringLiteral("Package ID"), QStringLiteral("Version (from project file)")});
    installedTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    installedTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    installedTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    outer->addWidget(installedTable, 1);

    auto* statusLabel = new QLabel(root);
    statusLabel->setObjectName(QStringLiteral("nugetStatusLabel"));
    statusLabel->setWordWrap(true);
    outer->addWidget(statusLabel);

    const auto currentCsproj = [=]() -> std::string {
        const QString data = projCombo->currentData().toString();
        if (!data.isEmpty())
            return data.toStdString();
        return initialCsprojPath;
    };

    const auto refreshProjects = [=, solutionProvider = std::move(solutionProvider),
                                  initialCsprojPath = std::move(initialCsprojPath)] {
        const QString prev = projCombo->currentData().toString();
        projCombo->blockSignals(true);
        projCombo->clear();
        if (solutionProvider) {
            if (const project::SolutionModel* sln = solutionProvider()) {
                for (const auto& p : sln->projects()) {
                    if (p.kind == project::ProjectKind::CSharp && !p.path.empty()) {
                        projCombo->addItem(toQs(p.name), toQs(p.path));
                    }
                }
            }
        }
        if (projCombo->count() == 0 && !initialCsprojPath.empty()) {
            projCombo->addItem(QFileInfo(toQs(initialCsprojPath)).completeBaseName(), toQs(initialCsprojPath));
        }
        const int idx = projCombo->findData(prev.isEmpty() ? toQs(initialCsprojPath) : prev);
        if (idx >= 0)
            projCombo->setCurrentIndex(idx);
        projCombo->blockSignals(false);
    };

    // Every row comes from a PackageReference in the selected project file; a read failure is
    // reported, never replaced by sample data.
    const auto refreshTable = [=, &fs] {
        refreshProjects();
        installedTable->setRowCount(0);
        const std::string csproj = currentCsproj();
        if (csproj.empty()) {
            statusLabel->setText(QStringLiteral("No C# project is selected."));
            return;
        }
        auto content = fs.readFile(csproj);
        if (!content) {
            statusLabel->setText(QStringLiteral("Cannot read %1: %2")
                                     .arg(QFileInfo(toQs(csproj)).fileName(), toQs(content.error().message())));
            return;
        }
        const std::vector<NuGetPackageEntry> installed = parseCsprojNuGetPackages(*content);
        const QString filter = searchEdit->text().trimmed();
        for (const auto& pkg : installed) {
            const QString qId = toQs(pkg.id);
            if (!filter.isEmpty() && !qId.contains(filter, Qt::CaseInsensitive))
                continue;
            const int r = installedTable->rowCount();
            installedTable->insertRow(r);
            installedTable->setItem(r, 0, new QTableWidgetItem(qId));
            installedTable->setItem(r, 1,
                                    new QTableWidgetItem(pkg.version.empty()
                                                             ? QStringLiteral("(not set in the project file)")
                                                             : toQs(pkg.version)));
        }
        statusLabel->setText(QStringLiteral("%1 package reference(s) in %2.")
                                 .arg(installed.size())
                                 .arg(QFileInfo(toQs(csproj)).fileName()));
    };
    refreshTable();

    QObject::connect(projCombo, &QComboBox::currentIndexChanged, root, [=](int) { refreshTable(); });
    QObject::connect(searchEdit, &QLineEdit::textChanged, root, [=](const QString&) { refreshTable(); });
    return root;
}

void showNuGetPackageManagerDialog(QWidget* parent, io::IFileSystem& fs, const std::string& csprojPath,
                                   const std::function<void()>& onSaved) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("nugetPackageManagerDialog"));
    dialog.setWindowTitle(QStringLiteral("NuGet Packages — %1").arg(QFileInfo(toQs(csprojPath)).completeBaseName()));
    dialog.setModal(true);
    dialog.resize(640, 420);

    auto* outer = new QVBoxLayout(&dialog);
    outer->addWidget(createNuGetToolWindowWidget(&dialog, fs, {}, csprojPath, onSaved), 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}

QWidget* createAsmViewerToolWindowWidget(QWidget* parent,
                                         // NOLINTNEXTLINE(performance-unnecessary-value-param): Qt callback snapshot.
                                         std::function<std::pair<std::string, int>()> activeFileAndLineProvider) {
    /*
     * Disabled until it can be real (ADR 0078 truth rule, FND-3). The previous view compiled the
     * file with a hard-coded clang++ and -std=c++20 instead of the file's own compile command, via
     * QProcess rather than IProcessLauncher, and showed an invented IL listing for C#. A correct
     * view needs the file's compile command from the compile database (CPP-3), run through
     * IProcessLauncher, and the .NET toolchain for IL (LTR-3). This widget starts no process.
     */
    auto* root = new QWidget(parent);
    root->setObjectName(QStringLiteral("asmViewerWidget"));
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto* refreshBtn = new QPushButton(QStringLiteral("Disassemble"), root);
    refreshBtn->setObjectName(QStringLiteral("asmRefreshButton"));
    refreshBtn->setEnabled(false);
    const QString reason =
        QStringLiteral("Disassembly is unavailable: it requires the file's real compile command from the compile "
                       "database (CPP-3). IL view is unavailable: it requires the .NET toolchain (LTR-3).");
    refreshBtn->setToolTip(reason);
    auto* actionRow = new QHBoxLayout();
    actionRow->addStretch(1);
    actionRow->addWidget(refreshBtn);
    layout->addLayout(actionRow);

    auto* output = new QPlainTextEdit(root);
    output->setObjectName(QStringLiteral("asmViewerOutput"));
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    QString text = reason;
    if (activeFileAndLineProvider) {
        if (const auto [filePath, line] = activeFileAndLineProvider(); !filePath.empty())
            text += QStringLiteral("\n\nActive file: %1 (line %2)").arg(toQs(filePath)).arg(line);
    }
    output->setPlainText(text);
    layout->addWidget(output, 1);
    return root;
}

void showLocalHistoryDialog(QWidget* parent, const std::string& historyPath, const std::string& fileKey,
                            const std::string& filePath, const std::string& currentContent,
                            std::function<Result<void>(const std::string&)> onRestore) {
    // The worker borrows this stack dialog and its local state. A QObject parent must never
    // delete it during exec(); owner destruction only rejects, and the worker is joined below.
    QDialog dialog;
    dialog.setWindowModality(Qt::ApplicationModal);
    if (parent)
        QObject::connect(parent, &QObject::destroyed, &dialog, &QDialog::reject);
    dialog.setObjectName(QStringLiteral("localHistoryDialog"));
    dialog.setWindowTitle(QStringLiteral("Local History - %1").arg(QFileInfo(toQs(filePath)).fileName()));
    dialog.resize(840, 520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* status = new QLabel(QStringLiteral("Reading stored revisions..."), &dialog);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* splitter = new QSplitter(Qt::Horizontal, &dialog);
    auto* revList = new QListWidget(splitter);
    revList->setObjectName(QStringLiteral("localHistoryRevisionList"));
    auto* preview = new QPlainTextEdit(splitter);
    preview->setObjectName(QStringLiteral("localHistoryPreview"));
    preview->setReadOnly(true);
    preview->setPlainText(toQs(currentContent));
    layout->addWidget(splitter);
    auto* revert = new QPushButton(QStringLiteral("Restore to Editor (Undo available; Save writes the file)"), &dialog);
    revert->setObjectName(QStringLiteral("localHistoryRevertButton"));
    revert->setEnabled(false);
    layout->addWidget(revert);
    auto* close = new QPushButton(QStringLiteral("Close"), &dialog);
    layout->addWidget(close);
    struct Requests {
        std::mutex mutex;
        std::condition_variable changed;
        std::int64_t revision = 0;
        std::uint64_t generation = 0;
        bool stopping = false;
    } requests;
    std::vector<storage::RevisionInfo> revisions;
    std::int64_t loadedId = 0;
    std::string loadedText;
    // A separate connection belongs exclusively to this worker; the SaveManager connection stays
    // with its owner. Queries are bounded and cancellation discards queued or obsolete selections.
    std::jthread reader([&, historyPath, fileKey](std::stop_token stop) {
        storage::HistoryOptions options;
        options.busyTimeout = std::chrono::milliseconds(250);
        auto history = storage::LocalHistory::open(historyPath, systemClock(), options);
        if (!history) {
            const auto error = toQs(history.error().message());
            QMetaObject::invokeMethod(&dialog, [status, error] { status->setText(error); }, Qt::QueuedConnection);
            return;
        }
        auto listed = history->revisions(fileKey, 512);
        if (!listed) {
            const auto error = toQs(listed.error().message());
            QMetaObject::invokeMethod(&dialog, [status, error] { status->setText(error); }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(
            &dialog,
            [&, items = std::move(*listed)] {
                revisions = items;
                for (const auto& revision : revisions) {
                    const auto seconds =
                        std::chrono::duration_cast<std::chrono::seconds>(revision.createdAt.time_since_epoch()).count();
                    revList->addItem(
                        QDateTime::fromSecsSinceEpoch(seconds).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) +
                        QStringLiteral(" - ") +
                        (revision.label.empty() ? QStringLiteral("Saved") : toQs(revision.label)));
                }
                status->setText(revisions.empty()
                                    ? QStringLiteral("No stored revisions for this file.")
                                    : QStringLiteral("Newest 512 stored revisions at most. Restore changes the editor "
                                                     "only; existing unsaved text is protected by Undo."));
                if (!revisions.empty())
                    revList->setCurrentRow(0);
            },
            Qt::QueuedConnection);
        std::uint64_t served = 0;
        while (!stop.stop_requested()) {
            std::int64_t revision = 0;
            std::uint64_t generation = 0;
            {
                std::unique_lock lock(requests.mutex);
                requests.changed.wait(lock, [&] { return requests.stopping || requests.generation != served; });
                if (requests.stopping || stop.stop_requested())
                    return;
                revision = requests.revision;
                generation = requests.generation;
                served = generation;
            }
            std::string bytes;
            auto content = history->content(
                revision,
                [&](std::string_view chunk) -> Result<void> {
                    if (stop.stop_requested())
                        return fail(ErrorCode::Cancelled, "history selection cancelled");
                    if (bytes.size() + chunk.size() > 4 * 1024 * 1024)
                        return fail(ErrorCode::OutOfRange, "History preview and editor restore are limited to 4 MiB.");
                    bytes.append(chunk);
                    return {};
                },
                stop);
            std::string decodedText;
            std::string error;
            if (!content)
                error = content.error().message();
            else {
                auto decoded = text::decodeBytes(bytes);
                if (!decoded)
                    error = decoded.error().message();
                else if (!decoded->encoding.fidelity.lossless() || decoded->binaryLikely)
                    error = "This revision cannot be decoded losslessly as text; restore is unavailable.";
                else
                    decodedText =
                        decoded->payloadIsText ? bytes.substr(decoded->payloadOffset) : std::move(decoded->text);
            }
            QMetaObject::invokeMethod(
                &dialog,
                [&, revision, generation, text = std::move(decodedText), error = std::move(error)] {
                    {
                        std::lock_guard lock(requests.mutex);
                        if (requests.stopping || generation != requests.generation)
                            return;
                    }
                    if (!error.empty()) {
                        status->setText(toQs(error));
                        revert->setEnabled(false);
                        return;
                    }
                    loadedId = revision;
                    loadedText = text;
                    preview->setPlainText(toQs(text));
                    status->setText(QStringLiteral(
                        "Stored revision loaded. Restore makes one undoable editor change; Save is explicit."));
                    revert->setEnabled(true);
                },
                Qt::QueuedConnection);
        }
    });
    QObject::connect(revList, &QListWidget::currentRowChanged, &dialog, [&](int row) {
        loadedId = 0;
        revert->setEnabled(false);
        if (row < 0 || static_cast<std::size_t>(row) >= revisions.size())
            return;
        {
            std::lock_guard lock(requests.mutex);
            requests.revision = revisions[static_cast<std::size_t>(row)].id;
            ++requests.generation;
        }
        requests.changed.notify_one();
    });
    QObject::connect(revert, &QPushButton::clicked, &dialog, [&] {
        const auto row = revList->currentRow();
        if (!loadedId || row < 0 || static_cast<std::size_t>(row) >= revisions.size() ||
            revisions[static_cast<std::size_t>(row)].id != loadedId)
            return;
        if (onRestore) {
            const auto restored = onRestore(loadedText);
            if (!restored) {
                status->setText(toQs(restored.error().message()));
                return;
            }
        }
        dialog.accept();
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
    {
        std::lock_guard lock(requests.mutex);
        requests.stopping = true;
    }
    reader.request_stop();
    requests.changed.notify_all();
    reader.join();
}

} // namespace frost::app
