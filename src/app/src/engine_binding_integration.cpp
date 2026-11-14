// SPDX-License-Identifier: Apache-2.0
// FEC-13 app actions/panel and captured worker lifetime; all engine/AST/LSP data comes from tools.
#include "engine_binding_integration.hpp"

#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/lua_host.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/nav/project_files.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "engine_binding_work.hpp"
#include "language_bridge.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <format>
#include <set>
#include <tuple>
#include <utility>

namespace frost::app {
namespace {
QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
std::string bytes(const QString& text) { return text.toUtf8().toStdString(); }
constexpr auto windowId = "engineBindings";
} // namespace

void EngineBindingPostRelay::revoke() {
    std::lock_guard lock(mutex);
    target = nullptr;
}
void EngineBindingPostRelay::post(std::function<void(EngineBindingIntegration&)> callback) {
    std::lock_guard lock(mutex);
    if (!target)
        return;
    const QPointer<EngineBindingIntegration> guarded(target);
    QMetaObject::invokeMethod(
        target,
        [guarded, callback = std::move(callback)] {
            if (guarded)
                callback(*guarded);
        },
        Qt::QueuedConnection);
}

struct EngineBindingIntegration::Impl {
    EngineBindingIntegration& owner;
    Workbench& workbench;
    std::uint64_t revision = 0;
    bool stopped = false, busy = false, fallback = false;
    std::stop_source stop;
    std::shared_ptr<std::atomic<bool>> active = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<EngineBindingRetirement> retirement;
    std::shared_ptr<EngineBindingPostRelay> relay = std::make_shared<EngineBindingPostRelay>();
    std::shared_ptr<binding_work::Graph> graph;
    struct AnalysisLease {
        std::shared_ptr<lang::ClangdService> provider;
        lang::AnalysisDocumentLease lease;
    };
    std::vector<AnalysisLease> analysisLeases;
    std::optional<binding_work::Capture> published;
    std::vector<engine::BindingUsage> usages;
    std::optional<engine::BindingSelectedRenamePreview> preview;
    lang::Json previewSummary;
    std::uint64_t registrySubscription = 0;
    QPointer<QWidget> panel;
    QPointer<QLabel> status;
    QPointer<QComboBox> bindings;
    QPointer<QLineEdit> newName;
    QPointer<QCheckBox> nativeRename;
    QPointer<QTreeWidget> results;
    QPointer<QPlainTextEdit> diff;
    std::string message;
    std::set<std::string, std::less<>> generatedPaths;

    Impl(EngineBindingIntegration& ownerValue, Workbench& workbenchValue, std::shared_ptr<EngineBindingRetirement> work,
         std::shared_ptr<EngineBindingPostRelay> delivery)
        : owner(ownerValue), workbench(workbenchValue),
          retirement(work ? std::move(work) : std::make_shared<EngineBindingRetirement>()) {
        if (delivery)
            relay = std::move(delivery);
        relay->target = &owner;
    }
    void setStatus(std::string text) {
        message = std::move(text);
        if (status)
            status->setText(qs(message));
    }
    void invalidate(bool discardGraph) {
        ++revision;
        active->store(false);
        stop.request_stop();
        for (const auto& lease : analysisLeases)
            if (auto released = lease.provider->releaseAnalysisDocument(lease.lease); !released)
                log::warn("bindings", "Cannot close native analysis document: {}", released.error().describe());
        analysisLeases.clear();
        busy = false;
        published.reset();
        usages.clear();
        preview.reset();
        previewSummary = lang::Json();
        if (results)
            results->clear();
        if (diff)
            diff->clear();
        if (discardGraph) {
            graph.reset();
            if (bindings) {
                QSignalBlocker block(bindings);
                bindings->clear();
            }
        }
    }
    Result<binding_work::Capture> capture(binding_work::Operation operation, bool standard, std::string name,
                                          bool native) {
        if (stopped || workbench.m_shutDown || workbench.m_projectRoot.empty() || !workbench.m_pool)
            return fail(ErrorCode::Busy, "Open a trusted engine workspace before querying bindings");
        const auto permission = workbench.m_services.trust.snapshot(workbench.m_projectRoot);
        if (!permission.trusted)
            return fail(ErrorCode::Untrusted, "Trust this workspace before executing semantic binding tools");
        if (!workbench.m_clangd || !workbench.m_lua || workbench.m_clangd->state() != lang::ClangdState::Running ||
            workbench.m_lua->state() != lang::LuaState::Running)
            return fail(ErrorCode::Unsupported, "Combined bindings require actual running clangd and LuaLS providers");
        const auto database = workbench.m_clangd->compileCommands();
        if (!database)
            return fail(ErrorCode::NotFound,
                        "The active clangd has no actual compilation database for AST registration analysis");
        binding_work::Capture value;
        value.fs = &workbench.m_services.fs;
        value.launcher = &workbench.m_services.launcher;
        value.scheduler = workbench.m_pool.get();
        value.trust = &workbench.m_services.trust;
        value.host = workbench.engineLuaHostProvider();
        value.root = workbench.m_projectRoot;
        value.trustRevision = permission.revision;
        value.database = *database;
        value.native = workbench.m_clangd;
        value.lua = workbench.m_lua;
        value.nativePid = value.native->serverPid().value_or(0);
        value.luaPid = value.lua->serverPid().value_or(0);
        if (value.nativePid == 0 || value.luaPid == 0)
            return fail(ErrorCode::Busy, "The actual language-server process identities are not ready");
        value.nativeRestarts = value.native->restarts();
        value.luaRestarts = value.lua->restarts();
        value.luaConfigurationChanges = value.lua->configurationChangesSent();
        value.nativeProvider = {
            engine::BindingLanguage::Native,
            std::format("clangd:{}:{}", reinterpret_cast<std::uintptr_t>(value.native.get()), value.nativePid),
            value.nativeRestarts + 1ULL, value.native->encoding()};
        value.luaProvider = {
            engine::BindingLanguage::Lua,
            std::format("luals:{}:{}", reinterpret_cast<std::uintptr_t>(value.lua.get()), value.luaPid),
            value.luaRestarts + 1ULL, value.lua->encoding()};
        value.operation = operation;
        value.newName = std::move(name);
        value.renameNative = native;
        value.standardRoute = standard;
        value.requestTag = bytes(QUuid::createUuid().toString(QUuid::WithoutBraces));
        value.tool = workbench.m_options.engineBindingToolPath;
        if (value.tool.empty())
            value.tool = bytes(QDir(QCoreApplication::applicationDirPath())
                                   .filePath(
#ifdef _WIN32
                                       QStringLiteral("frost-clang.exe")
#else
                                       QStringLiteral("frost-clang")
#endif
                                           ));
        value.luaStateDirectory = workbench.m_options.luaStateDirectory;
        if (value.luaStateDirectory.empty())
            if (auto trustFile = project::TrustStore::defaultStorePath())
                value.luaStateDirectory = io::joinPath(io::parentPath(*trustFile), "luals").value_or("");
        value.stateDirectory = bytes(QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                         .filePath(QStringLiteral("binding-analysis")));
        for (const auto* view : workbench.m_editors->uniqueDocumentViews()) {
            if (!isCFamilySource(view->path()) && !isLuaSource(view->path()))
                continue;
            value.openDocuments.push_back({view->path(), view->editor().editor().text(),
                                           isLuaSource(view->path()) ? value.lua->documentVersion(view->path())
                                                                     : value.native->documentVersion(view->path()),
                                           engine::BindingDocumentRole::UserSource});
        }
        if (workbench.m_nav)
            value.candidates = workbench.m_nav->projectFiles().candidates();
        if (const auto* view = workbench.currentView()) {
            value.activePath = view->path();
            value.caret = view->editor().editor().state().primary().head;
        }
        if (!standard && bindings)
            value.selectedLuaIdentity = bytes(bindings->currentData().toString());
        return value;
    }
    static sched::Task<void> execute(std::shared_ptr<EngineBindingPostRelay> relay, binding_work::Capture capture,
                                     std::shared_ptr<binding_work::Graph> reusable) {
        auto result = co_await binding_work::perform(capture, std::move(reusable));
        relay->post([capture, result = std::move(result)](EngineBindingIntegration& target) mutable {
            const QPointer<EngineBindingIntegration> self(&target);
            if (!self || self->m_impl->stopped || self->m_impl->revision != capture.revision)
                return;
            auto& impl = *self->m_impl;
            impl.busy = false;
            if (!capture.current() || impl.workbench.m_projectRoot != capture.root) {
                impl.invalidate(true);
                impl.setStatus("Binding inputs or providers changed; the stale result was discarded");
                return;
            }
            if (!result) {
                // A failed second stage owns no published result. Withdraw its capture and
                // exact temporary documents now, including raw provider refusal/cancellation.
                impl.invalidate(false);
                impl.setStatus(result.error().message());
                if (capture.standardRoute && result.error().code() == ErrorCode::NotFound) {
                    impl.fallback = true;
                    impl.workbench.m_language->findUsages();
                    impl.fallback = false;
                }
                return;
            }
            if (!result->nativeDocumentsToSynchronize.empty()) {
                auto next = capture;
                for (const auto& document : result->nativeDocumentsToSynchronize) {
                    if (!next.current()) {
                        impl.invalidate(true);
                        impl.setStatus("Binding capture changed before native document synchronization");
                        return;
                    }
                    auto acquired = next.native->acquireAnalysisDocument(document.path, document.snapshot);
                    if (!acquired) {
                        impl.invalidate(false);
                        impl.setStatus(acquired.error().message());
                        return;
                    }
                    impl.analysisLeases.push_back({next.native, *acquired});
                    next.analysisDocuments.push_back(std::move(*acquired));
                }
                if (!next.current()) {
                    impl.invalidate(true);
                    impl.setStatus("Binding capture changed during native document synchronization");
                    return;
                }
                impl.busy = true;
                impl.retirement->jobs.push_back(impl.workbench.m_pool->spawn(
                    sched::Lane::Background, execute(impl.relay, std::move(next), result->graph)));
                return;
            }
            impl.graph = result->graph;
            if (impl.graph)
                impl.generatedPaths.insert(lang::documentKey(impl.graph->metadataPath));
            if (impl.graph)
                for (const auto& document : impl.graph->documents)
                    if (document.role == engine::BindingDocumentRole::GeneratedEngineMetadata)
                        impl.generatedPaths.insert(lang::documentKey(document.path));
            impl.published = capture;
            if (impl.bindings) {
                const auto selected = impl.bindings->currentData();
                QSignalBlocker block(impl.bindings);
                impl.bindings->clear();
                for (const auto& binding : impl.graph->index->bindings())
                    if (binding.resolution == engine::BindingResolution::Resolved)
                        impl.bindings->addItem(qs(binding.lua.qualifiedName), qs(binding.lua.qualifiedName));
                impl.bindings->setCurrentIndex(impl.bindings->findData(selected));
            }
            if (result->ordinaryUsages) {
                impl.setStatus("No semantic engine binding association was reported for this symbol");
                if (capture.standardRoute) {
                    impl.fallback = true;
                    impl.workbench.m_language->findUsages();
                    impl.fallback = false;
                }
                return;
            }
            impl.usages = std::move(result->usages);
            impl.preview = std::move(result->preview);
            impl.previewSummary = std::move(result->previewSummary);
            impl.displayUsages(0);
            if (impl.preview && impl.diff)
                impl.diff->setPlainText(qs(result->previewDisplay));
            impl.setStatus(
                impl.preview ? "Read-only rename preview from actual AST and language-server edits"
                : capture.operation == binding_work::Operation::Refresh
                    ? "Semantic bindings captured from actual schema and AST inputs"
                    : std::format(
                          "{} provider-indexed C++/Lua locations{}; embedded Lua in native strings is unverified",
                          impl.usages.size(), impl.usages.size() > 10000 ? " (first 10000 displayed)" : ""));
        });
    }
    Result<void> start(binding_work::Operation operation, bool standard = false, std::string name = {},
                       bool native = false, std::optional<lang::LspPosition> forcedLuaPosition = {}) {
        auto value = capture(operation, standard, std::move(name), native);
        if (!value)
            return std::unexpected(value.error());
        if (forcedLuaPosition) {
            const auto document = std::ranges::find_if(value->openDocuments, [&](const auto& item) {
                return lang::documentKey(item.path) == lang::documentKey(value->activePath);
            });
            if (document == value->openDocuments.end())
                return fail(ErrorCode::NotFound, "The accepted Lua rename document is no longer open");
            auto offset = lang::fromLspPosition(document->snapshot, *forcedLuaPosition, value->luaProvider.encoding);
            if (!offset)
                return std::unexpected(offset.error());
            value->caret = *offset;
            value->selectedLuaIdentity.clear();
        }
        invalidate(false);
        stop = std::stop_source{};
        active = std::make_shared<std::atomic<bool>>(true);
        value->active = active;
        value->revision = revision;
        value->stop = stop.get_token();
        busy = true;
        setStatus("Reading actual engine schema, compilation context and semantic binding evidence...");
        std::erase_if(retirement->jobs, [](const auto& job) { return job.ready(); });
        retirement->jobs.push_back(
            workbench.m_pool->spawn(sched::Lane::Background, execute(relay, std::move(*value), graph)));
        workbench.m_window->toolWindows().show(windowId, false);
        return {};
    }
    struct RenameProbe {
        std::shared_ptr<lang::LuaService> lua;
        sched::IScheduler* scheduler = nullptr;
        io::IFileSystem* fs = nullptr;
        std::string root, path, newName, metadataPath, expectedMetadataPhysical;
        bool metadataRequired = false;
        lang::LspPosition position;
        text::Snapshot snapshot;
        std::function<void()> ordinary;
        std::function<bool()> current;
        std::stop_token stop;
        std::uint64_t revision = 0;
    };
    static sched::Task<void> probeRename(std::shared_ptr<EngineBindingPostRelay> relay, RenameProbe probe) {
        co_await probe.scheduler->schedule(sched::Lane::Background);
        auto params = lang::textDocumentPosition(probe.path, probe.position);
        if (!params || !probe.current())
            co_return;
        auto raw = co_await probe.lua->request("textDocument/definition", std::move(*params), probe.stop);
        co_await probe.scheduler->schedule(sched::Lane::Background);
        bool bound = false;
        std::string error;
        std::vector<std::string> protectedPaths;
        if (!raw)
            error = raw.error().message();
        else {
            auto admission = binding_work::admitRenameDefinitions(*probe.fs, *raw, probe.metadataPath,
                                                                  probe.expectedMetadataPhysical, probe.metadataRequired);
            if (!admission)
                error = admission.error().message();
            else {
                bound = admission->bound;
                if (bound)
                    protectedPaths.push_back(lang::documentKey(probe.metadataPath));
            }
        }
        relay->post([probe = std::move(probe), bound, error = std::move(error),
                     protectedPaths = std::move(protectedPaths)](EngineBindingIntegration& target) mutable {
            const QPointer<EngineBindingIntegration> self(&target);
            if (!self || self->m_impl->stopped || self->m_impl->revision != probe.revision)
                return;
            auto& impl = *self->m_impl;
            impl.busy = false;
            const auto* view = impl.workbench.currentView();
            if (!probe.current() || !view || lang::documentKey(view->path()) != lang::documentKey(probe.path) ||
                !view->editor().editor().text().sharesStorageWith(probe.snapshot) ||
                view->editor().editor().text().version() != probe.snapshot.version()) {
                impl.setStatus("The accepted Lua rename capture changed; no edits were applied");
                return;
            }
            if (!error.empty()) {
                impl.setStatus(std::move(error));
                return;
            }
            if (bound) {
                for (auto& path : protectedPaths)
                    impl.generatedPaths.insert(std::move(path));
                if (auto started =
                        impl.start(binding_work::Operation::Preview, false, probe.newName, false, probe.position);
                    !started)
                    impl.setStatus(started.error().message());
            } else {
                probe.ordinary();
            }
        });
    }
    bool interceptRename(std::string path, lang::LspPosition position, std::string name,
                         std::function<void()> ordinary) {
        if (stopped || !isLuaSource(path) || !workbench.m_lua || workbench.m_lua->state() != lang::LuaState::Running ||
            !workbench.currentView() || !ordinary)
            return false;
        const auto trust = workbench.m_services.trust.snapshot(workbench.m_projectRoot);
        if (!trust.trusted)
            return false;
        const auto lua = workbench.m_lua;
        const auto pid = lua->serverPid();
        const auto restarts = lua->restarts();
        const auto configuration = lua->configurationChangesSent();
        const auto version = lua->documentVersion(path);
        const auto root = workbench.m_projectRoot;
        invalidate(false);
        stop = std::stop_source{};
        active = std::make_shared<std::atomic<bool>>(true);
        const auto gate = active;
        const auto token = stop.get_token();
        auto* store = &workbench.m_services.trust;
        const auto current = [lua, pid, restarts, configuration, version, root, path, gate, token, store,
                              revision = trust.revision] {
            const auto permission = store->snapshot(root);
            return gate->load() && !token.stop_requested() && permission.trusted && permission.revision == revision &&
                   lua->state() == lang::LuaState::Running && lua->serverPid() == pid && lua->restarts() == restarts &&
                   lua->configurationChangesSent() == configuration && lua->documentVersion(path) == version;
        };
        std::string stateDirectory = workbench.m_options.luaStateDirectory;
        if (stateDirectory.empty())
            if (auto file = project::TrustStore::defaultStorePath())
                stateDirectory = io::joinPath(io::parentPath(*file), "luals").value_or("");
        auto metadataDirectory = lang::luaHostMetaDirectory(stateDirectory, "FrostEngine", root);
        if (!metadataDirectory) {
            setStatus(metadataDirectory.error().message());
            return true;
        }
        auto metadataFile = io::joinPath(*metadataDirectory, engine::kEngineLuaStubFileName);
        if (!metadataFile) {
            setStatus(metadataFile.error().message());
            return true;
        }
        // composeFolder publishes this exact library only after its generated files were written.
        // Snapshot the actual configuration independently of whether a binding Graph was queried.
        const bool metadataRequired = binding_work::expectsEngineMetadata(lua->configuration(), *metadataDirectory);
        std::string metadataReceipt;
        if (graph) {
            const auto at = graph->physicalReceipts.find(*metadataFile);
            if (at != graph->physicalReceipts.end())
                metadataReceipt = at->second;
        }
        RenameProbe probe{lua,
                          workbench.m_pool.get(),
                          &workbench.m_services.fs,
                          root,
                          path,
                          std::move(name),
                          *metadataFile,
                          std::move(metadataReceipt),
                          metadataRequired,
                          position,
                          workbench.currentView()->editor().editor().text(),
                          std::move(ordinary),
                          current,
                          token,
                          revision};
        busy = true;
        setStatus("Checking the actual LuaLS definition before rename...");
        std::erase_if(retirement->jobs, [](const auto& job) { return job.ready(); });
        retirement->jobs.push_back(
            workbench.m_pool->spawn(sched::Lane::Background, probeRename(relay, std::move(probe))));
        return true;
    }
    void displayUsages(std::size_t offset) {
        if (!results || !published || !published->current())
            return;
        const auto end = std::min({offset + 128, usages.size(), std::size_t{10000}});
        for (std::size_t i = offset; i < end; ++i) {
            const auto& usage = usages[i];
            const auto point = usage.capturedSnapshot.pointAt(usage.location.bytes.begin);
            auto* row = new QTreeWidgetItem(results);
            row->setText(0, usage.provider.language == engine::BindingLanguage::Native ? QStringLiteral("C++")
                                                                                       : QStringLiteral("Lua"));
            row->setText(1, qs(usage.location.path));
            row->setText(2, QString::number(point.line.value() + 1));
            row->setText(3, qs(usage.text));
            row->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<qulonglong>(i)));
        }
        if (end < usages.size() && end < 10000) {
            const QPointer<EngineBindingIntegration> self(&owner);
            const auto expected = revision;
            QTimer::singleShot(0, &owner, [self, expected, end] {
                if (self && self->m_impl->revision == expected)
                    self->m_impl->displayUsages(end);
            });
        }
    }
    static sched::Task<void> navigate(std::shared_ptr<EngineBindingPostRelay> relay, binding_work::Capture capture,
                                      std::shared_ptr<binding_work::Graph> graph, engine::BindingUsage usage) {
        co_await capture.scheduler->schedule(sched::Lane::Background);
        if (!graph->verify(capture))
            co_return;
        const auto point = usage.capturedSnapshot.pointAt(usage.location.bytes.begin);
        const auto* metadata = graph->generatedDocument();
        const bool generated = metadata && lang::documentKey(usage.location.path) == lang::documentKey(metadata->path);
        relay->post([capture, usage = std::move(usage), point, generated](EngineBindingIntegration& target) {
            const QPointer<EngineBindingIntegration> self(&target);
            if (!self || !capture.current() || self->m_impl->revision != capture.revision ||
                !self->m_impl->workbench.m_nav)
                return;
            auto& workbench = self->m_impl->workbench;
            if (auto opened = workbench.m_nav->openFile(usage.location.path,
                                                        ui::LinePosition{static_cast<int>(point.line.value()) + 1,
                                                                         static_cast<int>(point.column.value()) + 1});
                opened && generated)
                if (auto* view = workbench.currentView())
                    view->setForcedReadOnly(true);
        });
    }
};

EngineBindingIntegration::EngineBindingIntegration(Workbench& workbench,
                                                   std::shared_ptr<EngineBindingRetirement> retirement,
                                                   std::shared_ptr<EngineBindingPostRelay> relay)
    : QObject(&workbench), m_impl(std::make_unique<Impl>(*this, workbench, std::move(retirement), std::move(relay))) {
    auto& impl = *m_impl;
    auto* panel = new QWidget(workbench.m_window);
    panel->setObjectName(QStringLiteral("engineBindingsPanel"));
    impl.panel = panel;
    auto* layout = new QVBoxLayout(panel);
    auto* row = new QHBoxLayout;
    auto* refresh = new QPushButton(QStringLiteral("Refresh bindings"), panel);
    refresh->setObjectName(QStringLiteral("engineBindingsRefreshButton"));
    row->addWidget(refresh);
    impl.bindings = new QComboBox(panel);
    impl.bindings->setObjectName(QStringLiteral("engineBindingIdentity"));
    impl.bindings->setPlaceholderText(QStringLiteral("Select an exact Lua binding"));
    row->addWidget(impl.bindings, 1);
    auto* usages = new QPushButton(QStringLiteral("Find C++/Lua usages"), panel);
    usages->setObjectName(QStringLiteral("engineBindingUsagesButton"));
    row->addWidget(usages);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), panel);
    row->addWidget(cancel);
    layout->addLayout(row);
    auto* renameRow = new QHBoxLayout;
    impl.newName = new QLineEdit(panel);
    impl.newName->setObjectName(QStringLiteral("engineBindingNewName"));
    impl.newName->setPlaceholderText(QStringLiteral("New Lua name"));
    renameRow->addWidget(impl.newName);
    impl.nativeRename = new QCheckBox(QStringLiteral("Rename native declaration too"), panel);
    renameRow->addWidget(impl.nativeRename);
    auto* preview = new QPushButton(QStringLiteral("Preview rename (read only)"), panel);
    preview->setObjectName(QStringLiteral("engineBindingPreviewButton"));
    renameRow->addWidget(preview);
    layout->addLayout(renameRow);
    impl.status = new QLabel(QStringLiteral("Refresh to capture actual engine binding metadata"), panel);
    impl.status->setWordWrap(true);
    layout->addWidget(impl.status);
    impl.results = new QTreeWidget(panel);
    impl.results->setColumnCount(4);
    impl.results->setHeaderLabels(
        {QStringLiteral("Language"), QStringLiteral("File"), QStringLiteral("Line"), QStringLiteral("Captured text")});
    impl.results->setObjectName(QStringLiteral("engineBindingUsages"));
    layout->addWidget(impl.results, 1);
    impl.diff = new QPlainTextEdit(panel);
    impl.diff->setReadOnly(true);
    impl.diff->setObjectName(QStringLiteral("engineBindingRenamePreview"));
    layout->addWidget(impl.diff, 1);
    ui::ToolWindowSpec spec;
    spec.id = windowId;
    spec.title = QStringLiteral("Bindings");
    spec.side = ui::ToolWindowSide::Bottom;
    spec.createContent = [panel = QPointer<QWidget>(panel)] { return panel.data(); };
    if (auto added = workbench.m_window->toolWindows().add(std::move(spec)); !added)
        log::warn("bindings", "Cannot register Bindings window: {}", added.error().describe());
    connect(refresh, &QPushButton::clicked, this, [this] {
        if (auto result = this->refresh(); !result)
            m_impl->setStatus(result.error().message());
    });
    connect(usages, &QPushButton::clicked, this, [this] {
        if (auto result = findUsages(); !result)
            m_impl->setStatus(result.error().message());
    });
    connect(cancel, &QPushButton::clicked, this, [this] { this->cancel(); });
    connect(preview, &QPushButton::clicked, this, [this] {
        if (auto result = previewRename(bytes(m_impl->newName->text()), m_impl->nativeRename->isChecked()); !result)
            m_impl->setStatus(result.error().message());
    });
    connect(impl.bindings, &QComboBox::currentIndexChanged, this, [this] {
        if (m_impl->busy)
            m_impl->invalidate(false);
    });
    connect(impl.results, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* row, int) {
        const auto index = static_cast<std::size_t>(row->data(0, Qt::UserRole).toULongLong());
        if (!m_impl->published || !m_impl->graph || index >= m_impl->usages.size())
            return;
        m_impl->retirement->jobs.push_back(m_impl->workbench.m_pool->spawn(
            sched::Lane::Background,
            Impl::navigate(m_impl->relay, *m_impl->published, m_impl->graph, m_impl->usages[index])));
    });
    connect(&workbench, &Workbench::projectChanged, this, [this](const QString&) { cancel(); });
    impl.registrySubscription = workbench.m_documentRegistry->subscribe([this](const std::string& path) {
        if (isCFamilySource(path) || isLuaSource(path))
            cancel();
    });
    workbench.m_language->setCombinedBindingUsagesHandler([this] { return tryFindUsages(); });
    workbench.m_language->setCombinedBindingRenameHandler(
        [this](std::string path, lang::LspPosition position, std::string name, std::function<void()> ordinary) {
            return m_impl->interceptRename(std::move(path), position, std::move(name), std::move(ordinary));
        });
    for (const auto& [id, name, operation] : {std::tuple{"engine.bindings.refresh", "Refresh Engine Bindings", 0},
                                              {"navigate.bindingUsages", "Find C++/Lua Binding Usages", 1},
                                              {"refactor.bindingRenamePreview", "Preview C++/Lua Binding Rename", 2}}) {
        input::ActionSpec action;
        action.id = id;
        action.name = name;
        action.group = "FrostEngine";
        action.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
        const QPointer<Workbench> window(&workbench);
        action.handler = [window, operation](const input::ActionContext&, const input::InvokeInfo&) {
            if (!window || !window->m_engineBindings)
                return;
            auto& current = *window->m_engineBindings;
            auto result = operation == 0   ? current.refresh()
                          : operation == 1 ? current.findUsages()
                                           : current.previewRename(bytes(current.m_impl->newName->text()),
                                                                   current.m_impl->nativeRename->isChecked());
            if (!result)
                current.m_impl->setStatus(result.error().message());
        };
        action.enabled = [window](const input::ActionContext&) {
            return window && window->m_engineBindings && !window->m_engineBindings->m_impl->stopped &&
                   !window->m_projectRoot.empty();
        };
        if (auto added = workbench.m_registry.add(std::move(action));
            !added && added.error().code() != ErrorCode::AlreadyExists)
            log::warn("bindings", "Cannot register binding action: {}", added.error().describe());
    }
}

EngineBindingIntegration::~EngineBindingIntegration() {
    shutdown();
    m_impl->workbench.retireEngineBindingWork(takeRetirement());
}
void EngineBindingIntegration::cancel() {
    if (!m_impl->stopped) {
        m_impl->invalidate(true);
        m_impl->setStatus("Binding inputs changed; refresh before querying again");
    }
}
void EngineBindingIntegration::shutdown() {
    auto& impl = *m_impl;
    if (impl.stopped)
        return;
    impl.relay->revoke();
    impl.invalidate(true);
    impl.stopped = true;
    if (impl.workbench.m_language)
        impl.workbench.m_language->setCombinedBindingUsagesHandler({});
    if (impl.workbench.m_language)
        impl.workbench.m_language->setCombinedBindingRenameHandler({});
    if (impl.registrySubscription && impl.workbench.m_documentRegistry)
        impl.workbench.m_documentRegistry->unsubscribe(impl.registrySubscription);
}
std::shared_ptr<EngineBindingRetirement> EngineBindingIntegration::takeRetirement() {
    if (m_impl->retirement->jobs.empty())
        return {};
    return std::exchange(m_impl->retirement, std::make_shared<EngineBindingRetirement>());
}
Result<void> EngineBindingIntegration::refresh() { return m_impl->start(binding_work::Operation::Refresh); }
Result<void> EngineBindingIntegration::findUsages() { return m_impl->start(binding_work::Operation::Usages); }
Result<void> EngineBindingIntegration::previewRename(std::string name, bool native) {
    return m_impl->start(binding_work::Operation::Preview, false, std::move(name), native);
}
bool EngineBindingIntegration::tryFindUsages() {
    if (m_impl->fallback || m_impl->stopped || !m_impl->workbench.currentView() ||
        (!isCFamilySource(m_impl->workbench.currentView()->path()) &&
         !isLuaSource(m_impl->workbench.currentView()->path())))
        return false;
    auto result = m_impl->start(binding_work::Operation::Usages, true);
    if (!result)
        return false;
    return true;
}
lang::Json EngineBindingIntegration::snapshot() const {
    const auto& impl = *m_impl;
    const bool current = impl.published && impl.published->current();
    auto state = lang::Json::object()
                     .set("busy", impl.busy)
                     .set("available", current && impl.graph != nullptr)
                     .set("revision", impl.revision)
                     .set("message", impl.message)
                     .set("readOnly", true);
    auto bindings = lang::Json::array();
    auto usages = lang::Json::array();
    if (current && impl.graph) {
        state.set("graphRevision", impl.graph->index->revision());
        for (const auto& binding : impl.graph->index->bindings()) {
            auto usrs = lang::Json::array();
            for (const auto& mapping : binding.native)
                usrs.push(mapping.usr);
            bindings.push(lang::Json::object()
                              .set("identity", binding.lua.qualifiedName)
                              .set("resolution", binding.resolution == engine::BindingResolution::Resolved ? "resolved"
                                                 : binding.resolution == engine::BindingResolution::Ambiguous
                                                     ? "ambiguous"
                                                     : "unavailable")
                              .set("reason", binding.reason)
                              .set("nativeUsrs", std::move(usrs)));
        }
        for (const auto& usage : impl.usages)
            usages.push(lang::Json::object()
                            .set("language", usage.provider.language == engine::BindingLanguage::Native ? "cpp" : "lua")
                            .set("path", usage.location.path)
                            .set("begin", usage.location.bytes.begin.value())
                            .set("end", usage.location.bytes.end.value())
                            .set("text", usage.text));
    }
    state.set("bindings", std::move(bindings))
        .set("usages", std::move(usages))
        .set("previewReady", current && impl.preview.has_value());
    state.set("coverage", lang::Json::object()
                              .set("scope", "provider-indexed")
                              .set("embeddedLanguageReferencesVerified", false)
                              .set("applicationSafe", false));
    if (current && impl.preview)
        state.set("preview", impl.previewSummary);
    return state;
}
bool EngineBindingIntegration::protectsGeneratedResource(std::string_view path) const {
    return m_impl->generatedPaths.contains(lang::documentKey(path));
}
} // namespace frost::app
