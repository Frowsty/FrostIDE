// SPDX-License-Identifier: Apache-2.0
#include "frost/app/main_window.hpp"
#include "frost/app/workbench.hpp"
#include "frost/build/build_service.hpp"
#include "frost/build/dependencies.hpp"
#include "frost/io/path.hpp"
#include "frost/project/project_edit.hpp"
#include "frost/rpc/json.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "cmake_integration.hpp"
#include "cmake_kit_dialog.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include <stop_token>
#include <thread>

namespace frost::app {
namespace {
QString q(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
class DependencyProgress final : public QDialog {
public:
    DependencyProgress(QWidget* parent, std::stop_source& stop) : QDialog(parent), m_stop(stop) {
        setWindowTitle(tr("Dependencies"));
        auto* layout = new QVBoxLayout(this);
        m_label = new QLabel(tr("Running the consented dependency operation..."), this);
        layout->addWidget(m_label);
        auto* cancel = new QPushButton(tr("Cancel"), this);
        layout->addWidget(cancel);
        connect(cancel, &QPushButton::clicked, this, &DependencyProgress::reject);
    }
    void reject() override {
        m_stop.request_stop();
        m_label->setText(tr("Cancelling dependency operation..."));
    }

private:
    std::stop_source& m_stop;
    QLabel* m_label = nullptr;
};
} // namespace
void Workbench::showDependencies() {
    QPointer<Workbench> alive(this);
    QWidget* parent = activeTopWindow();
    if (!parent || !m_build || !m_buildService || m_projectRoot.empty())
        return;
    const auto root = m_projectRoot;
    const auto solution = m_solution;
    if (m_buildService->state() != build::BuildState::Idle || anyDebugSessionActive() ||
        (m_cmake && m_cmake->service().state() != build::BuildState::Idle)) {
        QMessageBox::warning(parent, tr("Dependencies"), tr("Wait for builds, configuration and debugging to finish."));
        return;
    }
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("dependencyManagerDialog"));
    dialog.setWindowTitle(tr("Install or Restore Dependencies"));
    auto* layout = new QFormLayout(&dialog);
    auto* manager = new QComboBox(&dialog);
    manager->addItems({tr("vcpkg manifest (no downloads)"), tr("Conan 2 (local cache only)"),
                       tr("Native NuGet packages.config (local feed)")});
#ifndef _WIN32
    manager->setItemData(2, 0, Qt::UserRole - 1);
#endif
    auto* executable = new QLineEdit(&dialog);
    auto* input = new QLineEdit(&dialog);
    auto* output = new QLineEdit(QStringLiteral(".frost/dependencies/vcpkg"), &dialog);
    auto* feed = new QLineEdit(&dialog);
    auto* triplet = new QLineEdit(QStringLiteral("x64-windows"), &dialog);
#ifndef _WIN32
    triplet->setText(QStringLiteral("x64-linux"));
#endif
    auto* host = new QLineEdit(&dialog);
    auto* build = new QLineEdit(&dialog);
    auto* native = new QComboBox(&dialog);
    native->addItem(tr("Do not change native project"), QString());
    if (solution)
        for (const auto& project : solution->projects())
            if (project.vcxproj)
                native->addItem(q(project.name), q(project.path));
    layout->addRow(tr("Manager"), manager);
    layout->addRow(tr("Absolute executable"), executable);
    layout->addRow(tr("Manifest / packages.config"), input);
    layout->addRow(tr("Output relative to workspace"), output);
    layout->addRow(tr("Local overlay ports / NuGet feed"), feed);
    layout->addRow(tr("vcpkg triplet"), triplet);
    layout->addRow(tr("Conan host profile file"), host);
    layout->addRow(tr("Conan build profile file"), build);
    layout->addRow(tr("Apply vcpkg to native project"), native);
    auto* explanation = new QLabel(tr("Package recipes and profiles can execute code. Nothing runs until you review "
                                      "and consent to the exact command. No manager is downloaded or bootstrapped."),
                                   &dialog);
    explanation->setWordWrap(true);
    layout->addRow(explanation);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const auto defaults = [&] {
        const auto snapshot = m_services.settings.snapshot();
        const int index = manager->currentIndex();
        auto tool = index == 0 ? snapshot->get(settings::key::toolsVcpkgPath)
                               : (index == 1 ? snapshot->get(settings::key::toolsConanPath)
                                             : snapshot->get(settings::key::toolsNugetPath));
        if (tool.empty() && index == 0) {
            const auto sdk = qEnvironmentVariable("FROST_SDK").toStdString();
            if (io::isAbsolutePath(sdk)) {
#ifdef _WIN32
                auto candidate = io::joinPath(sdk, "vcpkg/vcpkg.exe");
#else
                auto candidate = io::joinPath(sdk, "vcpkg/vcpkg");
#endif
                if (candidate) {
                    auto exists = m_services.fs.exists(*candidate);
                    if (exists && *exists)
                        tool = *candidate;
                }
            }
        }
        executable->setText(q(tool));
        input->setText(
            q(io::joinPath(root, index == 0 ? "vcpkg.json" : (index == 1 ? "conanfile.txt" : "packages.config"))
                  .value()));
        output->setText(index == 0 ? ".frost/dependencies/vcpkg"
                                   : (index == 1 ? ".frost/dependencies/conan" : "packages"));
        triplet->setEnabled(index == 0);
        native->setEnabled(index == 0);
        host->setEnabled(index == 1);
        build->setEnabled(index == 1);
        feed->setEnabled(index != 1);
    };
    connect(manager, &QComboBox::currentIndexChanged, &dialog, defaults);
    defaults();
    if (dialog.exec() != QDialog::Accepted || !alive || root != m_projectRoot || m_shutDown)
        return;
    build::DependencyRequest request{.manager = static_cast<build::DependencyManager>(manager->currentIndex()),
                                     .root = root,
                                     .executable = executable->text().toStdString(),
                                     .input = input->text().toStdString(),
                                     .outputDirectory = output->text().toStdString(),
                                     .localFeed = feed->isEnabled() ? feed->text().toStdString() : std::string{},
                                     .triplet = triplet->text().toStdString(),
                                     .hostProfile = host->text().toStdString(),
                                     .buildProfile = build->text().toStdString()};
    auto plan = build::planDependencies(m_services.fs, request);
    const auto fail = [&](std::string_view message) { QMessageBox::warning(parent, tr("Dependencies"), q(message)); };
    if (!plan) {
        fail(plan.error().message());
        return;
    }
    QStringList untrusted;
    for (const auto& folder : plan->trustRoots)
        if (!m_services.trust.isTrusted(folder))
            untrusted.push_back(q(folder));
    if (!untrusted.empty()) {
        ui::TrustDialog trust(*m_build, untrusted, parent, ui::TrustPurpose::Build);
        if (trust.exec() != QDialog::Accepted || !alive)
            return;
    }
    QString command =
        tr("Working directory: ") + q(plan->process.workingDirectory) + "\n" + q(plan->process.executable);
    for (const auto& argument : plan->process.arguments)
        command += "\n  " + q(rpc::Json(argument).dump());
    for (const auto& [name, value] : plan->process.environment)
        command += "\n" + q(name) + (value ? "=" + q(*value) : tr(" (unset)"));
    if (QMessageBox::question(parent, tr("Run Dependency Manager"),
                              tr("Execute this command and allow its package recipes to write to the shown "
                                 "output/cache directories?\n\n") +
                                  command,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    if (!alive || root != m_projectRoot || solution != m_solution || m_shutDown)
        return;
    auto consent = build::consentDependencies(*plan, m_services.trust);
    if (!consent) {
        fail(consent.error().message());
        return;
    }
    auto admission = m_buildService->reserveExternalLaunch();
    if (!admission) {
        fail(admission.error().message());
        return;
    }
    std::optional<Result<io::ProcessResult>> result;
    std::stop_source stop;
    DependencyProgress progress(parent, stop);
    auto* fs = &m_services.fs;
    auto* launcher = &m_services.launcher;
    auto* trust = &m_services.trust;
    std::thread worker([&, fs, launcher, trust, consent = *consent] {
        result = build::installDependencies(*fs, *launcher, *trust, consent, stop.get_token());
        QMetaObject::invokeMethod(&progress, &QDialog::accept, Qt::QueuedConnection);
    });
    progress.exec();
    if (progress.result() != QDialog::Accepted)
        stop.request_stop();
    worker.join();
    if (!alive)
        return;
    m_buildService->releaseExternalLaunch(*admission);
    if (!result || !*result) {
        if (result)
            fail(result->error().message());
        return;
    }
    if ((**result).exitCode != 0 || (**result).signal != 0) {
        fail((**result).stderrBytes + "\n" + (**result).stdoutBytes);
        return;
    }
    if (!alive || root != m_projectRoot || solution != m_solution || m_shutDown)
        return;
    QMessageBox finished(QMessageBox::Information, tr("Dependencies"), tr("Dependency manager exited successfully."),
                         QMessageBox::Ok, parent);
    finished.setDetailedText(q((**result).stdoutBytes + (**result).stderrBytes));
    finished.exec();
    if (!alive || root != m_projectRoot || solution != m_solution || m_shutDown)
        return;
    if (auto current = build::validateDependencyConsent(m_services.fs, m_services.trust, *consent); !current) {
        fail(current.error().message());
        return;
    }
    if (request.manager == build::DependencyManager::Vcpkg && !native->currentData().toString().isEmpty()) {
        project::ProjectEdit edit;
        edit.kind = project::ProjectEditKind::SetProperties;
        edit.earlyProperties = true;
        edit.definition = native->currentData().toString().toStdString();
        const auto active = m_build->activeConfigurationIndex();
        for (const auto& item : solution->projects())
            if (item.path == edit.definition && active && *active < item.configurationMap.size() &&
                item.configurationMap[*active]) {
                edit.configuration = item.configurationMap[*active]->configuration;
                edit.platform = item.configurationMap[*active]->platform;
            }
        const auto asDirectory = [](std::string value) {
            if (!value.ends_with('/') && !value.ends_with('\\'))
                value.push_back(io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/');
            return value;
        };
        const auto vcpkgRoot = asDirectory(std::string(io::parentPath(plan->process.executable)));
        const auto installed = asDirectory(plan->resolvedOutput);
        if (vcpkgRoot.find_first_of("$%@;\"|<>\r\n") != std::string::npos ||
            installed.find_first_of("$%@;\"|<>\r\n") != std::string::npos ||
            plan->physicalInputs.at(plan->request.input).find_first_of("$%@;\"|<>\r\n") != std::string::npos) {
            fail("These dependency paths contain MSBuild expression/list characters; configure the imports manually.");
            return;
        }
        auto definition = m_services.fs.readFileSnapshot(edit.definition, 4 * 1024 * 1024);
        if (!definition || q(*definition).contains("ForceImportAfterCppProps", Qt::CaseInsensitive) ||
            q(*definition).contains("ForceImportAfterCppTargets", Qt::CaseInsensitive)) {
            fail("This project already declares import hooks, or its definition cannot be read safely; configure vcpkg "
                 "imports manually.");
            return;
        }
        edit.propertyUpdates = {
            {"Prop.VcpkgEnabled", "true"},
            {"Prop.VcpkgEnableManifest", "true"},
            {"Prop.VcpkgManifestInstall", "false"},
            {"Prop.VcpkgRoot", vcpkgRoot},
            {"Prop.VcpkgInstalledDir", installed},
            {"Prop.VcpkgTriplet", request.triplet},
            {"Prop.VcpkgManifestRoot",
             asDirectory(std::string(io::parentPath(plan->physicalInputs.at(plan->request.input))))},
            {"Prop.ForceImportAfterCppProps",
             io::joinPath(vcpkgRoot, "scripts/buildsystems/msbuild/vcpkg.props").value()},
            {"Prop.ForceImportAfterCppTargets",
             io::joinPath(vcpkgRoot, "scripts/buildsystems/msbuild/vcpkg.targets").value()}};
        if (QMessageBox::question(parent, tr("Apply Native vcpkg"),
                                  tr("Set vcpkg properties and early import hooks for %1 (%2|%3)? Automatic manifest "
                                     "install remains disabled.")
                                      .arg(q(edit.definition), q(edit.configuration), q(edit.platform)),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            if (alive && root == m_projectRoot && solution == m_solution && !m_shutDown) {
                if (auto current = build::validateDependencyConsent(m_services.fs, m_services.trust, *consent);
                    !current) {
                    fail(current.error().message());
                    return;
                }
                if (auto refused = submitProjectEdit(std::move(edit), tr("Apply Native vcpkg")))
                    fail(refused->toStdString());
            }
    }
    if (request.manager == build::DependencyManager::NativeNuget || !m_cmake)
        return;
    const auto profile = build::dependencyCmakeProfile(*plan);
    auto path = io::joinPath(root, "CMakeUserPresets.json").value();
    auto before = m_services.fs.readFile(path);
    if (!before && before.error().code() != ErrorCode::NotFound) {
        fail(before.error().message());
        return;
    }
    QJsonObject cache{{"CMAKE_BUILD_TYPE", "Debug"}};
    for (const auto& value : profile.cacheVariables) {
        if (value.find('$') != std::string::npos) {
            fail("Dependency paths containing CMake preset macro characters require manual preset setup.");
            return;
        }
        const auto colon = value.find(':');
        const auto equals = value.find('=');
        cache.insert(q(value.substr(0, colon)), q(value.substr(equals + 1)));
    }
    const auto toolchain = profile.cacheVariables.front().substr(profile.cacheVariables.front().find('=') + 1);
    QJsonObject preset{{"name", q(profile.name)},
                       {"toolchainFile", q(toolchain)},
                       {"generator", "Ninja"},
                       {"binaryDir", "${sourceDir}/" + q(profile.buildDirectory)},
                       {"cacheVariables", cache}};
    const auto profiles = m_cmake->service().profiles();
    const auto active = m_cmake->service().activeProfileIndex();
    if (active && *active < profiles.size() && !profiles[*active].configurePreset.empty())
        preset.insert("inherits", q(profiles[*active].configurePreset));
    auto updated = appendCMakeKitPreset(before ? *before : std::string{},
                                        QJsonDocument(preset).toJson(QJsonDocument::Compact).toStdString());
    if (!updated) {
        fail(updated.error().message());
        return;
    }
    auto exists = m_services.fs.exists(toolchain);
    if (!exists || !*exists) {
        fail("Manager finished but the expected toolchain was not generated; custom Conan layout requires manual "
             "toolchain selection.");
        return;
    }
    if (QMessageBox::question(parent, tr("Add Dependency CMake Preset"),
                              tr("Add %1 to CMakeUserPresets.json using toolchain %2? Configure it through the "
                                 "ordinary CMake profile selector.")
                                  .arg(q(profile.name), q(toolchain)),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    if (!alive)
        return;
    auto now = m_services.fs.readFile(path);
    if (!alive || root != m_projectRoot || m_shutDown || !m_services.trust.isTrusted(root) ||
        !((before && now && *before == *now) || (!before && !now && now.error().code() == ErrorCode::NotFound))) {
        fail("Project, trust or presets changed; reopen dependency setup.");
        return;
    }
    if (auto current = build::validateDependencyConsent(m_services.fs, m_services.trust, *consent); !current) {
        fail(current.error().message());
        return;
    }
    auto saved = m_services.fs.safeWrite(path, *updated, {});
    if (!saved) {
        fail(saved.error().message());
        return;
    }
    if (saved->warning)
        fail(saved->warning->message());
    io::FileChangeBatch batch;
    batch.changes.push_back({.kind = io::FileChangeKind::Modified, .path = path});
    m_cmake->service().filesChanged(batch);
}
} // namespace frost::app
