// SPDX-License-Identifier: Apache-2.0
//
// CMakeToolWindow (see frost/ui/cmake/cmake_tool_window.hpp): the widgets and their bindings to the
// controller's signals.

#include "frost/ui/cmake/cmake_tool_window.hpp"

#include "frost/project/cmake_presets.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/tr.hpp"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QStyledItemDelegate>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <chrono>
#include <utility>
#include <vector>

namespace frost::ui::cmake {

namespace {

constexpr std::string_view kContext = "cmake";
class CacheDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (index.column() != 2 || !index.data(Qt::UserRole).toBool())
            return nullptr;
        return QStyledItemDelegate::createEditor(parent, option, index);
    }
};

constexpr int kOutputLines = 20'000; // The output keeps the newest lines of a long configure.

QString toQString(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

} // namespace

CMakeToolWindow::CMakeToolWindow(CMakeController& controller, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_profiles(new QComboBox(this)), m_reload(new QToolButton(this)),
      m_stop(new QToolButton(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(6, 3, 6, 3);
    bar->setSpacing(4);
    m_profiles->setObjectName(QStringLiteral("cmakeProfiles"));
    m_profiles->setToolTip(qtr(kContext, "CMake profile"));
    m_profiles->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_reload->setObjectName(QStringLiteral("cmakeReload"));
    m_reload->setText(qtr(kContext, "Reload CMake Project"));
    m_reload->setAutoRaise(true);
    m_reload->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_stop->setObjectName(QStringLiteral("cmakeStop"));
    m_stop->setText(qtr(kContext, "Stop"));
    m_stop->setAutoRaise(true);
    m_stop->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_stop->setEnabled(false);
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        m_reload->setIcon(style->icon(icons::kActionsRefresh));
        m_stop->setIcon(style->icon(icons::kRunStop, IconSlot::Small, IconRole::Error));
    }
    bar->addWidget(m_profiles);
    bar->addWidget(m_reload);
    bar->addWidget(m_stop);
    auto* kit = new QToolButton(this);
    kit->setObjectName(QStringLiteral("cmakeCreateKit"));
    kit->setText(qtr(kContext, "Create Kit…"));
    bar->addWidget(kit);
    connect(kit, &QToolButton::clicked, this, &CMakeToolWindow::createKitRequested);
    connect(&m_controller, &CMakeController::busyChanged, kit, [kit](bool busy) { kit->setEnabled(!busy); });
    bar->addStretch(1);
    layout->addLayout(bar);

    // The progress row (CMK-15): what the service is doing, for how long, with a busy bar.
    m_progressRow = new QWidget(this);
    m_progressRow->setObjectName(QStringLiteral("cmakeProgress"));
    auto* progress = new QHBoxLayout(m_progressRow);
    progress->setContentsMargins(6, 2, 6, 2);
    progress->setSpacing(6);
    m_progressLabel = new QLabel(m_progressRow);
    m_progressLabel->setObjectName(QStringLiteral("cmakeProgressLabel"));
    m_progressLabel->setTextFormat(Qt::PlainText);
    m_progressBar = new QProgressBar(m_progressRow);
    m_progressBar->setRange(0, 0); // Indeterminate: cmake reports phases, not fractions.
    m_progressBar->setTextVisible(false);
    m_progressBar->setMaximumHeight(6);
    m_progressBar->setMaximumWidth(160);
    progress->addWidget(m_progressLabel);
    progress->addWidget(m_progressBar);
    progress->addStretch(1);
    m_progressRow->setVisible(false);
    layout->addWidget(m_progressRow);
    m_elapsed = new QTimer(this);
    m_elapsed->setInterval(1000);

    m_banner = new QLabel(this);
    m_banner->setObjectName(QStringLiteral("cmakeErrorBanner"));
    m_banner->setWordWrap(true);
    m_banner->setTextFormat(Qt::PlainText);
    m_banner->setContentsMargins(6, 4, 6, 4);
    m_banner->setVisible(false);
    layout->addWidget(m_banner);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("cmakeTabs"));
    m_tabs->setDocumentMode(true);
    m_output = new QPlainTextEdit(m_tabs);
    m_output->setObjectName(QStringLiteral("cmakeOutput"));
    m_output->setFrameShape(QFrame::NoFrame);
    m_output->setReadOnly(true);
    m_output->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_output->setMaximumBlockCount(kOutputLines);
    m_tabs->addTab(m_output, qtr(kContext, "Output"));
    m_presets = new QTreeWidget(m_tabs);
    m_presets->setObjectName(QStringLiteral("cmakePresets"));
    m_presets->setFrameShape(QFrame::NoFrame);
    m_presets->setColumnCount(2);
    m_presets->setHeaderLabels({qtr(kContext, "Preset"), qtr(kContext, "Display name")});
    m_presets->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tabs->addTab(m_presets, qtr(kContext, "Presets"));
    m_targets = new QTreeWidget(m_tabs);
    m_targets->setObjectName(QStringLiteral("cmakeTargets"));
    m_targets->setHeaderLabels({qtr(kContext, "Target"), qtr(kContext, "Source directory")});
    m_tabs->addTab(m_targets, qtr(kContext, "Targets"));
    auto* cachePage = new QWidget(m_tabs);
    auto* cacheLayout = new QVBoxLayout(cachePage);
    auto* cacheHelp = new QLabel(qtr(kContext, "Edits are pending until Apply reconfigures successfully. Presets are unchanged; a later Reload may restore preset values."), cachePage);
    cacheHelp->setWordWrap(true);
    cacheLayout->addWidget(cacheHelp);
    m_cache = new QTreeWidget(cachePage);
    m_cache->setObjectName(QStringLiteral("cmakeCache"));
    m_cache->setHeaderLabels({qtr(kContext, "Variable"), qtr(kContext, "Type"), qtr(kContext, "Value")});
    m_cache->setItemDelegate(new CacheDelegate(m_cache));
    cacheLayout->addWidget(m_cache);
    m_applyCache = new QToolButton(cachePage);
    m_applyCache->setObjectName(QStringLiteral("cmakeApplyCache"));
    m_applyCache->setText(qtr(kContext, "Apply and Reconfigure"));
    cacheLayout->addWidget(m_applyCache);
    m_tabs->addTab(cachePage, qtr(kContext, "Cache"));
    connect(m_applyCache, &QToolButton::clicked, this, [this] {
        std::vector<project::CMakeCacheVariable> edits;
        for (int row = 0; row < m_cache->topLevelItemCount(); ++row) {
            const auto* item = m_cache->topLevelItem(row);
            if (item->data(2, Qt::UserRole).toBool() && item->text(2) != item->data(2, Qt::UserRole + 1).toString())
                edits.push_back({item->text(0).toStdString(), item->text(1).toStdString(), item->text(2).toStdString()});
        }
        if (!edits.empty())
            m_controller.applyCache(std::move(edits));
    });
    connect(&m_controller, &CMakeController::modelChanged, this, &CMakeToolWindow::refreshModel);
    layout->addWidget(m_tabs, 1);

    connect(m_reload, &QToolButton::clicked, &m_controller, &CMakeController::reload);
    connect(m_stop, &QToolButton::clicked, &m_controller, &CMakeController::cancel);
    connect(m_profiles, &QComboBox::activated, this, [this](int index) {
        if (!m_updatingProfiles) {
            m_controller.selectProfile(index);
        }
    });
    connect(&m_controller, &CMakeController::profilesChanged, this, [this] {
        refreshProfiles();
        refreshPresets();
    });
    connect(&m_controller, &CMakeController::problemChanged, this, &CMakeToolWindow::refreshProblem);
    connect(&m_controller, &CMakeController::busyChanged, this, &CMakeToolWindow::refreshBusy);
    connect(&m_controller, &CMakeController::progressChanged, this, &CMakeToolWindow::refreshProgress);
    connect(m_elapsed, &QTimer::timeout, this, &CMakeToolWindow::refreshProgress);
    connect(&m_controller, &CMakeController::configureStarted, this, [this](const QString&) { m_output->clear(); });
    connect(&m_controller, &CMakeController::configureOutput, this,
            [this](int, const QString& line) { m_output->appendPlainText(line); });

    refreshModel();
    refreshProfiles();
    refreshPresets();
    refreshProblem(m_controller.problem());
    refreshBusy(m_controller.busy());
}

void CMakeToolWindow::refreshModel() {
    m_targets->clear();
    m_cache->clear();
    const auto& model = m_controller.model();
    if (model) {
        for (const auto& target : model->targets)
            (void)new QTreeWidgetItem(m_targets, {toQString(target.name), toQString(target.sourceDirectory)});
        for (const auto& entry : model->cache) {
            auto* item = new QTreeWidgetItem(m_cache, {toQString(entry.name), toQString(entry.type), toQString(entry.value)});
            const bool editable = entry.name != "CMAKE_EXPORT_COMPILE_COMMANDS" &&
                (entry.type == "BOOL" || entry.type == "STRING" || entry.type == "PATH" || entry.type == "FILEPATH");
            item->setData(2, Qt::UserRole, editable);
            item->setData(2, Qt::UserRole + 1, toQString(entry.value));
            if (editable)
                item->setFlags(item->flags() | Qt::ItemIsEditable);
        }
    }
    refreshBusy(m_controller.busy());
}

void CMakeToolWindow::refreshProfiles() {
    m_updatingProfiles = true;
    const QSignalBlocker blocker(m_profiles);
    m_profiles->clear();
    m_profiles->addItems(m_controller.profileLabels());
    m_profiles->setCurrentIndex(m_controller.activeProfile());
    m_profiles->setEnabled(m_controller.profileLabels().size() > 1 && !m_controller.busy());
    m_updatingProfiles = false;
}

void CMakeToolWindow::refreshProblem(const QString& problem) {
    m_banner->setText(problem);
    m_banner->setVisible(!problem.isEmpty());
}

void CMakeToolWindow::refreshBusy(bool busy) {
    m_reload->setEnabled(!busy);
    m_cache->setEnabled(!busy);
    m_applyCache->setEnabled(!busy && m_controller.model() && m_controller.model()->cacheAvailable);
    m_stop->setEnabled(busy);
    m_profiles->setEnabled(m_controller.profileLabels().size() > 1 && !busy);
    refreshProgress();
}

void CMakeToolWindow::refreshProgress() {
    const build::CMakeProgress& progress = m_controller.progress();
    const QString text = m_controller.progressText();
    if (text.isEmpty()) {
        m_elapsed->stop();
        m_progressRow->setVisible(false);
        m_progressLabel->clear();
        return;
    }
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - progress.since).count();
    m_progressLabel->setText(qtr(kContext, "%1 (%2 s)").arg(text).arg(static_cast<qlonglong>(seconds)));
    m_progressRow->setVisible(true);
    if (!m_elapsed->isActive()) {
        m_elapsed->start();
    }
    // Stop follows the reported activity while the folder is busy (a reply read cannot be
    // stopped); before the first report arrives refreshBusy keeps Stop on.
    if (m_controller.busy()) {
        m_stop->setEnabled(progress.cancellable);
    }
}

void CMakeToolWindow::refreshPresets() {
    m_presets->clear();
    const project::CMakePresetListing& listing = m_controller.presetListing();
    const std::pair<QString, const std::vector<project::CMakePresetListEntry>*> kinds[] = {
        {qtr(kContext, "Configure presets"), &listing.configure}, {qtr(kContext, "Build presets"), &listing.build},
        {qtr(kContext, "Test presets"), &listing.test},           {qtr(kContext, "Package presets"), &listing.package},
        {qtr(kContext, "Workflow presets"), &listing.workflow},
    };
    for (const auto& [title, entries] : kinds) {
        if (entries->empty()) {
            continue; // cmake --list-presets leaves out empty kinds too.
        }
        auto* kind = new QTreeWidgetItem(m_presets, QStringList{title});
        for (const project::CMakePresetListEntry& entry : *entries) {
            (void)new QTreeWidgetItem(kind, QStringList{toQString(entry.name), toQString(entry.displayName)});
        }
    }
    m_presets->expandAll();
}

} // namespace frost::ui::cmake
