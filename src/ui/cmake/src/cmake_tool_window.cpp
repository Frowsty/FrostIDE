// SPDX-License-Identifier: Apache-2.0
//
// CMakeToolWindow (see frost/ui/cmake/cmake_tool_window.hpp): the widgets and their bindings to the
// controller's signals.

#include "frost/ui/cmake/cmake_tool_window.hpp"

#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/tr.hpp"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace frost::ui::cmake {

namespace {

constexpr std::string_view kContext = "cmake";
constexpr int kOutputLines = 20'000; // The output keeps the newest lines of a long configure.

} // namespace

CMakeToolWindow::CMakeToolWindow(CMakeController& controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(6, 3, 6, 3);
    bar->setSpacing(4);
    m_profiles = new QComboBox(this);
    m_profiles->setObjectName(QStringLiteral("cmakeProfiles"));
    m_profiles->setToolTip(qtr(kContext, "CMake profile"));
    m_profiles->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_reload = new QToolButton(this);
    m_reload->setObjectName(QStringLiteral("cmakeReload"));
    m_reload->setText(qtr(kContext, "Reload CMake Project"));
    m_reload->setAutoRaise(true);
    m_reload->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_stop = new QToolButton(this);
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
    bar->addStretch(1);
    layout->addLayout(bar);

    m_banner = new QLabel(this);
    m_banner->setObjectName(QStringLiteral("cmakeErrorBanner"));
    m_banner->setWordWrap(true);
    m_banner->setTextFormat(Qt::PlainText);
    m_banner->setContentsMargins(6, 4, 6, 4);
    m_banner->setVisible(false);
    layout->addWidget(m_banner);

    m_output = new QPlainTextEdit(this);
    m_output->setObjectName(QStringLiteral("cmakeOutput"));
    m_output->setFrameShape(QFrame::NoFrame);
    m_output->setReadOnly(true);
    m_output->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_output->setMaximumBlockCount(kOutputLines);
    layout->addWidget(m_output, 1);

    connect(m_reload, &QToolButton::clicked, &m_controller, &CMakeController::reload);
    connect(m_stop, &QToolButton::clicked, &m_controller, &CMakeController::cancel);
    connect(m_profiles, &QComboBox::activated, this, [this](int index) {
        if (!m_updatingProfiles) {
            m_controller.selectProfile(index);
        }
    });
    connect(&m_controller, &CMakeController::profilesChanged, this, &CMakeToolWindow::refreshProfiles);
    connect(&m_controller, &CMakeController::problemChanged, this, &CMakeToolWindow::refreshProblem);
    connect(&m_controller, &CMakeController::busyChanged, this, &CMakeToolWindow::refreshBusy);
    connect(&m_controller, &CMakeController::configureStarted, this, [this](const QString&) { m_output->clear(); });
    connect(&m_controller, &CMakeController::configureOutput, this,
            [this](int, const QString& line) { m_output->appendPlainText(line); });

    refreshProfiles();
    refreshProblem(m_controller.problem());
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
    m_stop->setEnabled(busy);
    m_profiles->setEnabled(m_controller.profileLabels().size() > 1 && !busy);
}

} // namespace frost::ui::cmake
