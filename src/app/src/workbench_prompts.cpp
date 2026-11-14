// SPDX-License-Identifier: Apache-2.0
//
// DialogWorkbenchPrompts (frost/app/workbench.hpp): the Qt dialogs behind IWorkbenchPrompts, from
// the open and save-as file choosers to new item, rename, delete, new project and the
// discard, rerun, exit and project-recovery confirmations (split from workbench.cpp, FND-12a).

#include "frost/app/welcome_screen.hpp"
#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/navigation_bar.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/terminal/terminal_tool_window.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/tr.hpp"
#include "frost/ui/workbench/file_system_view.hpp"

#include "cmake_integration.hpp"
#include "settings_dialog.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstring>

namespace frost::app {

namespace {

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

} // namespace

// --- DialogWorkbenchPrompts ---

std::string DialogWorkbenchPrompts::chooseOpenPath(QWidget* parent, OpenKind kind) {
    const QString chosen =
        kind == OpenKind::Folder
            ? QFileDialog::getExistingDirectory(parent, text("Open Folder"))
            : QFileDialog::getOpenFileName(
                  parent, text("Open"), QString(),
                  text("Projects and files (*.sln *.slnx CMakeLists.txt *);;Solutions (*.sln *.slnx);;"
                       "CMake project (CMakeLists.txt);;Standalone C++ project (compile_commands.json GNUmakefile "
                       "makefile Makefile);;"
                       "All files (*)"));
    return QDir::toNativeSeparators(chosen).toStdString();
}

std::string DialogWorkbenchPrompts::chooseSaveAsPath(QWidget* parent, const std::string& currentPath) {
    const QString initial = QDir::toNativeSeparators(qs(currentPath));
    const QString chosen = QFileDialog::getSaveFileName(parent, text("Save As"), initial, text("All files (*)"));
    return QDir::toNativeSeparators(chosen).toStdString();
}

std::string DialogWorkbenchPrompts::chooseNewItemName(QWidget* parent, const std::string& directory, bool folder) {
    QDialog dialog(parent);
    dialog.setObjectName(folder ? QStringLiteral("newFolderDialog") : QStringLiteral("newFileDialog"));
    dialog.setWindowTitle(folder ? text("New Folder") : text("New File"));
    dialog.setModal(true);
    dialog.setMinimumWidth(440);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    if (!directory.empty()) {
        auto* dirLabel = new QLabel(text("In: %1").arg(QDir::toNativeSeparators(qs(directory))), &dialog);
        dirLabel->setWordWrap(true);
        layout->addWidget(dirLabel);
    }

    auto* edit = new QLineEdit(&dialog);
    edit->setObjectName(QStringLiteral("newItemNameEdit"));
    edit->setPlaceholderText(folder ? text("include or src/detail") : text("example.cpp or sub/example.hpp"));
    layout->addWidget(edit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    if (ok) {
        ok->setText(text("Create"));
        ok->setEnabled(false);
    }
    QObject::connect(edit, &QLineEdit::textChanged, &dialog, [ok](const QString& value) {
        if (ok)
            ok->setEnabled(!value.trimmed().isEmpty());
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    ui::installDialogChrome(&dialog, false);

    edit->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return {};
    return edit->text().trimmed().toStdString();
}

std::string DialogWorkbenchPrompts::chooseRenameName(QWidget* parent, const std::string& currentPath, bool folder) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("renameItemDialog"));
    dialog.setWindowTitle(folder ? text("Rename Folder") : text("Rename File"));
    dialog.setModal(true);
    dialog.setMinimumWidth(440);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    const std::string_view currentName = io::fileName(currentPath);
    auto* label = new QLabel(text("Rename '%1' to:").arg(qs(currentName)), &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);

    auto* edit = new QLineEdit(qs(currentName), &dialog);
    edit->setObjectName(QStringLiteral("renameItemNameEdit"));
    if (!folder) {
        const QString initial = edit->text();
        const qsizetype dot = initial.lastIndexOf(u'.');
        if (dot > 0) {
            edit->setSelection(0, static_cast<int>(dot));
        } else {
            edit->selectAll();
        }
    } else {
        edit->selectAll();
    }
    layout->addWidget(edit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    if (ok) {
        ok->setText(text("Rename"));
    }
    QObject::connect(edit, &QLineEdit::textChanged, &dialog, [ok](const QString& value) {
        if (ok)
            ok->setEnabled(!value.trimmed().isEmpty());
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    ui::installDialogChrome(&dialog, false);

    edit->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return {};
    return edit->text().trimmed().toStdString();
}

bool DialogWorkbenchPrompts::confirmDelete(QWidget* parent, const QString& name, bool folder) {
    const QString prompt =
        folder ? text("Delete folder '%1' and all of its contents?").arg(name) : text("Delete file '%1'?").arg(name);
    return QMessageBox::question(parent, folder ? text("Delete Folder") : text("Delete File"), prompt,
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
}

std::optional<NewProjectSpec> DialogWorkbenchPrompts::chooseNewProject(QWidget* parent,
                                                                       const std::string& defaultLocation) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("newProjectDialog"));
    dialog.setWindowTitle(text("New Project"));
    dialog.setModal(true);
    dialog.setMinimumSize(680, 380);

    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(12);

    auto* body = new QHBoxLayout();
    body->setSpacing(16);

    auto* templateList = new QListWidget(&dialog);
    templateList->setObjectName(QStringLiteral("newProjectTemplateList"));
    templateList->setFixedWidth(265);
    struct TemplateEntry {
        NewProjectTemplate kind;
        QString label;
        QString description;
    };
    const std::array<TemplateEntry, 10> entries{{
        {NewProjectTemplate::CMakeExecutable, text("C++ Console App (CMake)"),
         text("Creates a C++20 CMake executable project with CMakeLists.txt and main.cpp.")},
        {NewProjectTemplate::CMakeStaticLibrary, text("C++ Static Library (CMake)"),
         text("Creates a C++20 CMake static library project with CMakeLists.txt, header, and source files.")},
        {NewProjectTemplate::CMakeSharedLibrary, text("C++ Shared Library (CMake)"),
         text("Creates a C++20 CMake shared library (DLL/.so) project with export macros, header, and source files.")},
        {NewProjectTemplate::VsCppConsole, text("C++ Console App (Visual Studio .sln)"),
         text("Creates a Visual Studio solution (.sln) and C++20 MSBuild project (.vcxproj) with main.cpp.")},
        {NewProjectTemplate::SlnxCppConsole, text("C++ Console App (Visual Studio .slnx)"),
         text("Creates an XML Visual Studio solution (.slnx) and C++20 MSBuild project (.vcxproj) with main.cpp.")},
        {NewProjectTemplate::VsCppStaticLibrary, text("C++ Static Library (Visual Studio .sln)"),
         text("Creates a Visual Studio solution (.sln) and C++20 static library project (.vcxproj).")},
        {NewProjectTemplate::VsCppDynamicLibrary, text("C++ Dynamic Library (Visual Studio .sln)"),
         text("Creates a Visual Studio solution (.sln) and C++20 DLL project (.vcxproj) with export macros.")},
        {NewProjectTemplate::DotNetConsole, text(".NET Console App (C#)"),
         text("Creates an SDK-style .NET 10 C# console application (.csproj) with Program.cs.")},
        {NewProjectTemplate::DotNetClassLibrary, text(".NET Class Library (C#)"),
         text("Creates an SDK-style .NET 10 C# class library (.csproj) with Class1.cs.")},
        {NewProjectTemplate::EmptyFolder, text("Empty Folder"),
         text("Creates an empty folder and opens it in the File System view.")},
    }};
    for (const TemplateEntry& entry : entries) {
        auto* item = new QListWidgetItem(entry.label, templateList);
        item->setData(Qt::UserRole, static_cast<int>(entry.kind));
    }
    templateList->setCurrentRow(0);
    body->addWidget(templateList);

    auto* rightCol = new QVBoxLayout();
    rightCol->setSpacing(10);

    auto* descriptionLabel = new QLabel(&dialog);
    descriptionLabel->setObjectName(QStringLiteral("newProjectDescription"));
    descriptionLabel->setWordWrap(true);
    rightCol->addWidget(descriptionLabel);

    auto* form = new QFormLayout();
    form->setSpacing(8);

    auto* nameEdit = new QLineEdit(QStringLiteral("UntitledProject"), &dialog);
    nameEdit->setObjectName(QStringLiteral("newProjectNameEdit"));
    nameEdit->selectAll();
    form->addRow(text("Project name:"), nameEdit);

    QString initialLocation = QDir::toNativeSeparators(qs(defaultLocation));
    if (initialLocation.isEmpty()) {
        initialLocation = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (initialLocation.isEmpty())
            initialLocation = QDir::homePath();
        initialLocation = QDir::toNativeSeparators(initialLocation);
    }

    auto* locationRow = new QWidget(&dialog);
    auto* locationLayout = new QHBoxLayout(locationRow);
    locationLayout->setContentsMargins(0, 0, 0, 0);
    locationLayout->setSpacing(6);
    auto* locationEdit = new QLineEdit(initialLocation, locationRow);
    locationEdit->setObjectName(QStringLiteral("newProjectLocationEdit"));
    auto* browseBtn = new QPushButton(text("Browse..."), locationRow);
    browseBtn->setObjectName(QStringLiteral("newProjectBrowseButton"));
    locationLayout->addWidget(locationEdit, 1);
    locationLayout->addWidget(browseBtn);
    form->addRow(text("Location:"), locationRow);

    rightCol->addLayout(form);

    auto* previewLabel = new QLabel(&dialog);
    previewLabel->setObjectName(QStringLiteral("newProjectPathPreview"));
    previewLabel->setWordWrap(true);
    rightCol->addWidget(previewLabel);
    rightCol->addStretch(1);

    body->addLayout(rightCol, 1);
    outer->addLayout(body, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QPushButton* createBtn = buttons->button(QDialogButtonBox::Ok);
    if (createBtn)
        createBtn->setText(text("Create"));

    const auto updateState = [&] {
        const int row = std::clamp(templateList->currentRow(), 0, static_cast<int>(entries.size()) - 1);
        descriptionLabel->setText(entries[static_cast<std::size_t>(row)].description);
        const QString name = nameEdit->text().trimmed();
        const QString loc = locationEdit->text().trimmed();
        const QString combined =
            (loc.isEmpty() || name.isEmpty()) ? QString() : QDir::toNativeSeparators(QDir(loc).filePath(name));
        previewLabel->setText(combined.isEmpty() ? QString() : text("Project directory: %1").arg(combined));
        if (createBtn)
            createBtn->setEnabled(!name.isEmpty() && !loc.isEmpty());
    };

    QObject::connect(templateList, &QListWidget::currentRowChanged, &dialog, [&](int) { updateState(); });
    QObject::connect(nameEdit, &QLineEdit::textChanged, &dialog, [&](const QString&) { updateState(); });
    QObject::connect(locationEdit, &QLineEdit::textChanged, &dialog, [&](const QString&) { updateState(); });
    QObject::connect(browseBtn, &QPushButton::clicked, &dialog, [&] {
        const QString chosen =
            QFileDialog::getExistingDirectory(&dialog, text("Select Project Location"), locationEdit->text().trimmed());
        if (!chosen.isEmpty())
            locationEdit->setText(QDir::toNativeSeparators(chosen));
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);

    updateState();
    nameEdit->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;

    NewProjectSpec spec;
    spec.name = nameEdit->text().trimmed().toStdString();
    spec.location = QDir::toNativeSeparators(locationEdit->text().trimmed()).toStdString();
    const int row = std::clamp(templateList->currentRow(), 0, static_cast<int>(entries.size()) - 1);
    spec.kind = entries[static_cast<std::size_t>(row)].kind;
    return spec;
}

bool DialogWorkbenchPrompts::confirmDiscard(QWidget* parent, const QString& file, const QString& error) {
    const auto answer =
        QMessageBox::warning(parent, text("Cannot Save File"),
                             text("%1 could not be saved:\n%2\n\nClose it and lose the changes?").arg(file, error),
                             QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    return answer == QMessageBox::Discard;
}

bool DialogWorkbenchPrompts::confirmRerun(QWidget* parent, const QString& project) {
    const auto answer = QMessageBox::question(parent, text("Process Is Running"),
                                              text("%1 is running. Stop and rerun it?").arg(project),
                                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
    return answer == QMessageBox::Yes;
}

bool DialogWorkbenchPrompts::confirmExit(QWidget* parent) {
    return QMessageBox::question(parent, text("Confirm Exit"), text("Are you sure you want to exit FrostIDE?"),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes) == QMessageBox::Yes;
}

bool DialogWorkbenchPrompts::confirmProjectRecovery(QWidget* parent, const QString& project) {
    return QMessageBox::question(
               parent, text("Recover Project Edit"),
               text("Restore the project definitions recorded before the interrupted edit of %1?").arg(project),
               QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
}

} // namespace frost::app
