// SPDX-License-Identifier: Apache-2.0
//
// The run dialogs of project_properties_dialog.hpp: the running-process enumeration (Windows
// toolhelp, Linux /proc), Attach to Process and Run/Debug Configurations, whose debugger settings
// stay read-only until RUN-1 (split from project_properties_dialog.cpp, FND-12a).

#include "frost/ui/dialog_chrome.hpp"

#include "project_properties_dialog.hpp"
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>

#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

namespace frost::app {

namespace {

QString toQs(std::string_view sv) { return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size())); }

QString runConfigurationsUnavailable() { return QStringLiteral("Requires run configurations (RUN-1)."); }

/*
 * A .NET process: the dotnet host itself, or an apphost with its <name>.runtimeconfig.json beside
 * it. The apphost's <name> is its file name without a ".exe" suffix (Windows) or the whole file
 * name (Linux apphosts have no suffix), so a dotted name such as "My.App" keeps every dot.
 */
void markDotNet(RunningProcessInfo& info) {
    const QString qName = toQs(info.name);
    if (qName.compare(QStringLiteral("dotnet.exe"), Qt::CaseInsensitive) == 0 ||
        qName.compare(QStringLiteral("dotnet"), Qt::CaseInsensitive) == 0) {
        info.isDotNet = true;
    } else if (!info.executablePath.empty()) {
        const QFileInfo exeInfo(toQs(info.executablePath));
        QString stem = exeInfo.fileName();
        if (stem.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive))
            stem.chop(4);
        const QString runtimeCfg = exeInfo.dir().filePath(stem + QStringLiteral(".runtimeconfig.json"));
        if (!stem.isEmpty() && QFileInfo::exists(runtimeCfg))
            info.isDotNet = true;
    }
}

// The whole content of `path`, empty when it cannot be read (the process exited, or no access).
QByteArray readSmallFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace

std::vector<RunningProcessInfo> readProcProcesses(const std::string& procRoot, std::uint32_t selfPid,
                                                  const ReadProcLink& readLink) {
    std::vector<RunningProcessInfo> processes;
    const QDir root(toQs(procRoot));
    for (const QString& entry : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const uint pid = entry.toUInt(&numeric);
        if (!numeric || pid == 0 || pid == selfPid)
            continue;
        const QDir dir(root.filePath(entry));
        std::string exe = readLink ? readLink(dir.filePath(QStringLiteral("exe")).toStdString()) : std::string();
        // The kernel marks the link of an executable that was replaced or removed after it started.
        constexpr std::string_view kDeleted = " (deleted)";
        if (exe.ends_with(kDeleted))
            exe.resize(exe.size() - kDeleted.size());
        const QByteArray cmdline = readSmallFile(dir.filePath(QStringLiteral("cmdline")));
        // Kernel threads have neither an executable nor a command line; nor does a process that is
        // gone or a zombie. Nothing could attach to them.
        if (exe.empty() && cmdline.isEmpty())
            continue;
        RunningProcessInfo info;
        info.pid = static_cast<std::uint32_t>(pid);
        info.executablePath = exe;
        if (!exe.empty()) {
            // comm holds at most 15 bytes of the name; the executable's own name is the real one.
            info.name = QFileInfo(toQs(exe)).fileName().toStdString();
        } else {
            // Another user's process: no executable link, but comm and the command line are readable.
            const QString comm = QString::fromUtf8(readSmallFile(dir.filePath(QStringLiteral("comm")))).trimmed();
            const qsizetype argv0End = cmdline.indexOf('\0');
            const QString argv0 =
                QFileInfo(QString::fromUtf8(argv0End < 0 ? cmdline : cmdline.left(argv0End))).fileName();
            constexpr qsizetype kCommLimit = 15;
            const bool truncated = comm.toUtf8().size() >= kCommLimit && argv0.startsWith(comm);
            info.name = (comm.isEmpty() || truncated ? argv0 : comm).toStdString();
        }
        if (info.name.empty())
            continue;
        markDotNet(info);
        processes.push_back(std::move(info));
    }
    return processes;
}

std::vector<RunningProcessInfo> enumerateRunningProcesses() {
    std::vector<RunningProcessInfo> processes;
#ifdef _WIN32
    std::map<DWORD, std::string> windowTitles;
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            if (!IsWindowVisible(hwnd))
                return TRUE;
            const int len = GetWindowTextLengthW(hwnd);
            if (len <= 0)
                return TRUE;
            std::wstring titleBuf(static_cast<std::size_t>(len) + 1, L'\0');
            if (GetWindowTextW(hwnd, titleBuf.data(), len + 1) <= 0)
                return TRUE;
            titleBuf.resize(static_cast<std::size_t>(len));
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != 0) {
                auto* titles = reinterpret_cast<std::map<DWORD, std::string>*>(lParam);
                if (!titles->contains(pid)) {
                    if (auto utf8 = io::fromWide(titleBuf))
                        (*titles)[pid] = std::move(*utf8);
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windowTitles));

    const DWORD selfPid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID == 0 || entry.th32ProcessID == 4 || entry.th32ProcessID == selfPid)
                    continue;
                RunningProcessInfo info;
                info.pid = static_cast<std::uint32_t>(entry.th32ProcessID);
                if (auto nameUtf8 = io::fromWide(entry.szExeFile))
                    info.name = std::move(*nameUtf8);
                if (const auto it = windowTitles.find(entry.th32ProcessID); it != windowTitles.end())
                    info.windowTitle = it->second;

                if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)) {
                    std::wstring pathBuf(2048, L'\0');
                    DWORD pathLen = static_cast<DWORD>(pathBuf.size());
                    if (QueryFullProcessImageNameW(proc, 0, pathBuf.data(), &pathLen) && pathLen > 0) {
                        pathBuf.resize(pathLen);
                        if (auto pathUtf8 = io::fromWide(pathBuf))
                            info.executablePath = std::move(*pathUtf8);
                    }
                    CloseHandle(proc);
                }

                markDotNet(info);
                processes.push_back(std::move(info));
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
#else
    // Linux: /proc through readProcProcesses; the exe links are read with readlink(2), which gives
    // the kernel's text (" (deleted)" included) and fails for other users' processes.
    const auto selfPid = static_cast<std::uint32_t>(QCoreApplication::applicationPid());
    processes = readProcProcesses("/proc", selfPid, [](const std::string& link) {
        std::string target(4096, '\0');
        const ssize_t length = ::readlink(link.c_str(), target.data(), target.size());
        if (length <= 0 || static_cast<std::size_t>(length) >= target.size())
            return std::string();
        target.resize(static_cast<std::size_t>(length));
        return target;
    });
#endif
    std::ranges::sort(processes, [](const RunningProcessInfo& a, const RunningProcessInfo& b) {
        if (a.windowTitle.empty() != b.windowTitle.empty())
            return !a.windowTitle.empty();
        return a.name < b.name;
    });
    return processes;
}

void showAttachToProcessDialog(QWidget* parent,
                               std::function<void(const RunningProcessInfo& proc, bool attachAsDotNet)> onAttach) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("attachToProcessDialog"));
    dialog.setWindowTitle(QStringLiteral("Attach to Process"));
    dialog.setModal(true);
    dialog.resize(780, 500);

    auto* outer = new QVBoxLayout(&dialog);
    auto* topBar = new QHBoxLayout();
    auto* filterEdit = new QLineEdit(&dialog);
    filterEdit->setObjectName(QStringLiteral("attachProcessFilterEdit"));
    filterEdit->setPlaceholderText(QStringLiteral("Filter by PID, process name, window title, or runtime..."));
    filterEdit->setClearButtonEnabled(true);

    auto* debuggerCombo = new QComboBox(&dialog);
    debuggerCombo->setObjectName(QStringLiteral("attachDebuggerCombo"));
    debuggerCombo->addItems({
        QStringLiteral("Auto-detect (lldb-dap / netcoredbg)"),
        QStringLiteral("Native C/C++ (lldb-dap)"),
        QStringLiteral(".NET CoreCLR (netcoredbg)"),
    });

    auto* refreshBtn = new QPushButton(QStringLiteral("Refresh"), &dialog);
    refreshBtn->setObjectName(QStringLiteral("attachRefreshButton"));

    topBar->addWidget(new QLabel(QStringLiteral("Filter:"), &dialog));
    topBar->addWidget(filterEdit, 1);
    topBar->addWidget(new QLabel(QStringLiteral("Debugger:"), &dialog));
    topBar->addWidget(debuggerCombo);
    topBar->addWidget(refreshBtn);
    outer->addLayout(topBar);

    auto* table = new QTableWidget(&dialog);
    table->setObjectName(QStringLiteral("attachProcessTable"));
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({
        QStringLiteral("PID"),
        QStringLiteral("Process Name"),
        QStringLiteral("Runtime"),
        QStringLiteral("Window Title / Executable Path"),
    });
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    outer->addWidget(table, 1);

    std::vector<RunningProcessInfo> allProcs;
    const auto populateTable = [&] {
        const QString filter = filterEdit->text().trimmed();
        table->setRowCount(0);
        for (std::size_t i = 0; i < allProcs.size(); ++i) {
            const auto& p = allProcs[i];
            const QString pidStr = QString::number(p.pid);
            const QString nameStr = toQs(p.name);
            const QString runtimeStr = p.isDotNet ? QStringLiteral(".NET CoreCLR") : QStringLiteral("Native (x64)");
            const QString detailsStr = !p.windowTitle.empty()
                                           ? QStringLiteral("%1 — %2").arg(toQs(p.windowTitle), toQs(p.executablePath))
                                           : toQs(p.executablePath);
            if (!filter.isEmpty() && !pidStr.contains(filter, Qt::CaseInsensitive) &&
                !nameStr.contains(filter, Qt::CaseInsensitive) && !runtimeStr.contains(filter, Qt::CaseInsensitive) &&
                !detailsStr.contains(filter, Qt::CaseInsensitive)) {
                continue;
            }
            const int r = table->rowCount();
            table->insertRow(r);
            auto* pidItem = new QTableWidgetItem(pidStr);
            pidItem->setData(Qt::UserRole, static_cast<int>(i));
            table->setItem(r, 0, pidItem);
            table->setItem(r, 1, new QTableWidgetItem(nameStr));
            table->setItem(r, 2, new QTableWidgetItem(runtimeStr));
            table->setItem(r, 3, new QTableWidgetItem(detailsStr));
        }
        if (table->rowCount() > 0) {
            table->selectRow(0);
            table->setCurrentCell(0, 0);
        }
    };

    const auto reloadProcesses = [&] {
        allProcs = enumerateRunningProcesses();
        populateTable();
    };
    reloadProcesses();

    QObject::connect(filterEdit, &QLineEdit::textChanged, &dialog, [&](const QString&) { populateTable(); });
    QObject::connect(refreshBtn, &QPushButton::clicked, &dialog, [&] { reloadProcesses(); });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    if (QPushButton* attachBtn = buttons->button(QDialogButtonBox::Ok)) {
        attachBtn->setObjectName(QStringLiteral("attachToProcessAcceptButton"));
        attachBtn->setText(QStringLiteral("Attach"));
    }

    QObject::connect(table, &QTableWidget::cellDoubleClicked, &dialog, [&](int row, int) {
        table->selectRow(row);
        table->setCurrentCell(row, 0);
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    filterEdit->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return;

    int row = table->currentRow();
    if (row < 0 && table->rowCount() > 0)
        row = 0;
    if (row < 0 || !table->item(row, 0))
        return;
    const int idx = table->item(row, 0)->data(Qt::UserRole).toInt();
    if (idx < 0 || static_cast<std::size_t>(idx) >= allProcs.size())
        return;
    const RunningProcessInfo chosen = allProcs[static_cast<std::size_t>(idx)];
    const int dbgMode = debuggerCombo->currentIndex();
    const bool attachAsDotNet = (dbgMode == 2) || (dbgMode == 0 && chosen.isDotNet);
    if (onAttach)
        onAttach(chosen, attachAsDotNet);
}

void showEditRunConfigurationsDialog(QWidget* parent, io::IFileSystem& fs, const project::SolutionModel* solution,
                                     const std::string& preferredProjectPath, const std::string& activeConfigPlatform,
                                     bool externalConsole, std::function<void(bool)> onExternalConsoleChanged,
                                     std::function<void()> onSaved,
                                     std::function<void(const std::vector<project::ProjectId>&)> onLaunchCompound) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("editRunConfigurationsDialog"));
    dialog.setWindowTitle(QStringLiteral("Edit Run/Debug Configurations"));
    dialog.setModal(true);
    dialog.resize(660, 520);

    auto* outer = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* projCombo = new QComboBox(&dialog);
    projCombo->setObjectName(QStringLiteral("runConfigProjectCombo"));
    if (solution) {
        for (const auto& p : solution->projects()) {
            if (p.nodeKind == project::NodeKind::Project) {
                projCombo->addItem(toQs(p.name), toQs(p.path));
                if (p.path == preferredProjectPath)
                    projCombo->setCurrentIndex(projCombo->count() - 1);
            }
        }
    } else if (!preferredProjectPath.empty()) {
        projCombo->addItem(QFileInfo(toQs(preferredProjectPath)).completeBaseName(), toQs(preferredProjectPath));
    }
    // Debugger settings are shown read-only: writing them belongs to run configurations (RUN-1),
    // and the shared .vcxproj is never the place for per-user launch settings.
    const auto readOnlyEdit = [&dialog](const char* objectName) {
        auto* edit = new QLineEdit(&dialog);
        edit->setObjectName(QLatin1String(objectName));
        edit->setReadOnly(true);
        edit->setToolTip(runConfigurationsUnavailable());
        return edit;
    };
    auto* cmdEdit = readOnlyEdit("runConfigCommandEdit");
    auto* argsEdit = readOnlyEdit("runConfigArgumentsEdit");
    auto* cwdEdit = readOnlyEdit("runConfigWorkingDirEdit");
    auto* envEdit = readOnlyEdit("runConfigEnvironmentEdit");
    auto* sourceLabel = new QLabel(&dialog);
    sourceLabel->setObjectName(QStringLiteral("runConfigSourceLabel"));
    sourceLabel->setWordWrap(true);
    auto* extConsoleCheck = new QCheckBox(QStringLiteral("Run in external console window"), &dialog);
    extConsoleCheck->setChecked(externalConsole);

    form->addRow(QStringLiteral("Target project:"), projCombo);
    form->addRow(QStringLiteral("Debugger command:"), cmdEdit);
    form->addRow(QStringLiteral("Program arguments:"), argsEdit);
    form->addRow(QStringLiteral("Working directory:"), cwdEdit);
    form->addRow(QStringLiteral("Environment variables:"), envEdit);
    form->addRow(QString(), sourceLabel);
    form->addRow(QString(), extConsoleCheck);
    outer->addLayout(form);

    // Compound Run Configuration section (launch multiple targets simultaneously).
    auto* compoundGroup = new QGroupBox(QStringLiteral("Compound Run Configuration (Multi-Target Launch)"), &dialog);
    compoundGroup->setObjectName(QStringLiteral("compoundRunGroup"));
    auto* compoundLayout = new QVBoxLayout(compoundGroup);
    auto* compoundTop = new QHBoxLayout();
    auto* compoundNameEdit = new QLineEdit(QStringLiteral("Server + Client (Compound)"), compoundGroup);
    compoundNameEdit->setObjectName(QStringLiteral("compoundConfigNameEdit"));
    auto* launchCompoundBtn = new QPushButton(QStringLiteral("Run Selected Targets Together"), compoundGroup);
    launchCompoundBtn->setObjectName(QStringLiteral("compoundLaunchButton"));
    compoundTop->addWidget(new QLabel(QStringLiteral("Compound Name:"), compoundGroup));
    compoundTop->addWidget(compoundNameEdit, 1);
    compoundTop->addWidget(launchCompoundBtn);
    compoundLayout->addLayout(compoundTop);

    auto* compoundList = new QListWidget(compoundGroup);
    compoundList->setObjectName(QStringLiteral("compoundProjectsList"));
    if (solution) {
        const auto projects = solution->projects();
        for (std::uint32_t i = 0; i < projects.size(); ++i) {
            if (projects[i].nodeKind != project::NodeKind::Project)
                continue;
            auto* item = new QListWidgetItem(toQs(projects[i].name), compoundList);
            item->setData(Qt::UserRole, static_cast<int>(i));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(projects[i].path == preferredProjectPath ? Qt::Checked : Qt::Unchecked);
        }
    }
    compoundLayout->addWidget(compoundList, 1);
    outer->addWidget(compoundGroup, 1);
    // Compound launches need real run configurations (RUN-5); the group is shown, but nothing in
    // it can be used, so no control advertises a launch that cannot happen.
    const QString compoundReason = QStringLiteral("Compound runs require run configurations (RUN-5).");
    compoundGroup->setEnabled(false);
    compoundGroup->setToolTip(compoundReason);
    launchCompoundBtn->setToolTip(compoundReason);

    QObject::connect(launchCompoundBtn, &QPushButton::clicked, &dialog, [&] {
        std::vector<project::ProjectId> selectedIds;
        for (int r = 0; r < compoundList->count(); ++r) {
            if (auto* item = compoundList->item(r); item && item->checkState() == Qt::Checked) {
                selectedIds.push_back(project::ProjectId{static_cast<std::uint32_t>(item->data(Qt::UserRole).toInt())});
            }
        }
        if (selectedIds.empty())
            return;
        dialog.accept();
        if (onLaunchCompound)
            onLaunchCompound(selectedIds);
    });

    // MSBuild imports <project>.user after the project, so its values win; the project file is
    // the fallback. A file that cannot be read leaves the fields empty and says so.
    const auto loadProj = [&] {
        for (QLineEdit* edit : {cmdEdit, argsEdit, cwdEdit, envEdit})
            edit->clear();
        const std::string path = projCombo->currentData().toString().toStdString();
        if (path.empty()) {
            sourceLabel->setText(runConfigurationsUnavailable());
            return;
        }
        ProjectPropertyMap props;
        QStringList readFrom;
        for (const std::string& file : {path, path + ".user"}) {
            if (!fs.exists(file).value_or(false))
                continue;
            if (auto content = fs.readFile(file)) {
                for (auto& [key, value] : parseVcxprojPropertiesForConfig(*content, activeConfigPlatform))
                    props[key] = std::move(value);
                readFrom.push_back(QFileInfo(toQs(file)).fileName());
            }
        }
        cmdEdit->setText(toQs(props["Prop.LocalDebuggerCommand"]));
        argsEdit->setText(toQs(props["Prop.LocalDebuggerCommandArguments"]));
        cwdEdit->setText(toQs(props["Prop.LocalDebuggerWorkingDirectory"]));
        envEdit->setText(toQs(props["Prop.LocalDebuggerEnvironment"]));
        sourceLabel->setText(
            QStringLiteral("Read-only (%1). Editing debugger settings requires run configurations (RUN-1).")
                .arg(readFrom.isEmpty() ? QStringLiteral("project file could not be read")
                                        : QStringLiteral("read from %1").arg(readFrom.join(QStringLiteral(", ")))));
    };
    loadProj();
    QObject::connect(projCombo, &QComboBox::currentIndexChanged, &dialog, [&](int) { loadProj(); });

    // OK applies the external-console choice only; no project file is written (RUN-1).
    static_cast<void>(onSaved);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (onExternalConsoleChanged && extConsoleCheck->isChecked() != externalConsole)
            onExternalConsoleChanged(extConsoleCheck->isChecked());
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}

} // namespace frost::app
