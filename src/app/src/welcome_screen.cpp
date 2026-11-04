// SPDX-License-Identifier: Apache-2.0
//
// WelcomeScreen (frost/app/welcome_screen.hpp).

#include "frost/app/welcome_screen.hpp"

#include "frost/ui/tr.hpp"

#include <QAction>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

namespace frost::app {

namespace {

constexpr int kPathRole = Qt::UserRole + 1;

QString text(const char* source) { return ui::qtr("frost::app::WelcomeScreen", source); }

} // namespace

WelcomeScreen::WelcomeScreen(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("welcomeScreen"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(48, 40, 48, 40);
    layout->setSpacing(16);

    auto* title = new QLabel(QStringLiteral("FrostIDE"), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 2.0);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto* buttons = new QHBoxLayout();
    m_newProject = new QPushButton(text("New Project..."), this);
    m_newProject->setObjectName(QStringLiteral("welcomeNewProject"));
    m_newProject->setToolTip(text("Create a new C++ CMake project or empty project folder"));
    m_open = new QPushButton(text("Open..."), this);
    m_open->setObjectName(QStringLiteral("welcomeOpen"));
    m_open->setToolTip(text("Open a solution (.sln, .slnx), CMakeLists.txt, or a file"));
    m_openFolder = new QPushButton(text("Open Folder..."), this);
    m_openFolder->setObjectName(QStringLiteral("welcomeOpenFolder"));
    m_openFolder->setToolTip(text("Open a project folder, including a CMake project"));
    buttons->addWidget(m_newProject);
    buttons->addWidget(m_open);
    buttons->addWidget(m_openFolder);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto* recentTitle = new QLabel(text("Recent Projects"), this);
    layout->addWidget(recentTitle);
    m_recent = new QListWidget(this);
    m_recent->setObjectName(QStringLiteral("welcomeRecent"));
    m_recent->setContextMenuPolicy(Qt::CustomContextMenu);
    m_recent->setSelectionMode(QAbstractItemView::SingleSelection);
    m_recent->installEventFilter(this);
    layout->addWidget(m_recent, 1);

    connect(m_newProject, &QPushButton::clicked, this, &WelcomeScreen::newProjectRequested);
    connect(m_open, &QPushButton::clicked, this, &WelcomeScreen::openRequested);
    connect(m_openFolder, &QPushButton::clicked, this, &WelcomeScreen::openFolderRequested);
    // Rider opens a recent project on a single click; Enter opens the current row (eventFilter).
    connect(m_recent, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) { activateRow(m_recent->row(item)); });
    connect(m_recent, &QListWidget::customContextMenuRequested, this, &WelcomeScreen::showContextMenu);
}

WelcomeScreen::~WelcomeScreen() = default;

void WelcomeScreen::setRecentProjects(const std::vector<std::string>& paths) {
    m_recent->clear();
    for (const std::string& path : paths) {
        const QString native = QDir::toNativeSeparators(QString::fromStdString(path));
        const QFileInfo info(native);
        QString name = info.fileName();
        if (name.isEmpty()) {
            name = native;
        }
        auto* item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(name, native), m_recent);
        item->setData(kPathRole, QString::fromStdString(path));
        item->setToolTip(native);
    }
}

std::vector<std::string> WelcomeScreen::recentProjects() const {
    std::vector<std::string> out;
    for (int row = 0; row < m_recent->count(); ++row) {
        out.push_back(m_recent->item(row)->data(kPathRole).toString().toStdString());
    }
    return out;
}

void WelcomeScreen::activateRow(int row) {
    if (QListWidgetItem* item = m_recent->item(row); item != nullptr) {
        emit openRecent(item->data(kPathRole).toString());
    }
}

void WelcomeScreen::removeRow(int row) {
    if (QListWidgetItem* item = m_recent->item(row); item != nullptr) {
        emit removeRecent(item->data(kPathRole).toString());
    }
}

bool WelcomeScreen::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_recent && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Delete && m_recent->currentRow() >= 0) {
            removeRow(m_recent->currentRow());
            return true;
        }
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && m_recent->currentRow() >= 0) {
            activateRow(m_recent->currentRow());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void WelcomeScreen::showContextMenu(const QPoint& position) {
    const int row = m_recent->row(m_recent->itemAt(position));
    if (row < 0) {
        return;
    }
    QMenu menu(this);
    QAction* open = menu.addAction(text("Open"));
    QAction* remove = menu.addAction(text("Remove from Recent Projects"));
    QAction* chosen = menu.exec(m_recent->viewport()->mapToGlobal(position));
    if (chosen == open) {
        activateRow(row);
    } else if (chosen == remove) {
        removeRow(row);
    }
}

} // namespace frost::app
