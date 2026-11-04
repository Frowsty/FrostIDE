// SPDX-License-Identifier: Apache-2.0
//
// WelcomeScreen (frost/app/welcome_screen.hpp).

#include "frost/app/welcome_screen.hpp"

#include "frost/ui/tr.hpp"

#include <QAction>
#include <QColor>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace frost::app {

namespace {

constexpr int kPathRole = Qt::UserRole + 1;

QString text(const char* source) { return ui::qtr("frost::app::WelcomeScreen", source); }

QString projectInitials(const QString& name) {
    QString letters;
    for (int i = 0; i < name.size() && letters.size() < 2; ++i) {
        const QChar ch = name.at(i);
        if (ch.isLetterOrNumber() && (i == 0 || ch.isUpper() || !name.at(i - 1).isLetterOrNumber())) {
            letters.append(ch.toUpper());
        }
    }
    if (letters.size() < 2) {
        for (int i = 0; i < name.size() && letters.size() < 2; ++i) {
            if (name.at(i).isLetterOrNumber()) {
                const QChar upper = name.at(i).toUpper();
                if (!letters.contains(upper) || letters.isEmpty()) {
                    letters.append(upper);
                }
            }
        }
    }
    return letters.isEmpty() ? QStringLiteral("PR") : letters;
}

QIcon makeProjectAvatarIcon(const QString& name) {
    static constexpr QColor kPalette[] = {
        QColor(0x35, 0x74, 0xF0), QColor(0x83, 0x4D, 0xF0), QColor(0x20, 0x8A, 0x5B),
        QColor(0xC9, 0x62, 0x2B), QColor(0x2A, 0x7B, 0x9B), QColor(0xB8, 0x3E, 0x65),
    };
    std::uint32_t hash = 2166136261U;
    for (QChar ch : name) {
        hash ^= static_cast<std::uint32_t>(ch.unicode());
        hash *= 16777619U;
    }
    const QColor color = kPalette[hash % 6];
    QPixmap pix(28, 28);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(2.0, 2.0, 24.0, 24.0), 6.0, 6.0);
    p.setPen(QColor(0xFF, 0xFF, 0xFF));
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(11);
    p.setFont(f);
    p.drawText(QRectF(2.0, 2.0, 24.0, 24.0), Qt::AlignCenter, projectInitials(name));
    p.end();
    return QIcon(pix);
}

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

    auto* subtitle = new QLabel(text("Welcome to FrostIDE — C++, C#, and .NET Development Environment"), this);
    subtitle->setObjectName(QStringLiteral("welcomeSubtitle"));
    layout->addWidget(subtitle);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(10);
    m_newProject = new QPushButton(text("New Solution / Project..."), this);
    m_newProject->setObjectName(QStringLiteral("welcomeNewProject"));
    m_newProject->setDefault(true);
    m_newProject->setMinimumHeight(32);
    m_newProject->setToolTip(text("Create a new C++, CMake, or .NET / C# project or solution"));
    m_open = new QPushButton(text("Open..."), this);
    m_open->setObjectName(QStringLiteral("welcomeOpen"));
    m_open->setMinimumHeight(32);
    m_open->setToolTip(text("Open a solution (.sln, .slnx), project, CMakeLists.txt, or a file"));
    m_openFolder = new QPushButton(text("Open Folder..."), this);
    m_openFolder->setObjectName(QStringLiteral("welcomeOpenFolder"));
    m_openFolder->setMinimumHeight(32);
    m_openFolder->setToolTip(text("Open a project folder, including a CMake project"));
    buttons->addWidget(m_newProject);
    buttons->addWidget(m_open);
    buttons->addWidget(m_openFolder);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto* recentTitle = new QLabel(text("Recent Projects"), this);
    QFont sectionFont = recentTitle->font();
    sectionFont.setBold(true);
    recentTitle->setFont(sectionFont);
    layout->addWidget(recentTitle);
    m_recent = new QListWidget(this);
    m_recent->setObjectName(QStringLiteral("welcomeRecent"));
    m_recent->setIconSize(QSize(28, 28));
    m_recent->setSpacing(3);
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
        auto* item =
            new QListWidgetItem(makeProjectAvatarIcon(name), QStringLiteral("%1\n%2").arg(name, native), m_recent);
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
