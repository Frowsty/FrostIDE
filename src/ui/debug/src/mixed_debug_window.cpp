// SPDX-License-Identifier: Apache-2.0
// Displays captured native/Lua stacks and emits the user's mixed-debug inspection actions.
#include "frost/ui/mixed_debug_window.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace frost::ui {
namespace {
void showStack(QTreeWidget& tree, const std::optional<MixedDebugStack>& stack) {
    tree.clear();
    if (!stack)
        return;
    tree.setHeaderLabels({QString::fromStdString(stack->title) + QObject::tr(" — thread %1").arg(stack->threadId),
                          QObject::tr("Source")});
    for (const auto& frame : stack->frames.frames) {
        auto* row = new QTreeWidgetItem(
            &tree, {QString::fromStdString(frame.name),
                    frame.path.empty() ? QString::fromStdString(frame.sourceName)
                                       : QString::fromStdString(frame.path) + QStringLiteral(":%1").arg(frame.line)});
        row->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(stack->sessionId));
        row->setData(0, Qt::UserRole + 1, QVariant::fromValue<qlonglong>(stack->threadId));
        row->setData(0, Qt::UserRole + 2, QVariant::fromValue<qlonglong>(frame.id));
    }
}
} // namespace
MixedDebugWindow::MixedDebugWindow(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("mixedDebugWindow"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("mixedDebugStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto* commands = new QHBoxLayout;
    m_capture = new QPushButton(tr("Capture C++ stack"), this);
    m_capture->setObjectName(QStringLiteral("mixedDebugCapture"));
    m_resume = new QPushButton(tr("Continue C++ and Lua"), this);
    m_resume->setObjectName(QStringLiteral("mixedDebugContinue"));
    commands->addWidget(m_capture);
    commands->addWidget(m_resume);
    commands->addStretch();
    layout->addLayout(commands);
    auto* stacks = new QHBoxLayout;
    for (auto** tree : {&m_native, &m_lua}) {
        *tree = new QTreeWidget(this);
        (*tree)->setRootIsDecorated(false);
        (*tree)->setColumnCount(2);
        stacks->addWidget(*tree);
        connect(*tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* row) {
            emit frameRequested(row->data(0, Qt::UserRole).toULongLong(), row->data(0, Qt::UserRole + 1).toLongLong(),
                                row->data(0, Qt::UserRole + 2).toLongLong());
        });
    }
    m_native->setObjectName(QStringLiteral("mixedNativeStack"));
    m_lua->setObjectName(QStringLiteral("mixedLuaStack"));
    layout->addLayout(stacks, 1);
    connect(m_capture, &QPushButton::clicked, this, &MixedDebugWindow::captureRequested);
    connect(m_resume, &QPushButton::clicked, this, &MixedDebugWindow::resumeRequested);
    setSnapshot({});
}
void MixedDebugWindow::setSnapshot(const MixedDebugSnapshot& snapshot) {
    m_status->setText(snapshot.status);
    m_capture->setEnabled(snapshot.canCapture);
    m_resume->setEnabled(snapshot.canResume);
    m_native->setHeaderLabels({tr("C++ stack"), tr("Source")});
    m_lua->setHeaderLabels({tr("Lua stack"), tr("Source")});
    showStack(*m_native, snapshot.native);
    showStack(*m_lua, snapshot.lua);
}
} // namespace frost::ui
