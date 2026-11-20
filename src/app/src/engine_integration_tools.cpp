// SPDX-License-Identifier: Apache-2.0
// FER-13/14 and FEC-11: current authenticated commands, real measured reports and actual macro sites.
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/ui/nav/project_files.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/engine_view.hpp"

#include "engine_integration_internal.hpp"
#include <QCheckBox>
#include <QCryptographicHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <format>
#include <tuple>
namespace frost::app {
namespace {
QString qs(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
void measuredRows(QTreeWidgetItem& parent, const lang::Json& value, std::size_t& budget, unsigned depth = 0) {
    if (!budget || depth > 16)
        return;
    if (!value.isObject() && !value.isArray()) {
        parent.setText(1, value.isNull() ? QStringLiteral("Unavailable") : qs(value.dump()));
        return;
    }
    for (std::size_t i = 0; i < value.size() && budget; ++i) {
        --budget;
        auto* row = new QTreeWidgetItem(&parent);
        row->setText(0, value.isObject() ? qs(value.keys()[i]) : QString::number(static_cast<qulonglong>(i)));
        measuredRows(*row, value.items()[i], budget, depth + 1);
    }
}
} // namespace
bool EngineIntegration::isListedAsset(std::string_view path) const {
    const auto& impl = *m_impl;
    if (!impl.identity || !impl.session || !impl.session->connected() || !impl.assetIndex ||
        !impl.admitted(impl.epoch, impl.root, impl.trustRevision))
        return false;
    auto prefix = lang::documentKey(impl.assetIndex->content().directory);
    if (!prefix.ends_with('/') && !prefix.ends_with('\\'))
        prefix += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
    const auto key = lang::documentKey(path);
    return key.starts_with(prefix) && impl.assetIndex->find(key.substr(prefix.size()));
}
void EngineIntegration::Impl::refreshSessionContext() {
    for (auto* view : workbench.m_editors->views())
        view->editor().setProperty("engineAssetContext", owner.isListedAsset(view->path()));
}
void EngineIntegration::Impl::clearPerformanceLenses() {
    performanceSources.clear();
    performanceSourceDigests.clear();
    for (auto* view : workbench.m_editors->views())
        view->editor().setCodeVisionLensesForProvider("engine.performance", {});
}
void EngineIntegration::invalidatePerformanceLenses() {
    ++m_impl->performanceRevision;
    m_impl->performancePending = false;
    m_impl->clearPerformanceLenses();
}
void EngineIntegration::Impl::clearSessionTools() {
    ++performanceRevision;
    performancePending = false;
    performanceSample.reset();
    clearPerformanceLenses();
    if (performanceTimer)
        performanceTimer->stop();
    if (performanceAutomatic)
        performanceAutomatic->setChecked(false);
    if (performanceTree)
        performanceTree->clear();
    for (auto* view : workbench.m_editors->views())
        view->editor().setProperty("engineAssetContext", false);
    if (performanceStatus)
        performanceStatus->setText(QStringLiteral("Pair a current trusted engine to request measured statistics"));
}
Result<void> EngineIntegration::showInFrostEditor(std::string path, bool openAsset) {
    auto& impl = *m_impl;
    if (!impl.assetIndex)
        return fail(ErrorCode::NotFound, "Refresh the actual engine asset listing first");
    const engine::ProjectAsset* asset = impl.assetIndex->find(path);
    if (!asset && io::isAbsolutePath(path)) {
        auto prefix = lang::documentKey(impl.assetIndex->content().directory);
        if (!prefix.ends_with('/') && !prefix.ends_with('\\'))
            prefix += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
        const auto key = lang::documentKey(path);
        if (key.starts_with(prefix))
            asset = impl.assetIndex->find(key.substr(prefix.size()));
    }
    if (!asset)
        return fail(ErrorCode::NotFound, "The current selection is not an engine-listed asset");
    const auto handle = std::to_string(asset->handle.value);
    auto params = lang::Json::object();
    if (openAsset)
        params.set("path", handle); // discardChanges is deliberately never requested.
    else
        params.set("select", lang::Json::array().push(handle)).set("refresh", true).set("search", "");
    auto sent = contentCommand(openAsset ? "editor.open-asset" : "editor.browse-assets", std::move(params), {});
    if (!sent)
        return std::unexpected(sent.error());
    return {};
}
Result<void> EngineIntegration::refreshPerformance() {
    auto& impl = *m_impl;
    if (impl.performancePending)
        return fail(ErrorCode::Busy, "A measured statistics request is already pending");
    if (!impl.identity)
        return fail(ErrorCode::Busy, "Pair an engine before requesting statistics");
    const auto revision = ++impl.performanceRevision;
    auto identity = *impl.identity;
    identity.schema = {};
    identity.availableCommands.clear();
    impl.performancePending = true;
    const QPointer<EngineIntegration> self(this);
    auto started = contentCommand(
        "stats.get",
        lang::Json::object()
            .set("profilerScopes", 128)
            .set("sections",
                 lang::Json::array({"frame", "rendering", "features", "passes", "budgets", "memory", "profiler"})),
        [self, revision, identity](Result<lang::Json> report) {
            if (!self || self->m_impl->performanceRevision != revision)
                return;
            auto& current = *self->m_impl;
            current.performancePending = false;
            if (!report) {
                if (current.performanceStatus)
                    current.performanceStatus->setText(qs(report.error().message()));
                return;
            }
            auto parsed = engine::parseEnginePerformance(identity, *report);
            if (!parsed) {
                if (current.performanceStatus)
                    current.performanceStatus->setText(qs(parsed.error().message()));
                return;
            }
            current.clearPerformanceLenses();
            current.performanceSample = std::move(*parsed);
            if (current.performanceTree) {
                current.performanceTree->clear();
                auto* root = new QTreeWidgetItem(current.performanceTree);
                root->setText(0, QStringLiteral("Actual stats.get snapshot"));
                std::size_t budget = 12000;
                measuredRows(*root, current.performanceSample->measured, budget);
                current.performanceTree->expandToDepth(1);
            }
            if (current.performanceStatus)
                current.performanceStatus->setText(
                    current.performanceSample->sourceProvenanceAvailable
                        ? QStringLiteral(
                              "Measured snapshot; inline metrics require matching build-source SHA256 receipts")
                        : QStringLiteral("Measured snapshot; this engine has no source-site profiler contract, so "
                                         "source lenses are unavailable"));
            current.capturePerformanceSources(revision);
        });
    if (!started) {
        impl.performancePending = false;
        return std::unexpected(started.error());
    }
    return {};
}
void EngineIntegration::Impl::capturePerformanceSources(std::uint64_t revision) {
    if (!performanceSample || !performanceSample->sourceProvenanceAvailable)
        return;
    auto scopes = performanceSample->sourceScopes;
    auto expectedIdentity = performanceSample->identity;
    const auto expectedEpoch = epoch;
    const auto expectedRoot = root;
    const auto trustRevisionCapture = trustRevision;
    auto* fs = &workbench.m_services.fs;
    auto* trust = &workbench.m_services.trust;
    const QPointer<EngineIntegration> self(&owner);
    background([self, scopes = std::move(scopes), expectedIdentity, expectedEpoch, expectedRoot, trustRevisionCapture,
                fs, trust, revision] {
        std::map<std::string, text::Snapshot, std::less<>> sources;
        std::map<std::string, std::string, std::less<>> digests;
        std::size_t total = 0;
        for (const auto& scope : scopes) {
            const auto key = lang::documentKey(scope.file);
            if (scope.sourceSha256.empty() || sources.contains(key) || !trust->isTrusted(scope.file))
                continue;
            const auto physical = fs->physicalPath(scope.file);
            if (!physical || lang::documentKey(*physical) != key)
                continue;
            auto bytes = fs->readFileSnapshot(scope.file, 2U * 1024U * 1024U);
            if (!bytes || bytes->size() > 16U * 1024U * 1024U - total)
                continue;
            const auto receipt = fs->physicalPath(scope.file);
            if (!receipt || lang::documentKey(*receipt) != key)
                continue;
            if (!engine::matchesEnginePerformanceSource(scope, *bytes))
                continue;
            const auto digest = scope.sourceSha256;
            total += bytes->size();
            auto buffer = text::TextBuffer::fromUtf8(std::move(*bytes));
            if (buffer) {
                sources.emplace(key, buffer->snapshot());
                digests.emplace(key, digest);
            }
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, sources = std::move(sources), digests = std::move(digests), expectedIdentity, expectedEpoch,
             expectedRoot, trustRevisionCapture, revision]() mutable {
                if (!self)
                    return;
                auto& current = *self->m_impl;
                if (current.performanceRevision != revision ||
                    !current.admitted(expectedEpoch, expectedRoot, trustRevisionCapture) ||
                    !current.currentIdentity(expectedIdentity))
                    return;
                current.performanceSources = std::move(sources);
                current.performanceSourceDigests = std::move(digests);
                current.publishPerformanceLenses();
            },
            Qt::QueuedConnection);
    });
}
void EngineIntegration::Impl::publishPerformanceLenses() {
    if (!performanceSample || !identity || !currentIdentity(performanceSample->identity))
        return;
    for (auto* view : workbench.m_editors->views()) {
        std::vector<ui::EditorCodeVisionLens> lenses;
        const auto key = lang::documentKey(view->path());
        const auto source = performanceSources.find(key);
        if (source != performanceSources.end() && source->second.text() == view->editor().editor().text().text()) {
            for (const auto& scope : performanceSample->sourceScopes) {
                const auto digest = performanceSourceDigests.find(key);
                if (digest == performanceSourceDigests.end() || scope.sourceSha256 != digest->second ||
                    !workbench.m_services.trust.isTrusted(scope.file) || lang::documentKey(scope.file) != key ||
                    !scope.line || scope.line > source->second.lineCount())
                    continue;
                const auto line = LineIndex{scope.line - 1};
                auto offset = source->second.offsetAt(text::Point{line, ByteColumn{0}});
                if (!offset)
                    continue;
                lenses.push_back(
                    {line, *offset,
                     std::format("{:.3f} ms/frame; {:.2f} calls/frame", scope.msPerFrame, scope.callsPerFrame),
                     "engine.performance",
                     "Measured compiled instrumentation site: " + scope.function +
                         ". Recorded profiler window; source SHA256 matches the build receipt."});
            }
        }
        view->editor().setCodeVisionLensesForProvider("engine.performance", std::move(lenses));
    }
}
void EngineIntegration::Impl::attachSessionTools() {
    auto* tools = new QWidget(workbench.m_window);
    tools->hide();
    performancePanel = tools;
    auto* layout = new QVBoxLayout(tools);
    auto* row = new QHBoxLayout;
    auto* refresh = new QPushButton(QStringLiteral("Request measured statistics"), tools);
    refresh->setObjectName(QStringLiteral("enginePerformanceRefresh"));
    row->addWidget(refresh);
    auto* automatic = new QCheckBox(QStringLiteral("Refresh every 2 seconds"), tools);
    performanceAutomatic = automatic;
    row->addWidget(automatic);
    layout->addLayout(row);
    performanceStatus = new QLabel(QStringLiteral("No measurement requested"), tools);
    performanceStatus->setWordWrap(true);
    performanceStatus->setTextFormat(Qt::PlainText);
    layout->addWidget(performanceStatus);
    performanceTree = new QTreeWidget(tools);
    performanceTree->setObjectName(QStringLiteral("enginePerformanceTree"));
    performanceTree->setColumnCount(2);
    performanceTree->setHeaderLabels({QStringLiteral("Measured field"), QStringLiteral("Value")});
    layout->addWidget(performanceTree);
    performanceTimer = new QTimer(&owner);
    performanceTimer->setInterval(2000);
    const QPointer<EngineIntegration> self(&owner);
    const auto request = [self] {
        if (self)
            if (auto result = self->refreshPerformance(); !result)
                self->m_impl->performanceStatus->setText(qs(result.error().message()));
    };
    QObject::connect(refresh, &QPushButton::clicked, &owner, request);
    QObject::connect(performanceTimer, &QTimer::timeout, &owner, request);
    QObject::connect(automatic, &QCheckBox::toggled, &owner, [this](bool enabled) {
        if (enabled)
            performanceTimer->start();
        else
            performanceTimer->stop();
    });
    ui::ToolWindowSpec spec;
    spec.id = std::string(kEnginePerformanceWindow);
    spec.title = QStringLiteral("Engine Performance");
    spec.side = ui::ToolWindowSide::Bottom;
    spec.createContent = [tools = QPointer<QWidget>(tools)] { return tools.data(); };
    if (auto added = workbench.m_window->toolWindows().add(std::move(spec)); !added)
        log::warn("engine", "Cannot register performance view: {}", added.error().describe());
    if (auto* mainLayout = qobject_cast<QVBoxLayout*>(panel->layout())) {
        auto* open = new QPushButton(QStringLiteral("Engine Performance..."), panel);
        mainLayout->addWidget(open);
        QObject::connect(open, &QPushButton::clicked, &owner,
                         [this] { workbench.m_window->toolWindows().show(kEnginePerformanceWindow); });
    }
    for (const auto& [id, title, open] : {std::tuple{"engine.showInEditor", "Show Asset in FrostEditor", false},
                                          {"engine.openInEditor", "Open Asset in FrostEditor", true}}) {
        input::ActionSpec action;
        action.id = id;
        action.name = title;
        action.group = "FrostEngine";
        action.handler = [self, open](const input::ActionContext&, const input::InvokeInfo&) {
            if (!self || !self->m_impl->workbench.currentView())
                return;
            auto result = self->showInFrostEditor(self->m_impl->workbench.currentView()->path(), open);
            if (!result)
                self->m_impl->setStatus(qs(result.error().message()));
        };
        action.enabled = [self](const input::ActionContext&) {
            return self && self->m_impl->identity && self->m_impl->assetIndex;
        };
        (void)workbench.m_registry.add(std::move(action));
    }
    QObject::connect(projectView, &ui::EngineView::showInEditor, &owner,
                     [self](const QString& path, const QString& handle, bool open) {
                         if (!self || !self->m_impl->assetIndex)
                             return;
                         const auto* asset = self->m_impl->assetIndex->find(path.toStdString());
                         if (!asset || std::to_string(asset->handle.value) != handle.toStdString())
                             return;
                         if (auto result = self->showInFrostEditor(asset->path, open); !result)
                             self->m_impl->setStatus(qs(result.error().message()));
                     });
}
} // namespace frost::app
