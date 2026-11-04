// SPDX-License-Identifier: Apache-2.0
//
// WelcomeScreen (frost/app/welcome_screen.hpp).

#include "frost/app/welcome_screen.hpp"

#include "frost/ui/glacial_logo.hpp"
#include "frost/ui/tr.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace frost::app {

namespace {

constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kTitleRole = Qt::UserRole + 2;
constexpr int kSubtitleRole = Qt::UserRole + 3;

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
    QPixmap pix(32, 32);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(2.0, 2.0, 28.0, 28.0), 6.0, 6.0);
    p.setPen(QColor(0xFF, 0xFF, 0xFF));
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(11);
    p.setFont(f);
    p.drawText(QRectF(2.0, 2.0, 28.0, 28.0), Qt::AlignCenter, projectInitials(name));
    p.end();
    return QIcon(pix);
}

QPixmap makeFrostBrandBadge() { return ui::glacialLogoPixmap(40, 2.0); }

class RecentProjectItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        const int h = std::max(48, option.fontMetrics.height() * 2 + 16);
        return QSize(option.rect.width(), h);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QRect rect = option.rect.adjusted(2, 1, -2, -1);
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
        if (selected || hovered) {
            QColor fill = option.palette.color(QPalette::Highlight);
            fill.setAlpha(selected ? 55 : 28);
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawRoundedRect(rect, 6.0, 6.0);
        }

        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const QRect iconRect(rect.left() + 10, rect.top() + (rect.height() - 32) / 2, 32, 32);
        icon.paint(painter, iconRect, Qt::AlignCenter);

        QString title = index.data(kTitleRole).toString();
        QString subtitle = index.data(kSubtitleRole).toString();
        if (title.isEmpty()) {
            const QString combined = index.data(Qt::DisplayRole).toString();
            const qsizetype nl = combined.indexOf(u'\n');
            title = nl >= 0 ? combined.left(nl) : combined;
            subtitle = nl >= 0 ? combined.mid(nl + 1) : QString();
        }

        const int textLeft = iconRect.right() + 12;
        const int textWidth = std::max(40, rect.right() - textLeft - 12);
        QFont titleFont = option.font;
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(option.palette.color(QPalette::Text));
        const QFontMetrics titleFm(titleFont);
        const QRect titleRect(textLeft, rect.top() + 6, textWidth, titleFm.height());
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          titleFm.elidedText(title, Qt::ElideRight, textWidth));

        if (!subtitle.isEmpty()) {
            QFont subFont = option.font;
            if (subFont.pointSizeF() > 8.0) {
                subFont.setPointSizeF(subFont.pointSizeF() * 0.92);
            }
            painter->setFont(subFont);
            QColor muted = option.palette.color(QPalette::PlaceholderText);
            if (!muted.isValid() || muted.alpha() == 0) {
                muted = option.palette.color(QPalette::Text);
                muted.setAlpha(150);
            }
            painter->setPen(muted);
            const QFontMetrics subFm(subFont);
            const QRect subRect(textLeft, titleRect.bottom() + 2, textWidth, subFm.height());
            painter->drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter,
                              subFm.elidedText(subtitle, Qt::ElideMiddle, textWidth));
        }

        painter->restore();
    }
};

} // namespace

WelcomeScreen::WelcomeScreen(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("welcomeScreen"));
    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // --- Left Sidebar (Rider 2-Pane Welcome Navigation) ---
    auto* sidebar = new QFrame(this);
    sidebar->setObjectName(QStringLiteral("welcomeSidebar"));
    sidebar->setFixedWidth(220);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(16, 24, 16, 20);
    sidebarLayout->setSpacing(12);

    auto* brandRow = new QHBoxLayout();
    brandRow->setSpacing(10);
    auto* brandIcon = new QLabel(sidebar);
    brandIcon->setPixmap(makeFrostBrandBadge());
    brandIcon->setFixedSize(40, 40);
    brandRow->addWidget(brandIcon);

    auto* brandTextLayout = new QVBoxLayout();
    brandTextLayout->setSpacing(1);
    auto* title = new QLabel(QStringLiteral("FrostIDE"), sidebar);
    title->setObjectName(QStringLiteral("welcomeTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.35);
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto* versionLabel = new QLabel(text("2026.11 • C++ / C# / .NET"), sidebar);
    versionLabel->setObjectName(QStringLiteral("welcomeVersion"));
    QFont verFont = versionLabel->font();
    if (verFont.pointSizeF() > 8.0) {
        verFont.setPointSizeF(verFont.pointSizeF() * 0.88);
    }
    versionLabel->setFont(verFont);
    brandTextLayout->addWidget(title);
    brandTextLayout->addWidget(versionLabel);
    brandRow->addLayout(brandTextLayout, 1);
    sidebarLayout->addLayout(brandRow);
    sidebarLayout->addSpacing(8);

    m_navList = new QListWidget(sidebar);
    m_navList->setObjectName(QStringLiteral("welcomeNavList"));
    m_navList->setFrameShape(QFrame::NoFrame);
    m_navList->setSpacing(2);
    m_navList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navList->setFixedHeight(108);
    for (const QString& navLabel : {text("Projects"), text("Customize"), text("Keymap Reference")}) {
        auto* navItem = new QListWidgetItem(navLabel, m_navList);
        navItem->setSizeHint(QSize(180, 32));
    }
    m_navList->setCurrentRow(0);
    sidebarLayout->addWidget(m_navList);
    sidebarLayout->addStretch(1);

    auto* quickThemeBtn = new QPushButton(text("Switch Theme..."), sidebar);
    quickThemeBtn->setObjectName(QStringLiteral("welcomeThemeButton"));
    quickThemeBtn->setMinimumHeight(30);
    connect(quickThemeBtn, &QPushButton::clicked, this, &WelcomeScreen::quickSwitchThemeRequested);
    sidebarLayout->addWidget(quickThemeBtn);

    auto* settingsBtn = new QPushButton(text("All Settings..."), sidebar);
    settingsBtn->setObjectName(QStringLiteral("welcomeSettingsButton"));
    settingsBtn->setMinimumHeight(30);
    connect(settingsBtn, &QPushButton::clicked, this, &WelcomeScreen::settingsRequested);
    sidebarLayout->addWidget(settingsBtn);

    rootLayout->addWidget(sidebar);

    auto* verticalDivider = new QFrame(this);
    verticalDivider->setFrameShape(QFrame::VLine);
    verticalDivider->setFrameShadow(QFrame::Plain);
    rootLayout->addWidget(verticalDivider);

    // --- Right Content Stack ---
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("welcomeStack"));

    // Page 0: Projects Page
    auto* projectsPage = new QWidget(m_pages);
    auto* projectsLayout = new QVBoxLayout(projectsPage);
    projectsLayout->setContentsMargins(32, 24, 32, 24);
    projectsLayout->setSpacing(14);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(10);
    m_search = new QLineEdit(projectsPage);
    m_search->setObjectName(QStringLiteral("welcomeSearch"));
    m_search->setPlaceholderText(text("Search projects..."));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumHeight(32);
    headerRow->addWidget(m_search, 1);

    m_newProject = new QPushButton(text("New Solution..."), projectsPage);
    m_newProject->setObjectName(QStringLiteral("welcomeNewProject"));
    m_newProject->setDefault(true);
    m_newProject->setMinimumHeight(32);
    m_newProject->setToolTip(text("Create a new C++, CMake, or .NET / C# project or solution"));
    m_open = new QPushButton(text("Open..."), projectsPage);
    m_open->setObjectName(QStringLiteral("welcomeOpen"));
    m_open->setMinimumHeight(32);
    m_open->setToolTip(text("Open a solution (.sln, .slnx), project, CMakeLists.txt, or a file"));
    m_openFolder = new QPushButton(text("Open Folder..."), projectsPage);
    m_openFolder->setObjectName(QStringLiteral("welcomeOpenFolder"));
    m_openFolder->setMinimumHeight(32);
    m_openFolder->setToolTip(text("Open a project folder, including a CMake project"));
    headerRow->addWidget(m_newProject);
    headerRow->addWidget(m_open);
    headerRow->addWidget(m_openFolder);
    projectsLayout->addLayout(headerRow);

    auto* headerDivider = new QFrame(projectsPage);
    headerDivider->setFrameShape(QFrame::HLine);
    headerDivider->setFrameShadow(QFrame::Plain);
    projectsLayout->addWidget(headerDivider);

    auto* subtitle = new QLabel(text("Recent Solutions & Projects"), projectsPage);
    subtitle->setObjectName(QStringLiteral("welcomeSubtitle"));
    QFont sectionFont = subtitle->font();
    sectionFont.setBold(true);
    subtitle->setFont(sectionFont);
    projectsLayout->addWidget(subtitle);

    m_recent = new QListWidget(projectsPage);
    m_recent->setObjectName(QStringLiteral("welcomeRecent"));
    m_recent->setFrameShape(QFrame::NoFrame);
    m_recent->setIconSize(QSize(32, 32));
    m_recent->setSpacing(2);
    m_recent->setItemDelegate(new RecentProjectItemDelegate(m_recent));
    m_recent->setContextMenuPolicy(Qt::CustomContextMenu);
    m_recent->setSelectionMode(QAbstractItemView::SingleSelection);
    m_recent->viewport()->setAttribute(Qt::WA_Hover, true);
    m_recent->installEventFilter(this);
    projectsLayout->addWidget(m_recent, 1);
    m_pages->addWidget(projectsPage);

    // Page 1: Customize Page
    auto* customizePage = new QWidget(m_pages);
    auto* customizeLayout = new QVBoxLayout(customizePage);
    customizeLayout->setContentsMargins(32, 28, 32, 28);
    customizeLayout->setSpacing(16);
    auto* customizeTitle = new QLabel(text("Customize Appearance & Behavior"), customizePage);
    QFont pageHeadingFont = customizeTitle->font();
    pageHeadingFont.setPointSizeF(pageHeadingFont.pointSizeF() * 1.3);
    pageHeadingFont.setBold(true);
    customizeTitle->setFont(pageHeadingFont);
    customizeLayout->addWidget(customizeTitle);

    auto* themeDesc =
        new QLabel(text("Choose from 14 built-in color themes including Rider Dark, Rider Light, Rider Islands Dark, "
                        "Darcula, Monokai Pro, Dracula, Nord, One Dark, Solarized, and Glacier."),
                   customizePage);
    themeDesc->setWordWrap(true);
    customizeLayout->addWidget(themeDesc);

    auto* customizeBtnRow = new QHBoxLayout();
    auto* openThemePopupBtn = new QPushButton(text("Quick Switch Color Theme..."), customizePage);
    openThemePopupBtn->setObjectName(QStringLiteral("welcomeCustomizeThemeButton"));
    openThemePopupBtn->setMinimumHeight(32);
    connect(openThemePopupBtn, &QPushButton::clicked, this, &WelcomeScreen::quickSwitchThemeRequested);
    auto* openSettingsPageBtn = new QPushButton(text("Configure Keymap & Editor Settings..."), customizePage);
    openSettingsPageBtn->setObjectName(QStringLiteral("welcomeCustomizeSettingsButton"));
    openSettingsPageBtn->setMinimumHeight(32);
    connect(openSettingsPageBtn, &QPushButton::clicked, this, &WelcomeScreen::settingsRequested);
    customizeBtnRow->addWidget(openThemePopupBtn);
    customizeBtnRow->addWidget(openSettingsPageBtn);
    customizeBtnRow->addStretch(1);
    customizeLayout->addLayout(customizeBtnRow);
    customizeLayout->addStretch(1);
    m_pages->addWidget(customizePage);

    // Page 2: Keymap Reference Page
    auto* keymapPage = new QWidget(m_pages);
    auto* keymapLayout = new QVBoxLayout(keymapPage);
    keymapLayout->setContentsMargins(32, 28, 32, 28);
    keymapLayout->setSpacing(14);
    auto* keymapTitle = new QLabel(text("Essential Keyboard Shortcuts"), keymapPage);
    keymapTitle->setFont(pageHeadingFont);
    keymapLayout->addWidget(keymapTitle);

    auto* shortcutGrid = new QGridLayout();
    shortcutGrid->setHorizontalSpacing(28);
    shortcutGrid->setVerticalSpacing(8);
    struct ShortcutRow {
        const char* action;
        const char* shortcut;
    };
    static constexpr ShortcutRow kShortcuts[] = {
        {"Show Context Actions / Quick Fixes", "Alt+Enter"},
        {"Search Everywhere (All / Classes / Files / Symbols / Actions)", "Double Shift / Ctrl+T"},
        {"Find Action", "Ctrl+Shift+A"},
        {"Go to File", "Ctrl+Shift+N"},
        {"Go to Declaration or Usages", "Ctrl+B / F12"},
        {"Go to Implementation", "Ctrl+Alt+B / Ctrl+F12"},
        {"Find Usages", "Alt+F7 / Shift+F12"},
        {"Rename Symbol", "Shift+F6 / Ctrl+R, Ctrl+R"},
        {"Reformat Code", "Ctrl+Alt+L / Ctrl+K, Ctrl+D"},
        {"Build Solution", "Ctrl+Shift+B / F6"},
        {"Run / Debug Target", "Shift+F10 / Shift+F9 (F5)"},
        {"Toggle Line Breakpoint", "Ctrl+F8 / F9"},
        {"Solution / Project Explorer", "Alt+1"},
        {"Problems View / Terminal", "Alt+6 / Alt+F12"},
    };
    int gridRow = 0;
    for (const ShortcutRow& entry : kShortcuts) {
        auto* actLbl = new QLabel(text(entry.action), keymapPage);
        auto* keyLbl = new QLabel(QString::fromUtf8(entry.shortcut), keymapPage);
        QFont keyFont = keyLbl->font();
        keyFont.setBold(true);
        keyLbl->setFont(keyFont);
        shortcutGrid->addWidget(actLbl, gridRow, 0);
        shortcutGrid->addWidget(keyLbl, gridRow, 1);
        ++gridRow;
    }
    keymapLayout->addLayout(shortcutGrid);
    keymapLayout->addStretch(1);
    m_pages->addWidget(keymapPage);

    rootLayout->addWidget(m_pages, 1);

    connect(m_navList, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_search, &QLineEdit::textChanged, this, &WelcomeScreen::filterRecentProjects);
    connect(m_newProject, &QPushButton::clicked, this, &WelcomeScreen::newProjectRequested);
    connect(m_open, &QPushButton::clicked, this, &WelcomeScreen::openRequested);
    connect(m_openFolder, &QPushButton::clicked, this, &WelcomeScreen::openFolderRequested);
    // Single click or Enter opens the recent project.
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
        item->setData(kTitleRole, name);
        item->setData(kSubtitleRole, native);
        item->setToolTip(native);
    }
    if (m_search != nullptr && !m_search->text().isEmpty()) {
        filterRecentProjects(m_search->text());
    }
}

void WelcomeScreen::filterRecentProjects(const QString& query) {
    const QString trimmed = query.trimmed();
    for (int row = 0; row < m_recent->count(); ++row) {
        QListWidgetItem* item = m_recent->item(row);
        if (trimmed.isEmpty()) {
            item->setHidden(false);
            continue;
        }
        const bool match = item->text().contains(trimmed, Qt::CaseInsensitive);
        item->setHidden(!match);
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
    QAction* copyPath = menu.addAction(text("Copy Path"));
    menu.addSeparator();
    QAction* remove = menu.addAction(text("Remove from Recent Projects"));
    QAction* chosen = menu.exec(m_recent->viewport()->mapToGlobal(position));
    if (chosen == open) {
        activateRow(row);
    } else if (chosen == copyPath) {
        if (QListWidgetItem* item = m_recent->item(row); item != nullptr) {
            QApplication::clipboard()->setText(QDir::toNativeSeparators(item->data(kPathRole).toString()));
        }
    } else if (chosen == remove) {
        removeRow(row);
    }
}

} // namespace frost::app
