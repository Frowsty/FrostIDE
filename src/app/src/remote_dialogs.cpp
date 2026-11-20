// SPDX-License-Identifier: Apache-2.0
// Explicit named remote endpoint approval, reversible mappings and separate WSL build profiles.
#include "remote_dialogs.hpp"

#include "frost/build/cmake_toolchain.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
namespace frost::app {
namespace {
QString q(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::string bytes(const QString& value) { return value.toUtf8().toStdString(); }
} // namespace
Result<std::vector<build::RemoteTarget>> editRemoteTargetForms(QWidget* parent,
                                                               std::vector<build::RemoteTarget> targets) {
    QDialog dialog(parent);
    dialog.setObjectName("remoteTargetsDialog");
    dialog.setWindowTitle("Remote targets");
    dialog.resize(970, 650);
    auto* outer = new QVBoxLayout(&dialog);
    auto* guidance = new QLabel(
        "These application targets are separate from project trust. WSL always uses the named distribution. SSH uses "
        "verified known hosts and noninteractive key authentication; no project SSH configuration is loaded.",
        &dialog);
    guidance->setWordWrap(true);
    outer->addWidget(guidance);
    auto* body = new QHBoxLayout;
    outer->addLayout(body, 1);
    auto* left = new QVBoxLayout;
    body->addLayout(left, 1);
    auto* list = new QListWidget(&dialog);
    list->setObjectName("remoteTargetList");
    left->addWidget(list);
    auto* addWsl = new QPushButton("Add WSL target", &dialog);
    auto* addSsh = new QPushButton("Add Linux SSH target", &dialog);
    auto* remove = new QPushButton("Remove target", &dialog);
    addWsl->setObjectName("remoteTargetAddWsl");
    addSsh->setObjectName("remoteTargetAddSsh");
    left->addWidget(addWsl);
    left->addWidget(addSsh);
    left->addWidget(remove);
    auto* right = new QVBoxLayout;
    body->addLayout(right, 3);
    auto* form = new QFormLayout;
    right->addLayout(form);
    const auto field = [&](const char* name, const char* label) {
        auto* edit = new QLineEdit(&dialog);
        edit->setObjectName(name);
        form->addRow(label, edit);
        return edit;
    };
    auto* id = field("remoteTargetId", "Target id:");
    auto* name = field("remoteTargetName", "Name:");
    auto* executable = field("remoteTargetExecutable", "Local wsl.exe / ssh executable:");
    auto* distribution = field("remoteTargetDistribution", "WSL distribution:");
    auto* host = field("remoteTargetHost", "SSH host:");
    auto* user = field("remoteTargetUser", "SSH user:");
    auto* port = new QSpinBox(&dialog);
    port->setObjectName("remoteTargetPort");
    port->setRange(1, 65535);
    port->setValue(22);
    form->addRow("SSH port:", port);
    auto* identity = field("remoteTargetIdentity", "SSH key file (optional):");
    auto* python = field("remoteTargetPython", "Remote Linux Python executable:");
    auto* mappings = new QTableWidget(0, 2, &dialog);
    mappings->setObjectName("remoteTargetMappings");
    mappings->setHorizontalHeaderLabels({"Local root", "Remote Linux root"});
    mappings->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    right->addWidget(mappings, 1);
    auto* mappingButtons = new QHBoxLayout;
    auto* addMapping = new QPushButton("Add path mapping", &dialog);
    auto* removeMapping = new QPushButton("Remove mapping", &dialog);
    mappingButtons->addWidget(addMapping);
    mappingButtons->addWidget(removeMapping);
    right->addLayout(mappingButtons);
    auto* approval = new QCheckBox("Allow this endpoint to execute commands from trusted projects", &dialog);
    approval->setObjectName("remoteTargetApproval");
    right->addWidget(approval);
    auto* error = new QLabel(&dialog);
    error->setObjectName("remoteTargetError");
    error->setWordWrap(true);
    outer->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    outer->addWidget(buttons);
    int current = -1;
    bool loading = false;
    const auto store = [&] {
        if (loading || current < 0 || static_cast<std::size_t>(current) >= targets.size())
            return;
        auto& target = targets[static_cast<std::size_t>(current)];
        target.id = bytes(id->text().trimmed());
        target.name = bytes(name->text().trimmed());
        target.transportExecutable = bytes(executable->text().trimmed());
        target.distribution =
            target.transport == build::RemoteTransport::Wsl ? bytes(distribution->text().trimmed()) : "";
        target.host = target.transport == build::RemoteTransport::Ssh ? bytes(host->text().trimmed()) : "";
        target.user = target.transport == build::RemoteTransport::Ssh ? bytes(user->text().trimmed()) : "";
        target.identityFile = target.transport == build::RemoteTransport::Ssh ? bytes(identity->text().trimmed()) : "";
        target.port = static_cast<unsigned>(port->value());
        target.remotePython = bytes(python->text().trimmed());
        target.executionApproved = approval->isChecked();
        target.mappings.clear();
        for (int row = 0; row < mappings->rowCount(); ++row)
            target.mappings.push_back({mappings->item(row, 0) ? bytes(mappings->item(row, 0)->text()) : "",
                                       mappings->item(row, 1) ? bytes(mappings->item(row, 1)->text()) : ""});
        if (list->item(current))
            list->item(current)->setText(q(target.name));
    };
    const auto show = [&](int row) {
        loading = true;
        current = row;
        const bool valid = row >= 0 && static_cast<std::size_t>(row) < targets.size();
        for (auto* edit : {id, name, executable, distribution, host, user, identity, python})
            edit->setEnabled(valid);
        port->setEnabled(valid);
        mappings->setEnabled(valid);
        approval->setEnabled(valid);
        remove->setEnabled(valid);
        addMapping->setEnabled(valid);
        removeMapping->setEnabled(valid);
        if (valid) {
            const auto& target = targets[static_cast<std::size_t>(row)];
            id->setText(q(target.id));
            name->setText(q(target.name));
            executable->setText(q(target.transportExecutable));
            distribution->setText(q(target.distribution));
            host->setText(q(target.host));
            user->setText(q(target.user));
            port->setValue(static_cast<int>(target.port));
            identity->setText(q(target.identityFile));
            python->setText(q(target.remotePython));
            approval->setChecked(target.executionApproved);
            const bool wsl = target.transport == build::RemoteTransport::Wsl;
            form->setRowVisible(distribution, wsl);
            form->setRowVisible(host, !wsl);
            form->setRowVisible(user, !wsl);
            form->setRowVisible(port, !wsl);
            form->setRowVisible(identity, !wsl);
            mappings->setRowCount(static_cast<int>(target.mappings.size()));
            for (std::size_t i = 0; i < target.mappings.size(); ++i) {
                mappings->setItem(static_cast<int>(i), 0, new QTableWidgetItem(q(target.mappings[i].localRoot)));
                mappings->setItem(static_cast<int>(i), 1, new QTableWidgetItem(q(target.mappings[i].remoteRoot)));
            }
        } else {
            for (auto* edit : {id, name, executable, distribution, host, user, identity, python})
                edit->clear();
            mappings->setRowCount(0);
            approval->setChecked(false);
        }
        loading = false;
    };
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, [&](int row) {
        store();
        show(row);
    });
    for (auto* edit : {id, executable, distribution, host, user, identity, python})
        QObject::connect(edit, &QLineEdit::textEdited, &dialog, [&] {
            if (!loading)
                approval->setChecked(false);
        });
    QObject::connect(port, &QSpinBox::valueChanged, &dialog, [&] {
        if (!loading)
            approval->setChecked(false);
    });
    QObject::connect(mappings, &QTableWidget::cellChanged, &dialog, [&] {
        if (!loading)
            approval->setChecked(false);
    });
    const auto add = [&](build::RemoteTransport transport) {
        store();
        build::RemoteTarget target;
        target.transport = transport;
        target.name = transport == build::RemoteTransport::Wsl ? "WSL target" : "Linux SSH target";
        for (unsigned i = 1;; ++i) {
            target.id = (transport == build::RemoteTransport::Wsl ? "wsl." : "ssh.") + std::to_string(i);
            if (std::ranges::none_of(targets, [&](const auto& existing) { return existing.id == target.id; }))
                break;
        }
        targets.push_back(target);
        list->addItem(q(target.name));
        list->setCurrentRow(static_cast<int>(targets.size()) - 1);
    };
    QObject::connect(addWsl, &QPushButton::clicked, &dialog, [&] { add(build::RemoteTransport::Wsl); });
    QObject::connect(addSsh, &QPushButton::clicked, &dialog, [&] { add(build::RemoteTransport::Ssh); });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
        if (current < 0)
            return;
        const auto row = current;
        current = -1;
        targets.erase(targets.begin() + row);
        delete list->takeItem(row);
        show(list->currentRow());
    });
    QObject::connect(addMapping, &QPushButton::clicked, &dialog, [&] {
        mappings->insertRow(mappings->rowCount());
        approval->setChecked(false);
    });
    QObject::connect(removeMapping, &QPushButton::clicked, &dialog, [&] {
        if (mappings->currentRow() >= 0) {
            mappings->removeRow(mappings->currentRow());
            approval->setChecked(false);
        }
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        store();
        auto validated = build::parseRemoteTargets(build::remoteTargetsJson(targets));
        if (!validated) {
            error->setText(q(validated.error().message()));
            return;
        }
        dialog.accept();
    });
    for (const auto& target : targets)
        list->addItem(q(target.name));
    if (!targets.empty())
        list->setCurrentRow(0);
    else
        show(-1);
    if (dialog.exec() != QDialog::Accepted)
        return fail(ErrorCode::Cancelled, "Remote target changes cancelled");
    return targets;
}
Result<WslProfileChoice> editWslProfileForm(QWidget* parent, build::WslCMakeProfile profile,
                                            const std::vector<build::RemoteTarget>& targets) {
    QDialog dialog(parent);
    dialog.setObjectName("wslCMakeProfileDialog");
    dialog.setWindowTitle("WSL CMake toolchain");
    dialog.resize(760, 520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    const auto field = [&](const char* key, const char* label, const std::string& value) {
        auto* edit = new QLineEdit(q(value), &dialog);
        edit->setObjectName(key);
        form->addRow(label, edit);
        return edit;
    };
    auto* name = field("wslProfileName", "Profile name:", profile.name);
    auto* source = field("wslProfileSource", "Local source directory:", profile.localSourceDirectory);
    source->setReadOnly(true);
    auto* target = new QComboBox(&dialog);
    target->setObjectName("wslProfileEndpoint");
    for (const auto& endpoint : targets)
        if (endpoint.transport == build::RemoteTransport::Wsl)
            target->addItem(q(endpoint.name) +
                                (endpoint.executionApproved ? QString{} : QStringLiteral(" (not approved)")),
                            q(endpoint.id));
    target->setCurrentIndex(target->findData(q(profile.targetId)));
    if (target->currentIndex() < 0 && target->count() > 0)
        target->setCurrentIndex(0);
    form->addRow("Named WSL target:", target);
    auto* cmake = field("wslProfileCMake", "Remote CMake executable:", profile.cmakeProgram);
    auto* ninja = field("wslProfileNinja", "Remote Ninja executable:", profile.ninjaProgram);
    auto* buildDirectory = field("wslProfileBuild", "Separate Linux build directory:", profile.buildDirectory);
    auto* selected = field("wslProfileTarget", "Build target (empty = generator default):", profile.buildTarget);
    auto* definitions = new QPlainTextEdit(&dialog);
    definitions->setObjectName("wslProfileDefinitions");
    for (const auto& value : profile.cacheDefinitions)
        definitions->appendPlainText(q(value));
    form->addRow("Cache KEY[:TYPE]=VALUE, one per line:", definitions);
    auto* guidance = new QLabel(
        QStringLiteral("Remote tools must match CMake %1 and Ninja %2. Configure and Build run only on explicit "
                       "request in the named distribution; source navigation uses its explicit path mappings.")
            .arg(q(build::pinnedCMakeVersion()), q(build::pinnedNinjaVersion())),
        &dialog);
    guidance->setWordWrap(true);
    layout->addWidget(guidance);
    auto* error = new QLabel(&dialog);
    error->setWordWrap(true);
    error->setObjectName("wslProfileError");
    layout->addWidget(error);
    auto* actions = new QHBoxLayout;
    layout->addLayout(actions);
    std::optional<build::WslCMakeOperation> operation;
    const auto accept = [&](std::optional<build::WslCMakeOperation> requested) {
        profile.name = bytes(name->text().trimmed());
        profile.targetId = bytes(target->currentData().toString());
        profile.cmakeProgram = bytes(cmake->text().trimmed());
        profile.ninjaProgram = bytes(ninja->text().trimmed());
        profile.buildDirectory = bytes(buildDirectory->text().trimmed());
        profile.buildTarget = bytes(selected->text().trimmed());
        profile.cacheDefinitions.clear();
        for (const auto& line : definitions->toPlainText().split('\n'))
            if (!line.trimmed().isEmpty())
                profile.cacheDefinitions.push_back(bytes(line));
        const auto endpoint =
            std::find_if(targets.begin(), targets.end(), [&](const auto& row) { return row.id == profile.targetId; });
        if (endpoint == targets.end()) {
            error->setText("Select a saved named WSL target first.");
            return;
        }
        auto valid =
            build::wslCMakeCommand(profile, *endpoint, requested.value_or(build::WslCMakeOperation::Configure), 1);
        if (!valid) {
            error->setText(q(valid.error().message()));
            return;
        }
        if (auto retained =
                build::parseWslCMakeProfiles(build::wslCMakeProfilesJson(std::vector<build::WslCMakeProfile>{profile}));
            !retained) {
            error->setText(q(retained.error().message()));
            return;
        }
        operation = requested;
        dialog.accept();
    };
    for (const auto& [label, value] : std::initializer_list<std::pair<const char*, build::WslCMakeOperation>>{
             {"Configure", build::WslCMakeOperation::Configure},
             {"Build", build::WslCMakeOperation::Build},
             {"Clean", build::WslCMakeOperation::Clean},
             {"List targets", build::WslCMakeOperation::ListTargets}}) {
        auto* button = new QPushButton(label, &dialog);
        actions->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, &dialog, [&, value] { accept(value); });
    }
    auto* save = new QPushButton("Save without running", &dialog);
    auto* cancel = new QPushButton("Cancel", &dialog);
    actions->addWidget(save);
    actions->addWidget(cancel);
    QObject::connect(save, &QPushButton::clicked, &dialog, [&] { accept(std::nullopt); });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return fail(ErrorCode::Cancelled, "WSL CMake profile cancelled");
    return WslProfileChoice{std::move(profile), operation};
}
} // namespace frost::app
