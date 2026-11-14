// SPDX-License-Identifier: Apache-2.0
// Modules and settings reflect successful adapter responses; failures remain visible.
#include "frost/ui/debug_symbols_window.hpp"

#include "frost/ui/debug_controller.hpp"

#include <QCheckBox>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace frost::ui {
namespace {
QStringList lines(QPlainTextEdit* input) {
    QStringList result;
    for (const auto& line : input->toPlainText().split('\n'))
        if (!line.trimmed().isEmpty())
            result.push_back(line.trimmed());
    return result;
}
rpc::Json strings(const QStringList& values) {
    auto result = rpc::Json::array();
    for (const auto& value : values)
        result.push(value.toUtf8().toStdString());
    return result;
}
QString field(const rpc::Json& object, const char* key) {
    return QString::fromStdString(std::string(object.stringAt(key)));
}
} // namespace
DebugSymbolsWindow::DebugSymbolsWindow(DebugController& controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    auto* layout = new QVBoxLayout(this);
    m_status = new QLabel(tr("Modules and visualizers require adapter support and a stopped target."), this);
    m_status->setObjectName("debugSymbolsStatus");
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_natvisStatus = new QLabel(this);
    m_natvisStatus->setWordWrap(true);
    layout->addWidget(m_natvisStatus);
    m_modules = new QTreeWidget(this);
    m_modules->setObjectName("debugModules");
    m_modules->setHeaderLabels({tr("Module"), tr("Path"), tr("Address"), tr("Symbols"), tr("Symbol file")});
    layout->addWidget(m_modules);
    m_refresh = new QPushButton(tr("Refresh modules"), this);
    m_refresh->setObjectName("debugRefreshModules");
    layout->addWidget(m_refresh);
    m_reload = new QPushButton(tr("Reload selected module symbols"), this);
    m_reload->setObjectName("debugReloadSymbols");
    layout->addWidget(m_reload);
    layout->addWidget(new QLabel(tr("Symbol directories or srv*absolute-cache*https://server, one per line"), this));
    m_paths = new QPlainTextEdit(this);
    m_paths->setObjectName("debugSymbolPaths");
    m_paths->setMaximumHeight(80);
    layout->addWidget(m_paths);
    m_allowServer = new QCheckBox(tr("Allow symbol server network access for this session"), this);
    layout->addWidget(m_allowServer);
    m_apply = new QPushButton(tr("Apply symbol paths"), this);
    layout->addWidget(m_apply);
    m_sourceMap = new QPlainTextEdit(this);
    m_sourceMap->setObjectName("debugSourceMap");
    m_sourceMap->setPlaceholderText(tr("Absolute build root => absolute local root, one mapping per line"));
    m_sourceMap->setMaximumHeight(60);
    layout->addWidget(m_sourceMap);
    m_applySourceMap = new QPushButton(tr("Apply source mappings"), this);
    layout->addWidget(m_applySourceMap);
    layout->addWidget(new QLabel(tr("Absolute Natvis files, one per line (saved changes reload when stopped)"), this));
    m_visualizers = new QPlainTextEdit(this);
    m_visualizers->setObjectName("debugNatvisFiles");
    m_visualizers->setMaximumHeight(80);
    layout->addWidget(m_visualizers);
    m_loadVisualizers = new QPushButton(tr("Load / reload visualizers"), this);
    m_loadVisualizers->setObjectName("debugReloadNatvis");
    layout->addWidget(m_loadVisualizers);
    m_watcher = new QFileSystemWatcher(this);
    connect(&controller, &DebugController::visualizersChanged, this, &DebugSymbolsWindow::setVisualizers);
    setVisualizers(controller.visualizers());
    auto* saved = new QTimer(this);
    saved->setSingleShot(true);
    saved->setInterval(250);
    connect(saved, &QTimer::timeout, this, [this] {
        m_visualizersChanged = true;
        reloadVisualizers();
    });
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [saved](const QString&) { saved->start(); });
    // Parent watches survive atomic-save replacements that temporarily remove a file watch.
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [saved](const QString&) { saved->start(); });
    connect(m_refresh, &QPushButton::clicked, this, &DebugSymbolsWindow::refresh);
    connect(m_apply, &QPushButton::clicked, this, &DebugSymbolsWindow::applySymbols);
    connect(m_reload, &QPushButton::clicked, this, &DebugSymbolsWindow::reloadSelectedModule);
    connect(m_applySourceMap, &QPushButton::clicked, this, &DebugSymbolsWindow::applySourceMap);
    connect(m_loadVisualizers, &QPushButton::clicked, this, [this] {
        m_visualizersChanged = true;
        reloadVisualizers();
    });
    const auto update = [this] {
        const auto* session = m_controller.session();
        const bool stopped = session && session->state() == debug::DebugState::Stopped;
        const auto caps = session ? session->capabilities() : debug::dap::Capabilities{};
        m_refresh->setEnabled(stopped && caps.supportsModulesRequest);
        m_apply->setEnabled(stopped && caps.frostSupportsSymbolControl);
        m_reload->setEnabled(stopped && caps.frostSupportsSymbolControl);
        m_loadVisualizers->setEnabled(stopped && caps.frostSupportsNatvisReload);
        m_applySourceMap->setEnabled(stopped && caps.frostSupportsSourceMap);
        m_natvisStatus->setText(
            !caps.frostStructuredNatvis ? tr("Structured Natvis availability has not been announced by this adapter.")
            : *caps.frostStructuredNatvis
                ? tr("Structured Natvis is available; individual expressions may still report diagnostics.")
                : tr("Structured Natvis is unavailable in this debugger installation. Raw native fields remain "
                     "available."));
        if (!session) {
            m_modules->clear();
            m_status->setText(tr("No debug session."));
        }
        if (m_visualizersChanged)
            reloadVisualizers();
    };
    connect(&controller, &DebugController::stateChanged, this, update);
    update();
}
void DebugSymbolsWindow::resultMessage(bool success, const QString& text) {
    m_status->setText((success ? tr("Adapter: ") : tr("Unavailable: ")) + text);
}
void DebugSymbolsWindow::setVisualizers(QStringList files) {
    m_visualizers->setPlainText(files.join('\n'));
    if (!m_watcher->files().isEmpty())
        m_watcher->removePaths(m_watcher->files());
    if (!m_watcher->directories().isEmpty())
        m_watcher->removePaths(m_watcher->directories());
    if (!files.isEmpty())
        m_watcher->addPaths(files);
    QStringList directories;
    for (const auto& file : files)
        directories.push_back(QFileInfo(file).absolutePath());
    directories.removeDuplicates();
    if (!directories.isEmpty())
        m_watcher->addPaths(directories);
}
void DebugSymbolsWindow::refresh() {
    QPointer<DebugSymbolsWindow> self(this);
    m_controller.requestInspection(
        "modules", rpc::Json::object().set("startModule", 0).set("moduleCount", 4096),
        [self](Result<rpc::Json> result) {
            if (!self)
                return;
            if (!result) {
                self->resultMessage(false, QString::fromStdString(result.error().message()));
                return;
            }
            self->m_modules->clear();
            const auto* modules = result->find("modules");
            if (!modules || !modules->isArray() || modules->size() > 4096) {
                self->resultMessage(false, tr("Invalid or unbounded module response."));
                return;
            }
            for (const auto& module : modules->items()) {
                auto* row = new QTreeWidgetItem(
                    self->m_modules, {field(module, "name"), field(module, "path"), field(module, "addressRange"),
                                      field(module, "symbolStatus"), field(module, "symbolFilePath")});
                row->setData(0, Qt::UserRole, field(module, "name"));
            }
            self->resultMessage(true, tr("Module inventory refreshed."));
        });
}
void DebugSymbolsWindow::applySymbols() {
    QPointer<DebugSymbolsWindow> self(this);
    m_controller.requestInspection("frost.setSymbolPaths",
                                   rpc::Json::object()
                                       .set("symbolPaths", strings(lines(m_paths)))
                                       .set("allowSymbolServer", m_allowServer->isChecked()),
                                   [self](Result<rpc::Json> result) {
                                       if (self)
                                           self->resultMessage(
                                               bool(result), result ? tr("Symbol paths accepted.")
                                                                    : QString::fromStdString(result.error().message()));
                                   });
}
void DebugSymbolsWindow::reloadSelectedModule() {
    const auto* selected = m_modules->currentItem();
    if (!selected)
        return;
    QPointer<DebugSymbolsWindow> self(this);
    m_controller.requestInspection(
        "frost.reloadSymbols",
        rpc::Json::object().set("module", selected->data(0, Qt::UserRole).toString().toStdString()),
        [self](Result<rpc::Json> result) {
            if (!self)
                return;
            self->resultMessage(bool(result), result ? tr("Symbol reload completed.")
                                                     : QString::fromStdString(result.error().message()));
            if (result)
                self->refresh();
        });
}
void DebugSymbolsWindow::applySourceMap() {
    auto mappings = rpc::Json::object();
    for (const auto& line : lines(m_sourceMap)) {
        const auto separator = line.indexOf(QStringLiteral("=>"));
        if (separator <= 0 || line.indexOf(QStringLiteral("=>"), separator + 2) >= 0) {
            resultMessage(false, tr("Use one build-root => local-root pair per line."));
            return;
        }
        const auto source = line.left(separator).trimmed().toStdString();
        if (mappings.find(source)) {
            resultMessage(false, tr("Duplicate source mapping root."));
            return;
        }
        mappings.set(source, line.mid(separator + 2).trimmed().toStdString());
    }
    QPointer<DebugSymbolsWindow> self(this);
    m_controller.requestInspection("frost.setSourceMap", rpc::Json::object().set("sourceMap", std::move(mappings)),
                                   [self](Result<rpc::Json> result) {
                                       if (self)
                                           self->resultMessage(
                                               bool(result), result ? tr("Source mappings accepted.")
                                                                    : QString::fromStdString(result.error().message()));
                                   });
}
void DebugSymbolsWindow::reloadVisualizers() {
    const auto* session = m_controller.session();
    if (!session || session->state() != debug::DebugState::Stopped ||
        !session->capabilities().frostSupportsNatvisReload)
        return;
    const auto files = lines(m_visualizers);
    m_visualizersChanged = false;
    QPointer<DebugSymbolsWindow> self(this);
    m_controller.requestInspection(
        "frost.reloadNatvis", rpc::Json::object().set("natvis", strings(files)), [self](Result<rpc::Json> result) {
            if (self)
                self->resultMessage(
                    bool(result), result
                                      ? tr("Visualizer reload completed; adapter diagnostics are in the debug console.")
                                      : QString::fromStdString(result.error().message()));
        });
}
} // namespace frost::ui
