// SPDX-License-Identifier: Apache-2.0
// Local registry product entry, actual supervised server diagnostics and document lifetime.
#include "language_registry_integration.hpp"

#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/uri.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "language_bridge.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLabel>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
namespace frost::app {
struct LanguageRegistryIntegration::Entry {
    lang::LanguageServerDefinition definition;
    std::shared_ptr<lang::RegistryProcessLauncher> launcher;
    std::shared_ptr<lang::LuaService> service;
    std::shared_ptr<lang::RegistryLspRequester> requester;
    std::map<std::string, std::uint64_t> versions;
    std::uint64_t listener{};
    bool failureReported{};
    static sched::Task<void> stop(std::shared_ptr<Entry> entry) { co_await entry->service->stop(); }
};
namespace {
QString q(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
} // namespace
LanguageRegistryIntegration::LanguageRegistryIntegration(Workbench& w) : QObject(&w), m_workbench(w) {
    input::ActionSpec action;
    action.id = "languages.configureRegistry";
    action.name = "Configure Language Servers";
    action.group = "Languages";
    const QPointer<LanguageRegistryIntegration> self(this);
    action.handler = [self](const input::ActionContext&, const input::InvokeInfo&) {
        if (self)
            self->configure();
    };
    (void)w.m_registry.add(std::move(action));
    QObject::connect(&m_poll, &QTimer::timeout, this, [this] { poll(); });
    m_poll.start(250);
}
LanguageRegistryIntegration::~LanguageRegistryIntegration() { shutdown(); }
void LanguageRegistryIntegration::retire() {
    ++m_generation;
    m_workbench.m_bridge->setRegisteredServers({});
    for (auto& [id, entry] : m_entries) {
        entry->service->diagnostics().removeListener(entry->listener);
        m_retired.push_back(m_workbench.m_pool->spawn(sched::Lane::Background, Entry::stop(entry)));
    }
    for (auto* view : m_workbench.m_editors->views())
        if (m_registry && m_registry->forPath(view->path())) {
            delete view->editor().findChild<ui::lang::EditorInsightBinding*>();
            view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, {});
            view->editor().setInsightEnabled(false);
        }
    m_entries.clear();
    m_workbench.refreshLiveProblems();
}
void LanguageRegistryIntegration::shutdown() {
    if (m_stopped)
        return;
    m_stopped = true;
    m_poll.stop();
    retire();
    for (auto& task : m_retired)
        if (task.valid())
            task.join();
    m_retired.clear();
}
void LanguageRegistryIntegration::poll() {
    if (m_stopped || m_workbench.m_shutDown)
        return;
    std::erase_if(m_retired, [](auto& task) {
        if (!task.ready())
            return false;
        task.join();
        return true;
    });
    const auto root = m_workbench.m_projectRoot;
    const auto manifest = m_workbench.m_services.settings.snapshot()->get(settings::key::languageServerRegistry);
    const auto trust = m_workbench.m_services.trust.snapshot(root);
    const auto physicalNow =
        root.empty() ? Result<std::string>(std::string{}) : m_workbench.m_services.fs.physicalPath(root);
    const auto physicalKey = physicalNow ? lang::documentKey(*physicalNow) : std::string{};
    if (root != m_root || manifest != m_manifest || trust.revision != m_revision || physicalKey != m_physicalRoot) {
        if (root != m_root || trust.revision != m_revision)
            m_physicalIdentityRevoked = false;
        else if (!m_physicalRoot.empty() && physicalKey != m_physicalRoot) {
            m_physicalIdentityRevoked = true;
            m_workbench.notify(QStringLiteral("Language server workspace changed"),
                               QStringLiteral("The physical project changed. Revoke and grant project trust before "
                                              "restarting registered servers."),
                               ui::NotificationKind::Warning);
        }
        retire();
        m_registry.reset();
        m_root = root;
        m_manifest = manifest;
        m_revision = trust.revision;
        m_physicalRoot = physicalKey;
        auto registry = lang::LanguageServerRegistry::parse(manifest);
        if (!registry) {
            m_workbench.notify(QStringLiteral("Language server registry unavailable"), q(registry.error().message()),
                               ui::NotificationKind::Warning);
            return;
        }
        m_registry = std::move(*registry);
    }
    if (!m_registry || root.empty() || !trust.trusted || m_physicalIdentityRevoked)
        return;
    auto physical = m_workbench.m_services.fs.physicalPath(root);
    if (!physical)
        return;
    const auto generation = m_generation;
    const QPointer<LanguageRegistryIntegration> self(this);
    bool changed = false;
    for (const auto& d : m_registry->definitions()) {
        if (!d.enabled || m_entries.contains(d.id))
            continue;
        const bool interested = std::ranges::any_of(m_workbench.m_editors->views(), [&](auto* view) {
            const auto* selected = m_registry->forPath(view->path());
            if (!selected || selected->id != d.id)
                return false;
            auto path = m_workbench.m_services.fs.physicalPath(view->path());
            auto prefix = m_physicalRoot;
            if (!prefix.ends_with('/') && !prefix.ends_with('\\'))
                prefix += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
            return path && lang::documentKey(*path).starts_with(prefix);
        });
        if (!interested)
            continue;
        auto entry = std::make_shared<Entry>();
        entry->definition = d;
        entry->launcher = std::make_shared<lang::RegistryProcessLauncher>(
            m_workbench.m_services.launcher, m_workbench.m_services.fs, m_workbench.m_services.trust, d, root,
            trust.revision, *physical);
        auto options = lang::registeredLanguageOptions(d, root);
        options.onLifecycle = [self, generation] {
            if (self)
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, generation] {
                        if (self && self->m_generation == generation)
                            self->publish();
                    },
                    Qt::QueuedConnection);
        };
        entry->service = std::make_shared<lang::LuaService>(
            *entry->launcher, io::AsyncContext{*m_workbench.m_reactor, *m_workbench.m_pool}, std::move(options));
        entry->requester = std::make_shared<lang::RegistryLspRequester>(
            entry->service, entry->launcher, [self](std::function<void()> apply) {
                return self && QMetaObject::invokeMethod(
                                   self.data(),
                                   [self, apply = std::move(apply)] {
                                       if (self)
                                           apply();
                                   },
                                   Qt::QueuedConnection);
            });
        entry->listener = entry->service->diagnostics().addListener([self, generation](const lang::FileDiagnostics&) {
            if (self)
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, generation] {
                        if (self && self->m_generation == generation)
                            self->publish();
                    },
                    Qt::QueuedConnection);
        });
        if (auto started = entry->service->start(); !started) {
            m_workbench.notify(q(d.name), q(started.error().message()), ui::NotificationKind::Warning);
            continue;
        }
        m_entries.emplace(d.id, std::move(entry));
        changed = true;
    }
    std::map<std::string, std::shared_ptr<lang::ILspRequester>> routes;
    for (auto* view : m_workbench.m_editors->uniqueDocumentViews())
        if (auto server = service(view->path())) {
            const auto* definition = m_registry->forPath(view->path());
            routes.emplace(lang::documentKey(view->path()), m_entries.at(definition->id)->requester);
        }
    m_workbench.m_bridge->setRegisteredServers(std::move(routes));
    for (auto* view : m_workbench.m_editors->uniqueDocumentViews())
        sync(*view);
    const auto active = m_workbench.currentView();
    const std::string activePath = active ? active->path() : std::string{};
    if (changed || activePath != m_activePath) {
        m_activePath = activePath;
        publish();
    }
}
std::shared_ptr<lang::LuaService> LanguageRegistryIntegration::service(std::string_view path) const {
    if (!m_registry)
        return {};
    const auto* d = m_registry->forPath(path);
    if (!d)
        return {};
    auto found = m_entries.find(d->id);
    if (found == m_entries.end() || !found->second->launcher->admitDocument(path))
        return {};
    return found->second->service;
}
void LanguageRegistryIntegration::sync(DocumentEditorView& view) {
    auto server = service(view.path());
    if (!server || m_root.empty())
        return;
    auto physical = m_workbench.m_services.fs.physicalPath(view.path());
    auto root = m_workbench.m_services.fs.physicalPath(m_root);
    if (!physical || !root)
        return;
    if (lang::documentKey(*root) != m_physicalRoot)
        return;
    auto prefix = m_physicalRoot;
    if (!prefix.ends_with('/') && !prefix.ends_with('\\'))
        prefix += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
    if (!lang::documentKey(*physical).starts_with(prefix))
        return;
    const auto* definition = m_registry->forPath(view.path());
    auto& entry = *m_entries.at(definition->id);
    const auto snapshot = view.document().document().text();
    const auto key = lang::documentKey(view.path());
    if (const auto it = entry.versions.find(key); it != entry.versions.end() && it->second == snapshot.version())
        return;
    if (!server->openDocument(view.path(), snapshot)) {
        text::ChangeSet whole;
        whole.baseVersion = std::numeric_limits<std::uint64_t>::max();
        whole.version = snapshot.version();
        if (!server->changeDocument(view.path(), whole, snapshot))
            return;
    }
    entry.versions[key] = snapshot.version();
    view.editor().setInsightEnabled(server->state() == lang::LuaState::Running);
}
void LanguageRegistryIntegration::close(std::string_view path) {
    auto server = service(path);
    if (!server)
        return;
    (void)server->closeDocument(path);
    const auto* d = m_registry->forPath(path);
    m_entries.at(d->id)->versions.erase(lang::documentKey(path));
}
void LanguageRegistryIntegration::saved(std::string_view path) {
    if (auto server = service(path))
        (void)server->saveDocument(path);
}
void LanguageRegistryIntegration::publish() {
    if (m_stopped || m_workbench.m_shutDown)
        return;
    m_workbench.updateLanguageTarget();
    for (auto& [id, e] : m_entries) {
        if (e->service->state() == lang::LuaState::Failed && !e->failureReported) {
            e->failureReported = true;
            if (auto error = e->service->lastError())
                m_workbench.notify(q(e->definition.name + " unavailable"), q(error->message()),
                                   ui::NotificationKind::Warning);
        }
    }
    for (auto* view : m_workbench.m_editors->views()) {
        auto server = service(view->path());
        if (!server)
            continue;
        sync(*view);
        auto* binding = view->editor().findChild<ui::lang::EditorInsightBinding*>();
        if (server->state() != lang::LuaState::Running) {
            delete binding;
            binding = nullptr;
            view->editor().setInsightEnabled(false);
            view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, {});
            continue;
        }
        view->editor().setInsightEnabled(true);
        if (view != m_workbench.currentView() && binding) {
            delete binding;
            binding = nullptr;
        }
        if (view == m_workbench.currentView() && !binding && server->state() == lang::LuaState::Running) {
            binding = new ui::lang::EditorInsightBinding(view->editor(), {*m_workbench.m_bridge, *m_workbench.m_pool});
            binding->refreshSemanticTokens();
        }
        auto set = server->diagnostics().forFile(view->path());
        if (set && !set->stale) {
            if (binding)
                binding->setLiveDiagnostics(*set);
            else
                view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, ui::lang::toEditorDiagnostics(*set));
        } else {
            view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, {});
            if (binding) {
                lang::FileDiagnostics empty;
                empty.file = view->path();
                binding->setLiveDiagnostics(empty);
            }
        }
    }
    m_workbench.refreshLiveProblems();
}
std::vector<lang::LiveDiagnostic> LanguageRegistryIntegration::problems() const {
    std::vector<lang::LiveDiagnostic> out;
    for (const auto& [id, e] : m_entries) {
        if (!e->launcher->admitWorkspace())
            continue;
        auto rows = e->service->diagnostics().problems();
        out.insert(out.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
    }
    return out;
}
lang::Json LanguageRegistryIntegration::snapshot() const {
    auto rows = lang::Json::array();
    for (const auto& [id, e] : m_entries) {
        auto row = lang::Json::object()
                       .set("id", id)
                       .set("declaredVersion", e->definition.version)
                       .set("state", static_cast<int>(e->service->state()));
        if (auto error = e->service->lastError())
            row.set("error", error->message());
        if (auto caps = e->service->capabilities())
            row.set("serverName", caps->serverName).set("serverVersion", caps->serverVersion);
        rows.push(std::move(row));
    }
    return lang::Json::object()
        .set("servers", std::move(rows))
        .set("physicalIdentityRevoked", m_physicalIdentityRevoked);
}
Result<void> LanguageRegistryIntegration::setManifest(std::string manifest) {
    if (auto parsed = lang::LanguageServerRegistry::parse(manifest); !parsed)
        return std::unexpected(parsed.error());
    auto saved = m_workbench.m_services.settings.set(settings::Scope::Application,
                                                     settings::key::languageServerRegistry, std::move(manifest));
    if (!saved)
        return saved;
    poll();
    return {};
}
Result<void> Workbench::setLanguageServerRegistry(std::string manifest) {
    return m_registeredLanguages->setManifest(std::move(manifest));
}
lang::Json Workbench::languageServerRegistryState() const {
    return m_registeredLanguages ? m_registeredLanguages->snapshot() : lang::Json::object();
}
void LanguageRegistryIntegration::configure() {
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Language servers — local pinned registry"));
    dialog.resize(880, 650);
    QObject::connect(this, &QObject::destroyed, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(
        QStringLiteral("Import a version 1 registry for locally installed stdio servers. Each server needs id, name, "
                       "languageId, version, extensions, executable, arguments, enabled, SHA256 artifacts and pinned "
                       "licensePath/licenseSha256 evidence with an approved license. Project settings cannot add "
                       "servers. Trust is required at every launch. No packages are downloaded."),
        &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* edit = new QPlainTextEdit(&dialog);
    edit->setPlainText(q(m_workbench.m_services.settings.snapshot()->get(settings::key::languageServerRegistry)));
    layout->addWidget(edit);
    auto* load = new QPushButton(QStringLiteral("Import local manifest…"), &dialog);
    layout->addWidget(load);
    const QPointer<LanguageRegistryIntegration> alive(this);
    QObject::connect(load, &QPushButton::clicked, &dialog, [&, edit, alive] {
        auto file = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Language server registry"), {},
                                                 QStringLiteral("JSON (*.json)"));
        if (file.isEmpty() || !alive)
            return;
        auto bytes = alive->m_workbench.m_services.fs.readFileSnapshot(file.toStdString(), 1024 * 1024);
        if (bytes)
            edit->setPlainText(q(*bytes));
        else
            label->setText(q(bytes.error().message()));
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    const QPointer<LanguageRegistryIntegration> self(this);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [self, edit, label, &dialog] {
        if (!self)
            return;
        auto result = self->setManifest(edit->toPlainText().toUtf8().toStdString());
        if (result)
            dialog.accept();
        else
            label->setText(q(result.error().message()));
    });
    dialog.exec();
}
} // namespace frost::app
