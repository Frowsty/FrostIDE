// SPDX-License-Identifier: Apache-2.0
// File/target template choice and exact generated-file/project preview, before one Apply.
#include "frost/app/workbench.hpp"

#include "cmake_integration.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

namespace frost::app {
bool Workbench::confirmSourceTemplatePlan(const document::RefactoringPlan& plan) {
    QPointer<Workbench> owner(this);
    QDialog dialog;
    dialog.setObjectName("sourceTemplatePreview");
    dialog.setWindowTitle("Preview generated files and membership");
    dialog.setWindowModality(Qt::ApplicationModal);
    connect(this, &QObject::destroyed, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        "No files have been changed. Apply creates the files and changes membership as one Undo operation.", &dialog));
    auto* tabs = new QTabWidget(&dialog);
    layout->addWidget(tabs);
    auto show = [&](const std::string& path, std::string_view before, std::string_view after) {
        auto* pane = new QPlainTextEdit(tabs);
        pane->setReadOnly(true);
        const auto text =
            before.empty() ? std::string(after) : "BEFORE\n" + std::string(before) + "\nAFTER\n" + std::string(after);
        pane->setPlainText(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
        const auto label = QString::fromStdString(path);
        tabs->addTab(pane, label);
        tabs->setTabToolTip(tabs->count() - 1, label);
    };
    for (const auto& file : plan.creates)
        show(file.path, {}, file.afterBytes);
    for (const auto& file : plan.disk)
        show(file.path, file.expectedBytes, file.afterBytes);
    for (const auto& file : plan.buffers)
        show(file.path, file.expectedText, file.afterText);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(900, 600);
    const auto result = dialog.exec();
    return owner && result == QDialog::Accepted;
}
void Workbench::showSourceTemplateDialog() {
    if (m_projectRoot.empty() || !m_window)
        return;
    QPointer<Workbench> owner(this);
    const auto model = m_cmake ? m_cmake->shownModel() : nullptr;
    const auto solution = m_solution;
    QDialog dialog;
    dialog.setObjectName("sourceTemplateDialog");
    dialog.setWindowTitle("New File / Class / Target from Template");
    dialog.setWindowModality(Qt::ApplicationModal);
    connect(this, &QObject::destroyed, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* kind = new QComboBox(&dialog);
    kind->setObjectName("sourceTemplateKind");
    kind->addItems({"C++ class (header and source)", "C++ source", "C++ header", "CMake script", "Lua module"});
    if (model)
        kind->addItems({"New CMake executable target", "New CMake static library target"});
    form->addRow("Template:", kind);
    auto* name = new QLineEdit(&dialog);
    name->setObjectName("sourceTemplateName");
    name->setPlaceholderText("Widget or namespace::Widget for a class");
    form->addRow("Name:", name);
    auto* directory = new QLineEdit(QString::fromStdString(m_projectRoot), &dialog);
    directory->setObjectName("sourceTemplateDirectory");
    form->addRow("Existing directory:", directory);
    auto* browse = new QPushButton("Browse existing directory...", &dialog);
    form->addRow({}, browse);
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        const auto selected = QFileDialog::getExistingDirectory(&dialog, "Template directory", directory->text());
        if (!selected.isEmpty())
            directory->setText(selected);
    });
    auto* membership = new QComboBox(&dialog);
    membership->setObjectName("sourceTemplateMembership");
    if (model)
        for (const auto& target : model->targets) {
            if (target.generatorProvided || target.type == project::CMakeTargetType::Utility ||
                target.type == project::CMakeTargetType::InterfaceLibrary)
                continue;
            membership->addItem(QString::fromStdString(target.name), QString::fromStdString(target.id));
        }
    else if (solution)
        for (const auto& project : solution->projects()) {
            if (project.kind == frost::project::ProjectKind::Cpp &&
                project.nodeKind == frost::project::NodeKind::Project)
                membership->addItem(QString::fromStdString(project.name), QString::fromStdString(project.path));
        }
    else
        membership->addItem("Folder only — no build target membership");
    form->addRow("Target / project:", membership);
    auto* author = new QLineEdit(&dialog);
    author->setObjectName("sourceTemplateAuthor");
    form->addRow("Author (optional):", author);
    auto* description = new QLineEdit(&dialog);
    description->setObjectName("sourceTemplateDescription");
    form->addRow("Description (optional):", description);
    layout->addLayout(form);
    auto* explanation = new QLabel(
        "Files are UTF-8, in an existing directory. You will preview all file and membership changes before Apply.",
        &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    connect(kind, &QComboBox::currentIndexChanged, &dialog, [&](int index) { membership->setEnabled(index < 5); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setText("Prepare preview");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(650, 400);
    if (dialog.exec() != QDialog::Accepted || !owner)
        return;
    if ((m_cmake ? m_cmake->shownModel() : nullptr) != model || m_solution != solution) {
        notify("Template context changed", "Open the template dialog again for the current project.",
               ui::NotificationKind::Warning);
        return;
    }
    SourceTemplateSpec spec;
    const auto selected = kind->currentIndex();
    spec.file.kind = selected == 1   ? lang::FileTemplateKind::Source
                     : selected == 2 ? lang::FileTemplateKind::Header
                     : selected == 3 ? lang::FileTemplateKind::CMakeScript
                     : selected == 4 ? lang::FileTemplateKind::LuaModule
                     : selected == 5 ? lang::FileTemplateKind::Executable
                                     : lang::FileTemplateKind::Class;
    spec.file.name = name->text().trimmed().toUtf8().toStdString();
    spec.file.author = author->text().toUtf8().toStdString();
    spec.file.description = description->text().toUtf8().toStdString();
    spec.directory = QDir::fromNativeSeparators(directory->text()).toUtf8().toStdString();
    spec.newCMakeTarget = selected >= 5;
    spec.staticLibrary = selected == 6;
    if (!spec.newCMakeTarget) {
        if (model)
            spec.cmakeTargetId = membership->currentData().toString().toStdString();
        else if (solution)
            spec.vcxprojPath = membership->currentData().toString().toStdString();
    }
    auto created = createSourceTemplate(spec);
    if (!created && owner)
        notify("Template creation", QString::fromStdString(created.error().message()), ui::NotificationKind::Error);
}
} // namespace frost::app
