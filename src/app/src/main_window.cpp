// SPDX-License-Identifier: Apache-2.0
//
// Implementation of the M0 main window (see frost/app/main_window.hpp).

#include "frost/app/main_window.hpp"

#include "frost/core/build_info.hpp"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QString>

namespace frost::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setObjectName(QStringLiteral("FrostMainWindow"));
    setWindowTitle(QStringLiteral("FrostIDE"));
    resize(1280, 800);

    // TODO(M1): route every user-visible string through frost::i18n::tr (PLAN.md section 2.2).
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* exitAction = fileMenu->addAction(QStringLiteral("E&xit"));
    exitAction->setObjectName(QStringLiteral("actionExit"));
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    auto* placeholder = new QLabel(QStringLiteral("FrostIDE %1").arg(QString::fromUtf8(build_info::kVersion)), this);
    placeholder->setObjectName(QStringLiteral("placeholder"));
    placeholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder);

    statusBar()->showMessage(QStringLiteral("Ready"));
}

MainWindow::~MainWindow() = default;

} // namespace frost::app
