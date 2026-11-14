// SPDX-License-Identifier: Apache-2.0
#include "cmake_kit_dialog.hpp"

#include "frost/build/cmake_service.hpp"
#include "frost/build/cmake_toolchain.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/settings/document.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/store.hpp"
#include "frost/ui/dialog_chrome.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

namespace frost::app {
Result<std::string> appendCMakeKitPreset(std::string original, std::string_view presetJson) {
    auto preset = QJsonDocument::fromJson(QByteArray(presetJson.data(), static_cast<qsizetype>(presetJson.size())));
    if (!preset.isObject() || !preset.object().value("name").isString())
        return fail(ErrorCode::InvalidArgument, "kit must be a named configure preset");
    if (original.empty())
        return std::string("{\n  \"version\": 3,\n  \"configurePresets\": [") + std::string(presetJson) + "]\n}\n";
    // Strict JSON validation matches CMake. Parse only for validation; preserve the actual bytes.
    auto parsed = QJsonDocument::fromJson(QByteArray::fromStdString(original));
    if (!parsed.isObject() || parsed.object().value("version").toInt() < 3)
        return fail(ErrorCode::InvalidArgument,
                    "CMakeUserPresets.json must be a valid JSON object with preset version 3 or newer");
    auto document = settings::SettingsDocument::parse(original);
    if (!document)
        return std::unexpected(document.error());
    const auto* member = document->find("configurePresets");
    if (member) {
        const auto array = parsed.object().value("configurePresets");
        if (!array.isArray())
            return fail(ErrorCode::InvalidArgument, "configurePresets must be an array");
        for (const auto& entry : array.toArray())
            if (entry.toObject().value("name") == preset.object().value("name"))
                return fail(ErrorCode::InvalidArgument, "a configure preset already uses that kit name");
        original.insert(member->valueEnd - 1,
                        std::string(array.toArray().isEmpty() ? "" : ",") + std::string(presetJson));
    } else {
        const auto close = original.find_last_of('}');
        original.insert(close, std::string(document->members().empty() ? "" : ",") + "\n  \"configurePresets\": [" +
                                   std::string(presetJson) + "]\n");
    }
    return original;
}

void showCMakeKitDialog(io::IFileSystem& fs, settings::SettingsStore& store, build::CMakeService& service,
                        QWidget& parent, std::function<bool()> stillCurrent) {
    const auto root = service.sourceDirectory();
    const auto generation = service.profileGeneration();
    if (root.empty() || service.state() != build::BuildState::Idle)
        return;
    const auto path = io::joinPath(root, "CMakeUserPresets.json");
    if (!path)
        return;
    auto before = fs.readFile(*path);
    if (!before && before.error().code() != ErrorCode::NotFound) {
        QMessageBox::warning(&parent, QObject::tr("CMake Kit"), QString::fromStdString(before.error().message()));
        return;
    }
    const auto snapshot = store.snapshot();
    build::CMakeToolOptions options;
    options.cmakePath = snapshot->get(settings::key::toolsCmakePath);
    options.ninjaPath = snapshot->get(settings::key::toolsNinjaPath);
    options.pathVariable = qEnvironmentVariable("PATH").toStdString();
    options.sdkDirectory = qEnvironmentVariable("FROST_SDK").toStdString();
    // Filesystem discovery only: opening this settings UI never executes project code or tools.
    const auto tools = build::findCMakeTools(fs, options);
    QDialog dialog(&parent);
    dialog.setObjectName(QStringLiteral("cmakeKitDialog"));
    dialog.setWindowTitle(QObject::tr("Create CMake Kit"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel(QObject::tr("Creates a personal configure preset. Select it in the profile selector, then "
                                        "Reload to configure through the project trust gate."),
                            &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* discovered =
        new QLabel(tools ? QObject::tr("CMake: %1\nNinja: %2")
                               .arg(QString::fromStdString(tools->cmake), QString::fromStdString(tools->ninja))
                         : QString::fromStdString(tools.error().message()),
                   &dialog);
    discovered->setTextFormat(Qt::PlainText);
    layout->addWidget(discovered);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(&dialog);
    name->setObjectName(QStringLiteral("cmakeKitName"));
    auto* generator = new QComboBox(&dialog);
    generator->setEditable(true);
    generator->addItems({"Ninja", "Ninja Multi-Config", "Unix Makefiles"});
    auto* type = new QComboBox(&dialog);
    type->addItems({"Debug", "Release", "RelWithDebInfo", "MinSizeRel"});
    auto* compiler = new QComboBox(&dialog);
    compiler->setObjectName(QStringLiteral("cmakeKitCompiler"));
    compiler->setEditable(true);
    compiler->addItem(QString());
    if (const auto model = service.model())
        for (const auto& chain : model->toolchains)
            if (chain.language == "CXX" && !chain.compilerPath.empty())
                compiler->addItem(QString::fromStdString(chain.compilerPath));
    auto* toolchain = new QLineEdit(&dialog);
    form->addRow(QObject::tr("Kit name"), name);
    form->addRow(QObject::tr("Generator"), generator);
    form->addRow(QObject::tr("Build type"), type);
    form->addRow(QObject::tr("C++ compiler (detected or absolute path)"), compiler);
    form->addRow(QObject::tr("Toolchain file (absolute path, optional)"), toolchain);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    ui::installDialogChrome(&dialog, true);
    if (dialog.exec() != QDialog::Accepted || (stillCurrent && !stillCurrent()))
        return;
    const auto kitName = name->text().trimmed();
    // A kit name is also a build-directory component. Never allow it to escape the project.
    bool validName = !kitName.isEmpty();
    for (const auto ch : kitName)
        validName = validName && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                                  ch == '-' || ch == '_');
    const auto cxx = compiler->currentText().toStdString();
    const auto chain = toolchain->text().toStdString();
    const auto now = fs.readFile(*path);
    const bool unchanged =
        (before && now && *before == *now) || (!before && !now && now.error().code() == ErrorCode::NotFound);
    if (!validName || generator->currentText().trimmed().isEmpty() || (!cxx.empty() && !io::isAbsolutePath(cxx)) ||
        (!chain.empty() && !io::isAbsolutePath(chain)) || !unchanged || service.sourceDirectory() != root ||
        service.profileGeneration() != generation || service.state() != build::BuildState::Idle) {
        QMessageBox::warning(&parent, QObject::tr("CMake Kit"),
                             QObject::tr("Use a unique name of letters, digits, dash or underscore and absolute "
                                         "compiler/toolchain paths. Reopen if the project or presets changed."));
        return;
    }
    for (const auto& profile : service.profiles())
        if (profile.configurePreset == kitName.toStdString() || profile.name == kitName.toStdString()) {
            QMessageBox::warning(&parent, QObject::tr("CMake Kit"), QObject::tr("A profile already uses this name."));
            return;
        }
    const auto presets = project::loadCMakePresets(fs, root);
    if (!presets) {
        QMessageBox::warning(&parent, QObject::tr("CMake Kit"), QString::fromStdString(presets.error().message()));
        return;
    }
    if (*presets)
        for (const auto& existing : (**presets).configurePresets)
            if (existing.name == kitName.toStdString()) {
                QMessageBox::warning(
                    &parent, QObject::tr("CMake Kit"),
                    QObject::tr("A configure preset already uses this name, including hidden presets."));
                return;
            }
    QJsonObject cache{{"CMAKE_BUILD_TYPE", type->currentText()}};
    if (!cxx.empty())
        cache.insert("CMAKE_CXX_COMPILER", QString::fromStdString(cxx));
    QJsonObject preset{{"name", kitName},
                       {"generator", generator->currentText()},
                       {"binaryDir", QStringLiteral("${sourceDir}/cmake-build-") + kitName},
                       {"cacheVariables", cache}};
    if (!chain.empty())
        preset.insert("toolchainFile", QString::fromStdString(chain));
    const auto json = QJsonDocument(preset).toJson(QJsonDocument::Compact).toStdString();
    const auto updated = appendCMakeKitPreset(before ? *before : std::string(), json);
    if (!updated) {
        QMessageBox::warning(&parent, QObject::tr("CMake Kit"), QString::fromStdString(updated.error().message()));
        return;
    }
    auto saved = fs.safeWrite(*path, *updated, {});
    if (!saved)
        QMessageBox::warning(&parent, QObject::tr("CMake Kit"), QString::fromStdString(saved.error().message()));
    if (saved && saved->warning)
        QMessageBox::warning(&parent, QObject::tr("CMake Kit"), QString::fromStdString(saved->warning->message()));
    if (saved && (!stillCurrent || stillCurrent())) {
        io::FileChangeBatch batch;
        batch.changes.push_back({.kind = io::FileChangeKind::Modified, .path = *path});
        service.filesChanged(batch);
    }
    // The existing profile reload/configure path retains the project trust gate.
}
} // namespace frost::app
