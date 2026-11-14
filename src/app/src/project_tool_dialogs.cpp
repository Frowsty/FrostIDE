// SPDX-License-Identifier: Apache-2.0
//
// The tool dialogs and windows of project_properties_dialog.hpp: Add Project Reference (edits by
// SLN-6), the NuGet window and dialog (LTR-7), the ASM/IL viewer (CPP-3, LTR-3) and Local History
// (split from project_properties_dialog.cpp, FND-12a).

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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

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

void showLocalHistoryDialog(QWidget* parent, storage::LocalHistory& history, const std::string& filePath,
                            const std::string& currentContent,
                            std::function<void(const std::string& restoredContent)> onRestore) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("localHistoryDialog"));
    dialog.setWindowTitle(QStringLiteral("Local History — %1").arg(QFileInfo(toQs(filePath)).fileName()));
    dialog.setModal(true);
    dialog.resize(840, 520);

    auto* outer = new QVBoxLayout(&dialog);
    auto* splitter = new QSplitter(Qt::Horizontal, &dialog);
    auto* revList = new QListWidget(splitter);
    revList->setObjectName(QStringLiteral("localHistoryRevisionList"));
    revList->setMinimumWidth(260);
    auto* preview = new QPlainTextEdit(splitter);
    preview->setObjectName(QStringLiteral("localHistoryPreview"));
    preview->setReadOnly(true);
    splitter->addWidget(revList);
    splitter->addWidget(preview);
    splitter->setStretchFactor(1, 1);
    outer->addWidget(splitter, 1);

    std::vector<storage::RevisionInfo> revisions;
    if (auto listed = history.revisions(filePath))
        revisions = std::move(*listed);
    for (std::size_t i = 0; i < revisions.size(); ++i) {
        const auto& r = revisions[i];
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(r.createdAt.time_since_epoch()).count();
        const QString timeStr = QDateTime::fromSecsSinceEpoch(secs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        const QString label = r.label.empty() ? QStringLiteral("Saved") : toQs(r.label);
        auto* item = new QListWidgetItem(QStringLiteral("%1 — %2").arg(timeStr, label), revList);
        item->setData(Qt::UserRole, static_cast<int>(i));
    }
    if (revisions.empty()) {
        preview->setPlainText(toQs(currentContent));
    }

    QObject::connect(revList, &QListWidget::currentRowChanged, &dialog, [&](int row) {
        if (row < 0 || static_cast<std::size_t>(row) >= revisions.size())
            return;
        if (auto bytes = history.content(revisions[static_cast<std::size_t>(row)].id)) {
            preview->setPlainText(toQs(*bytes));
        }
    });
    if (!revisions.empty())
        revList->setCurrentRow(0);

    auto* btnLayout = new QHBoxLayout();
    auto* revertBtn = new QPushButton(QStringLiteral("Revert to Selected Revision"), &dialog);
    revertBtn->setObjectName(QStringLiteral("localHistoryRevertButton"));
    revertBtn->setEnabled(!revisions.empty());
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), &dialog);
    btnLayout->addStretch(1);
    btnLayout->addWidget(revertBtn);
    btnLayout->addWidget(closeBtn);
    outer->addLayout(btnLayout);

    QObject::connect(revertBtn, &QPushButton::clicked, &dialog, [&] {
        const int row = revList->currentRow();
        if (row < 0 || static_cast<std::size_t>(row) >= revisions.size())
            return;
        if (auto bytes = history.content(revisions[static_cast<std::size_t>(row)].id)) {
            if (onRestore)
                onRestore(*bytes);
            dialog.accept();
        }
    });
    QObject::connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}

} // namespace frost::app
