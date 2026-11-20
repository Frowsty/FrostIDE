// SPDX-License-Identifier: Apache-2.0
// Public contribution API v1 product adapter: explicit user commands and actual local DAP launch.
#include "extension_integration.hpp"

#include "frost/app/workbench.hpp"
#include "frost/debug/dap_client.hpp"
#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/lang/uri.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/shell/shell_window.hpp"

#include "language_registry_integration.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
namespace frost::app {
namespace {
QString q(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
bool matches(const lang::LanguageServerDefinition& d, std::string_view path) {
    const auto dot = path.find_last_of('.');
    if (dot == path.npos)
        return false;
    std::string ext(path.substr(dot));
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return std::ranges::find(d.extensions, ext) != d.extensions.end();
}
} // namespace
struct ExtensionIntegration::PreparedDebug {
    std::shared_ptr<lang::RegistryProcessLauncher> admission;
    std::shared_ptr<std::atomic<bool>> active;
    std::unique_ptr<io::IProcess> process;
    std::optional<Error> error;
    io::FileStatus status;
    std::string file, root, hash, adapterId, title;
    lang::Json arguments;
    std::uint64_t generation{}, version{};
};
namespace {
sched::Task<void> prepareExtensionDebug(std::function<void()> prepare) {
    prepare();
    co_return;
}
} // namespace
ExtensionIntegration::ExtensionIntegration(Workbench& workbench) : QObject(&workbench), m_workbench(workbench) {
    const QPointer<ExtensionIntegration> self(this);
    const auto add = [&](std::string id, std::string name, auto callback) {
        input::ActionSpec action;
        action.id = std::move(id);
        action.name = std::move(name);
        action.group = "Extensions";
        action.handler = [self, callback](const input::ActionContext&, const input::InvokeInfo&) {
            if (self)
                (self.data()->*callback)();
        };
        (void)m_workbench.m_registry.add(std::move(action));
    };
    add("extensions.configure", "Configure Extension Contributions", &ExtensionIntegration::configure);
    add("extensions.executeCommand", "Run Extension Language Command", &ExtensionIntegration::chooseCommand);
    add("extensions.debugCurrentFile", "Debug Current File With Extension Adapter", &ExtensionIntegration::chooseDebug);
    QObject::connect(&m_poll, &QTimer::timeout, this, [this] { poll(); });
    m_poll.start(250);
}
ExtensionIntegration::~ExtensionIntegration() { shutdown(); }
void ExtensionIntegration::revoke() {
    m_active->store(false);
    m_stop.request_stop();
    ++m_generation;
    if (m_debugGeneration && m_workbench.m_debug && m_workbench.m_debug->sessionGeneration() == m_debugGeneration)
        m_workbench.m_debug->stop();
    m_debugGeneration = 0;
    m_stop = std::stop_source{};
    m_active = std::make_shared<std::atomic<bool>>(true);
}
void ExtensionIntegration::shutdown() {
    if (m_stopped)
        return;
    m_stopped = true;
    m_poll.stop();
    revoke();
    for (auto& task : m_tasks)
        if (task.valid())
            task.join();
    m_tasks.clear();
}
void ExtensionIntegration::poll() {
    if (m_stopped || m_workbench.m_shutDown)
        return;
    std::erase_if(m_tasks, [](auto& task) {
        if (!task.ready())
            return false;
        task.join();
        return true;
    });
    const auto manifest = m_workbench.m_services.settings.snapshot()->get(settings::key::extensionCatalog);
    const auto root = m_workbench.m_projectRoot;
    const auto trust = m_workbench.m_services.trust.snapshot(root);
    auto physical = root.empty() ? Result<std::string>(std::string{}) : m_workbench.m_services.fs.physicalPath(root);
    const auto physicalKey = physical ? lang::documentKey(*physical) : std::string{};
    if (manifest == m_manifest && root == m_root && m_revision == trust.revision && physicalKey == m_physicalRoot)
        return;
    if (root != m_root || !trust.trusted)
        m_physicalIdentityRevoked = false;
    else if (!m_physicalRoot.empty() && physicalKey != m_physicalRoot)
        m_physicalIdentityRevoked = true;
    revoke();
    m_manifest = manifest;
    m_root = root;
    m_physicalRoot = physicalKey;
    m_revision = trust.revision;
    auto parsed = lang::ExtensionCatalog::parse(manifest);
    if (!parsed) {
        m_catalog.reset();
        m_workbench.notify(QStringLiteral("Extension catalog unavailable"), q(parsed.error().message()),
                           ui::NotificationKind::Warning);
        return;
    }
    m_catalog = std::move(*parsed);
}
Result<void> ExtensionIntegration::setCatalog(std::string manifest) {
    if (m_stopped || m_workbench.m_shutDown)
        return fail(ErrorCode::Cancelled, "Extension host is shutting down");
    auto parsed = lang::ExtensionCatalog::parse(manifest);
    if (!parsed)
        return std::unexpected(parsed.error());
    auto combined = parsed->composeLanguageRegistry(
        m_workbench.m_services.settings.snapshot()->get(settings::key::languageServerRegistry));
    if (!combined)
        return std::unexpected(combined.error());
    auto result = m_workbench.m_services.settings.set(settings::Scope::Application, settings::key::extensionCatalog,
                                                      std::move(manifest));
    if (!result)
        return result;
    poll();
    if (m_workbench.m_registeredLanguages)
        m_workbench.m_registeredLanguages->poll();
    return {};
}
Result<std::string> ExtensionIntegration::composeLanguageRegistry(std::string_view base) const {
    if (!m_catalog)
        return fail(ErrorCode::InvalidArgument, "Extension catalog is invalid or unavailable");
    return m_catalog->composeLanguageRegistry(base);
}
lang::Json ExtensionIntegration::snapshot() const {
    auto rows = lang::Json::array();
    if (m_catalog)
        for (const auto& entry : m_catalog->contributions())
            rows.push(lang::Json::object()
                          .set("id", entry.id)
                          .set("declaredVersion", entry.version)
                          .set("enabled", entry.enabled));
    return lang::Json::object().set("apiVersion", lang::kExtensionApiVersion).set("extensions", std::move(rows));
}
sched::Task<void> ExtensionIntegration::reportCommand(QPointer<ExtensionIntegration> self, std::uint64_t generation,
                                                      sched::Task<Result<lang::Json>> task) {
    auto result = co_await std::move(task);
    if (self)
        QMetaObject::invokeMethod(
            self.data(),
            [self, generation, result = std::move(result)] {
                if (!self || self->m_generation != generation || self->m_stopped)
                    return;
                self->m_workbench.notify(QStringLiteral("Extension language command"),
                                         q(result ? result->dump().substr(0, 4096) : result.error().message()),
                                         result ? ui::NotificationKind::Info : ui::NotificationKind::Warning);
            },
            Qt::QueuedConnection);
}
Result<void> ExtensionIntegration::executeCommand(std::string selected) {
    poll();
    if (!m_catalog || !m_workbench.m_registeredLanguages)
        return fail(ErrorCode::NotFound, "No extension language commands are configured");
    auto* view = m_workbench.currentView();
    if (!view)
        return fail(ErrorCode::NotFound, "Open a registered language document first");
    const auto file = view->path();
    for (const auto& entry : m_catalog->contributions())
        if (entry.enabled)
            for (const auto& command : entry.commands)
                if (entry.id + "/" + command.id == selected) {
                    auto arguments = lang::expandExtensionValues(command.arguments, file, m_workbench.m_projectRoot);
                    if (!arguments)
                        return std::unexpected(arguments.error());
                    auto task = m_workbench.m_registeredLanguages->executeCommand(
                        file, command.serverId, command.command, std::move(*arguments), m_stop.get_token());
                    m_tasks.push_back(m_workbench.m_pool->spawn(
                        sched::Lane::Background,
                        reportCommand(QPointer<ExtensionIntegration>(this), m_generation, std::move(task))));
                    return {};
                }
    return fail(ErrorCode::NotFound, "Extension command is disabled, removed or unknown");
}
Result<void> ExtensionIntegration::debugCurrentFile(std::string selected) {
    poll();
    if (!m_catalog || !m_workbench.m_debug)
        return fail(ErrorCode::NotFound, "No extension debug controller is available");
    auto* view = m_workbench.currentView();
    if (!view || m_root.empty())
        return fail(ErrorCode::NotFound, "Open a file in a trusted project before debugging");
    if (m_physicalIdentityRevoked)
        return fail(ErrorCode::PermissionDenied,
                    "Physical project changed; close and reopen the project before extension debug");
    const auto revision = m_revision;
    const auto physicalIdentity = m_physicalRoot;
    const auto file = view->path(), root = m_root;
    const QPointer<Workbench> owner(&m_workbench);
    const auto active = m_active;
    std::optional<lang::ExtensionDebugAdapter> chosen;
    for (const auto& entry : m_catalog->contributions())
        if (entry.enabled)
            for (const auto& adapter : entry.debugAdapters)
                if (entry.id + "/" + adapter.id == selected && adapter.tool.enabled && matches(adapter.tool, file))
                    chosen = adapter;
    if (!chosen)
        return fail(ErrorCode::NotFound, "No enabled extension adapter matches the current source file");
    if (view->document().dirty()) {
        auto saved = m_workbench.save(*view);
        if (!saved)
            return saved;
    }
    if (!owner || !active->load() || m_workbench.m_projectRoot != root || m_workbench.m_shutDown)
        return fail(ErrorCode::Cancelled, "Workspace changed while preparing extension debug");
    auto physical = m_workbench.m_services.fs.physicalPath(root);
    if (!physical)
        return std::unexpected(physical.error());
    if (lang::documentKey(*physical) != physicalIdentity ||
        m_workbench.m_services.trust.snapshot(root).revision != revision)
        return fail(ErrorCode::PermissionDenied, "Workspace authority changed during debug preparation");
    auto args = lang::expandExtensionValues(chosen->launch, file, root);
    if (!args)
        return std::unexpected(args.error());
    auto rawArguments = lang::Json::array();
    for (const auto& value : chosen->tool.arguments)
        rawArguments.push(value);
    auto adapterArgs = lang::expandExtensionValues(rawArguments, file, root);
    if (!adapterArgs)
        return std::unexpected(adapterArgs.error());
    auto tool = chosen->tool;
    tool.arguments.clear();
    for (const auto& value : adapterArgs->items())
        tool.arguments.push_back(*value.asString());
    auto admission = std::make_shared<lang::RegistryProcessLauncher>(
        m_workbench.m_services.launcher, m_workbench.m_services.fs, m_workbench.m_services.trust, tool, root, revision,
        *physical, active);
    if (auto allowed = admission->admitDocument(file); !allowed)
        return allowed;
    auto prepared = std::make_shared<PreparedDebug>();
    prepared->admission = std::move(admission);
    prepared->active = active;
    prepared->generation = m_generation;
    prepared->file = file;
    prepared->root = root;
    prepared->version = view->document().snapshot().version();
    prepared->arguments = std::move(*args);
    prepared->adapterId = chosen->adapterId;
    prepared->title = chosen->title;
    auto* fs = &m_workbench.m_services.fs;
    const QPointer<ExtensionIntegration> self(this);
    m_tasks.push_back(m_workbench.m_pool->spawn(
        sched::Lane::Background, prepareExtensionDebug([prepared, fs, tool, self] {
            const auto prepare = [&]() -> Result<void> {
                if (!prepared->active->load())
                    return fail(ErrorCode::Cancelled, "Extension debug preparation was revoked");
                auto before = fs->readFileSnapshot(prepared->file, 4 * 1024 * 1024);
                if (!before)
                    return std::unexpected(before.error());
                prepared->hash = io::sha256Hex(*before);
                auto status = fs->status(prepared->file);
                if (!status)
                    return std::unexpected(status.error());
                prepared->status = *status;
                io::ProcessSpec spec;
                spec.executable = tool.executable;
                spec.arguments = tool.arguments;
                spec.workingDirectory = prepared->root;
                spec.stdinMode = spec.stdoutMode = spec.stderrMode = io::Stdio::Pipe;
                spec.killOnClose = true;
                auto child = prepared->admission->launch(spec);
                if (!child)
                    return std::unexpected(child.error());
                prepared->process = std::move(*child);
                if (!prepared->active->load())
                    return fail(ErrorCode::Cancelled, "Extension debug preparation was revoked");
                return {};
            };
            if (auto result = prepare(); !result)
                prepared->error = result.error();
            if (self)
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, prepared] {
                        if (self)
                            self->adoptDebug(prepared);
                    },
                    Qt::QueuedConnection);
        })));
    return {};
}
void ExtensionIntegration::adoptDebug(std::shared_ptr<PreparedDebug> prepared) {
    poll();
    if (m_stopped || m_workbench.m_shutDown || prepared->generation != m_generation || !prepared->active->load())
        return;
    if (prepared->error) {
        m_workbench.notify(QStringLiteral("Extension debug refused"), q(prepared->error->message()),
                           ui::NotificationKind::Warning);
        return;
    }
    auto* view = m_workbench.currentView();
    if (!view || view->path() != prepared->file || view->document().dirty() ||
        view->document().snapshot().version() != prepared->version) {
        m_workbench.notify(QStringLiteral("Extension debug refused"),
                           QStringLiteral("The saved source or active document changed during preparation."),
                           ui::NotificationKind::Warning);
        return;
    }
    auto* fs = &m_workbench.m_services.fs;
    auto cheapAdmission = [prepared, fs]() -> Result<void> {
        if (!prepared->active->load())
            return fail(ErrorCode::Cancelled, "Extension contribution was revoked");
        if (auto allowed = prepared->admission->admitDocument(prepared->file); !allowed)
            return allowed;
        auto now = fs->status(prepared->file);
        if (!now || *now != prepared->status)
            return fail(ErrorCode::Busy, "Debug source metadata changed during preparation");
        return {};
    };
    auto factory = [prepared, fs](debug::DapHandlers handlers) -> Result<std::unique_ptr<debug::IDebugAdapterClient>> {
        if (!prepared->process)
            return fail(ErrorCode::Cancelled, "Prepared extension process is unavailable");
        auto prior = std::move(handlers.beforeWrite);
        handlers.beforeWrite = [prepared, fs, prior = std::move(prior)](std::string_view command) -> Result<void> {
            if (prior)
                if (auto allowed = prior(command); !allowed)
                    return allowed;
            if (command == "launch") {
                if (!prepared->active->load())
                    return fail(ErrorCode::Cancelled, "Extension contribution was revoked");
                if (auto allowed = prepared->admission->admitDocument(prepared->file); !allowed)
                    return allowed;
                // Runs on the DAP writer immediately before launch, never the GUI adoption path.
                auto now = fs->readFileSnapshot(prepared->file, 4 * 1024 * 1024);
                if (!now || io::sha256Hex(*now) != prepared->hash)
                    return fail(ErrorCode::Busy, "Debug source changed since the saved launch snapshot");
                if (!prepared->active->load())
                    return fail(ErrorCode::Cancelled, "Extension contribution was revoked");
            }
            return {};
        };
        return debug::makeDapClient(debug::processTransport(std::move(prepared->process)), std::move(handlers));
    };
    debug::DebugTarget target;
    target.name = prepared->title;
    target.program = prepared->file;
    target.workingDirectory = prepared->root;
    target.trustRoots = {prepared->root};
    auto started =
        m_workbench.m_debug->startAdapterTarget(target, std::move(factory), prepared->arguments, prepared->adapterId,
                                                debug::DebugTargetMode::Launch, false, std::move(cheapAdmission));
    if (started)
        m_debugGeneration = m_workbench.m_debug->sessionGeneration();
    else
        m_workbench.notify(QStringLiteral("Extension debug refused"), q(started.error().message()),
                           ui::NotificationKind::Warning);
}

void ExtensionIntegration::chooseCommand() {
    poll();
    QStringList items;
    if (m_catalog)
        for (const auto& entry : m_catalog->contributions())
            if (entry.enabled)
                for (const auto& command : entry.commands)
                    items.push_back(q(entry.id + "/" + command.id));
    if (items.empty()) {
        m_workbench.notify(
            QStringLiteral("Extension commands unavailable"),
            QStringLiteral("Configure an enabled command contribution and its actual language provider first."),
            ui::NotificationKind::Warning);
        return;
    }
    const QPointer<ExtensionIntegration> self(this);
    bool accepted = false;
    const auto selected = QInputDialog::getItem(m_workbench.m_window, QStringLiteral("Run extension language command"),
                                                QStringLiteral("Contribution"), items, 0, false, &accepted);
    if (!self)
        return;
    if (accepted)
        if (auto result = executeCommand(selected.toStdString()); !result)
            m_workbench.notify(QStringLiteral("Extension command refused"), q(result.error().message()),
                               ui::NotificationKind::Warning);
}
void ExtensionIntegration::chooseDebug() {
    poll();
    QStringList items;
    auto* view = m_workbench.currentView();
    if (m_catalog && view)
        for (const auto& entry : m_catalog->contributions())
            if (entry.enabled)
                for (const auto& adapter : entry.debugAdapters)
                    if (adapter.tool.enabled && matches(adapter.tool, view->path()))
                        items.push_back(q(entry.id + "/" + adapter.id));
    if (items.empty()) {
        m_workbench.notify(
            QStringLiteral("Debug adapter unavailable"),
            QStringLiteral(
                "Configure an enabled pinned adapter for this file's language. No debugger is inferred or downloaded."),
            ui::NotificationKind::Warning);
        return;
    }
    const QPointer<ExtensionIntegration> self(this);
    bool accepted = false;
    const auto selected =
        QInputDialog::getItem(m_workbench.m_window, QStringLiteral("Debug current file"),
                              QStringLiteral("Local adapter contribution"), items, 0, false, &accepted);
    if (!self)
        return;
    if (accepted)
        if (auto result = debugCurrentFile(selected.toStdString()); !result)
            m_workbench.notify(QStringLiteral("Extension debug refused"), q(result.error().message()),
                               ui::NotificationKind::Warning);
}
void ExtensionIntegration::configure() {
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Public extension contributions — API v1"));
    dialog.resize(880, 650);
    QObject::connect(this, &QObject::destroyed, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(
        QStringLiteral("Application-only public API v1 contributions: explicit capabilities for pinned local "
                       "languageServers, debugAdapters, and commands actually advertised by a language server. No "
                       "native DLL/script loading in the IDE, background install hooks or downloads."),
        &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* edit = new QPlainTextEdit(&dialog);
    edit->setPlainText(q(m_workbench.m_services.settings.snapshot()->get(settings::key::extensionCatalog)));
    layout->addWidget(edit);
    auto* load = new QPushButton(QStringLiteral("Import local contribution manifest…"), &dialog);
    layout->addWidget(load);
    const QPointer<ExtensionIntegration> self(this);
    QObject::connect(load, &QPushButton::clicked, &dialog, [self, &dialog, edit, label] {
        const auto file = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Extension contribution manifest"), {},
                                                       QStringLiteral("JSON (*.json)"));
        if (file.isEmpty() || !self)
            return;
        auto bytes = self->m_workbench.m_services.fs.readFileSnapshot(file.toStdString(), 1024 * 1024);
        if (bytes)
            edit->setPlainText(q(*bytes));
        else
            label->setText(q(bytes.error().message()));
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [self, edit, label, &dialog] {
        if (!self)
            return;
        auto result = self->setCatalog(edit->toPlainText().toUtf8().toStdString());
        if (result)
            dialog.accept();
        else
            label->setText(q(result.error().message()));
    });
    dialog.exec();
}
Result<void> Workbench::setExtensionCatalog(std::string manifest) {
    return m_extensions->setCatalog(std::move(manifest));
}
lang::Json Workbench::extensionCatalogState() const {
    return m_extensions ? m_extensions->snapshot() : lang::Json::object();
}
Result<void> Workbench::debugWithExtension(std::string id) { return m_extensions->debugCurrentFile(std::move(id)); }
Result<void> Workbench::executeExtensionCommand(std::string id) { return m_extensions->executeCommand(std::move(id)); }
} // namespace frost::app
