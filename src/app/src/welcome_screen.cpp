// SPDX-License-Identifier: Apache-2.0
//
// WelcomeScreen (frost/app/welcome_screen.hpp).

#include "frost/app/welcome_screen.hpp"

#include "frost/core/build_info.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/glacial_logo.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/tr.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QRadialGradient>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::app {

namespace {

constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kTitleRole = Qt::UserRole + 2;
constexpr int kSubtitleRole = Qt::UserRole + 3;
constexpr int kBadgeRole = Qt::UserRole + 4;
constexpr int kAvatarColorRole = Qt::UserRole + 5;

QString text(const char* source) { return ui::qtr("frost::app::WelcomeScreen", source); }

QColor themeColor(ui::ColorKey key, const QColor& fallback) {
    if (const ui::FrostStyle* style = ui::FrostStyle::current()) {
        return style->color(key);
    }
    return fallback;
}

void paintLauncherEllipticalGlow(QPainter* painter, const QPointF& center, qreal rx, qreal ry, const QColor& color,
                                 int peakAlpha) {
    if (painter == nullptr || rx <= 0.0 || ry <= 0.0 || peakAlpha <= 0) {
        return;
    }
    painter->save();
    painter->translate(center);
    painter->scale(1.0, ry / rx);
    QRadialGradient grad(QPointF(0.0, 0.0), rx);
    QColor c0 = color;
    c0.setAlpha(std::clamp(peakAlpha, 0, 255));
    QColor cMid = color;
    cMid.setAlpha(std::clamp(peakAlpha / 2, 0, 255));
    QColor c1 = color;
    c1.setAlpha(0);
    grad.setColorAt(0.0, c0);
    grad.setColorAt(0.50, cMid);
    grad.setColorAt(1.0, c1);
    painter->setPen(Qt::NoPen);
    painter->setBrush(grad);
    painter->drawEllipse(QPointF(0.0, 0.0), rx, rx);
    painter->restore();
}

#ifdef _WIN32
void syncWin32LauncherDarkFrame(QWidget* window) {
    if (window == nullptr || QGuiApplication::platformName() != QStringLiteral("windows")) {
        return;
    }
    const WId wid = window->winId();
    if (wid == 0) {
        return;
    }
    auto* hwnd = reinterpret_cast<HWND>(wid);
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    if (dwm == nullptr) {
        dwm = LoadLibraryW(L"dwmapi.dll");
    }
    if (dwm == nullptr) {
        return;
    }
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttr = reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (setAttr == nullptr) {
        return;
    }
    const QColor bg = themeColor(ui::ColorKey::MainWindowBackground, window->palette().color(QPalette::Window));
    const QColor border = themeColor(ui::ColorKey::SeparatorColor, window->palette().color(QPalette::Mid));
    const BOOL dark = bg.lightness() < 128 ? TRUE : FALSE;
    constexpr DWORD kDwmUseImmersiveDarkMode = 20;
    constexpr DWORD kDwmWindowCornerPreference = 33;
    constexpr DWORD kDwmBorderColor = 34;
    const DWORD cornerPref = 2U; // DWMWCP_ROUND = 2
    const COLORREF borderRef = RGB(border.red(), border.green(), border.blue());
    static_cast<void>(setAttr(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark)));
    static_cast<void>(setAttr(hwnd, kDwmWindowCornerPreference, &cornerPref, sizeof(cornerPref)));
    static_cast<void>(setAttr(hwnd, kDwmBorderColor, &borderRef, sizeof(borderRef)));
}
#endif

// Rounded frosted glass card container for WelcomeScreen list boxes ("Projects, Customize, Keymap"
// and "Recent Solutions & Projects") so list boxes have smooth rounded corners and a frosted glass interior.
class FrostedListBoxCard final : public QFrame {
public:
    explicit FrostedListBoxCard(QWidget* parent = nullptr, qreal radius = 10.0) : QFrame(parent), m_radius(radius) {
        setFrameShape(QFrame::NoFrame);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
    }

    static void makeListTransparent(QListWidget* list) {
        if (list == nullptr) {
            return;
        }
        list->setFrameShape(QFrame::NoFrame);
        list->setAutoFillBackground(false);
        QPalette pal = list->palette();
        pal.setColor(QPalette::Base, Qt::transparent);
        pal.setColor(QPalette::Window, Qt::transparent);
        list->setPalette(pal);
        if (QWidget* vp = list->viewport()) {
            vp->setAutoFillBackground(false);
            vp->setPalette(pal);
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const QColor winBg = themeColor(ui::ColorKey::MainWindowBackground, palette().color(QPalette::Window));
        const QColor sep = themeColor(ui::ColorKey::SeparatorColor, palette().color(QPalette::Mid));
        const QColor accent = themeColor(ui::ColorKey::TabbedPaneUnderlineColor, QColor(0x35, 0x74, 0xF0));
        const bool dark = winBg.lightness() < 128;

        QPainterPath clipPath;
        clipPath.addRoundedRect(r, m_radius, m_radius);
        painter.save();
        painter.setClipPath(clipPath);

        // Translucent frosted glass card fill
        QColor cardFill = dark ? winBg.darker(118) : winBg.lighter(106);
        cardFill.setAlpha(dark ? 185 : 210);
        painter.fillPath(clipPath, cardFill);

        // Ambient color spill inside the rounded list box card
        const qreal w = std::max<qreal>(1.0, r.width());
        const qreal h = std::max<qreal>(1.0, r.height());
        paintLauncherEllipticalGlow(&painter, QPointF(r.left() + w * 0.22, r.top() + h * 0.18), w * 0.65,
                                    std::max<qreal>(70.0, h * 0.65), accent, dark ? 24 : 12);
        paintLauncherEllipticalGlow(&painter, QPointF(r.right() - w * 0.18, r.bottom() - h * 0.15), w * 0.55,
                                    std::max<qreal>(60.0, h * 0.55), QColor(0x7C, 0x4D, 0xFF), dark ? 18 : 9);
        painter.restore();

        // Outer rounded border + inner specular frosted glass rim
        QColor outerBorder = sep;
        outerBorder.setAlpha(dark ? 190 : 160);
        painter.setPen(QPen(outerBorder, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(r, m_radius, m_radius);

        QColor innerRim = dark ? QColor(255, 255, 255, 16) : QColor(255, 255, 255, 110);
        painter.setPen(QPen(innerRim, 1.0));
        painter.drawRoundedRect(r.adjusted(1.0, 1.0, -1.0, -1.0), std::max<qreal>(1.0, m_radius - 1.0),
                                std::max<qreal>(1.0, m_radius - 1.0));
    }

private:
    qreal m_radius = 10.0;
};

// Left navigation list delegate ("Projects", "Customize", "Keymap Reference") with rounded frosted pills.
class WelcomeNavItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return QSize(option.rect.width(), std::max(34, option.fontMetrics.height() + 14));
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QRectF pill = QRectF(option.rect).adjusted(3.0, 2.0, -3.0, -2.0);
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
        const QColor winBg = themeColor(ui::ColorKey::MainWindowBackground, option.palette.color(QPalette::Window));
        const bool dark = winBg.lightness() < 128;
        const QColor accent = themeColor(ui::ColorKey::TabbedPaneUnderlineColor, QColor(0x35, 0x74, 0xF0));

        if (selected || hovered) {
            QLinearGradient grad(pill.topLeft(), pill.topRight());
            if (selected) {
                QColor c0 = accent;
                c0.setAlpha(dark ? 78 : 48);
                QColor c1(0x21, 0xD4, 0xFD);
                c1.setAlpha(dark ? 38 : 24);
                grad.setColorAt(0.0, c0);
                grad.setColorAt(1.0, c1);
            } else {
                QColor hCol = themeColor(ui::ColorKey::ListHoverBackground, option.palette.color(QPalette::Midlight));
                hCol.setAlpha(dark ? 115 : 85);
                grad.setColorAt(0.0, hCol);
                grad.setColorAt(1.0, hCol);
            }
            painter->setPen(Qt::NoPen);
            painter->setBrush(grad);
            painter->drawRoundedRect(pill, 7.0, 7.0);

            if (selected) {
                QColor rim = accent.lighter(135);
                rim.setAlpha(dark ? 95 : 70);
                painter->setPen(QPen(rim, 1.0));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(pill.adjusted(0.5, 0.5, -0.5, -0.5), 6.5, 6.5);

                // Glowing vertical pill indicator on the left edge
                const QRectF bar(pill.left() + 4.0, pill.top() + 7.0, 3.0, std::max<qreal>(8.0, pill.height() - 14.0));
                painter->setPen(Qt::NoPen);
                painter->setBrush(QColor(0x21, 0xD4, 0xFD));
                painter->drawRoundedRect(bar, 1.5, 1.5);
            }
        }

        QFont f = option.font;
        f.setWeight(selected ? QFont::DemiBold : QFont::Medium);
        painter->setFont(f);
        const QColor textCol =
            selected
                ? themeColor(ui::ColorKey::ListSelectionForeground, option.palette.color(QPalette::HighlightedText))
                : themeColor(ui::ColorKey::LabelForeground, option.palette.color(QPalette::Text));
        painter->setPen(textCol);
        const QRectF textRect = pill.adjusted(14.0, 0.0, -8.0, 0.0);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                          index.data(Qt::DisplayRole).toString());
        painter->restore();
    }
};

// Frosted glass sidebar pane for WelcomeScreen's left navigation column.
class WelcomeSidebarPane final : public QFrame {
public:
    explicit WelcomeSidebarPane(QWidget* parent = nullptr) : QFrame(parent) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor baseBg = themeColor(ui::ColorKey::ToolWindowBackground, palette().color(QPalette::Window));
        const bool dark = baseBg.lightness() < 128;
        painter.fillRect(rect(), baseBg);

        const qreal w = std::max(1, width());
        const qreal h = std::max(1, height());
        // Glacial cyan + sapphire ambient spill behind the FrostIDE logo & nav list
        paintLauncherEllipticalGlow(&painter, QPointF(42.0, 44.0), w * 0.95, 165.0, QColor(0x21, 0xD4, 0xFD),
                                    dark ? 30 : 16);
        paintLauncherEllipticalGlow(&painter, QPointF(w * 0.45, 145.0), w * 0.95, 190.0, QColor(0x35, 0x74, 0xF0),
                                    dark ? 24 : 13);
        // Subtle violet glow near bottom action buttons
        paintLauncherEllipticalGlow(&painter, QPointF(w * 0.35, h - 70.0), w * 0.90, 160.0, QColor(0x7C, 0x4D, 0xFF),
                                    dark ? 20 : 10);
    }
};

// Frosted glass content stack for WelcomeScreen's right pane.
class WelcomeContentStack final : public QStackedWidget {
public:
    explicit WelcomeContentStack(QWidget* parent = nullptr) : QStackedWidget(parent) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor baseBg = themeColor(ui::ColorKey::MainWindowBackground, palette().color(QPalette::Window));
        const bool dark = baseBg.lightness() < 128;
        painter.fillRect(rect(), baseBg);

        const qreal w = std::max(1, width());
        const qreal h = std::max(1, height());
        const QColor accent = themeColor(ui::ColorKey::TabbedPaneUnderlineColor, QColor(0x35, 0x74, 0xF0));
        // Ambient sapphire/cyan spill across top toolbar ("New Solution...", "Open...", "Clone Repository...")
        paintLauncherEllipticalGlow(&painter, QPointF(w * 0.58, 36.0), w * 0.55, 160.0, accent, dark ? 28 : 15);
        paintLauncherEllipticalGlow(&painter, QPointF(w * 0.20, 42.0), w * 0.38, 130.0, QColor(0x21, 0xD4, 0xFD),
                                    dark ? 18 : 10);
        // Soft violet/indigo ambient glow at bottom-right
        paintLauncherEllipticalGlow(&painter, QPointF(w * 0.82, h * 0.86), w * 0.50, h * 0.45, QColor(0x7C, 0x4D, 0xFF),
                                    dark ? 22 : 11);
    }
};

QString projectTypeBadge(const QFileInfo& info) {
    const QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("sln"))
        return QStringLiteral("SLN");
    if (suffix == QStringLiteral("slnx"))
        return QStringLiteral("SLNX");
    if (suffix == QStringLiteral("csproj"))
        return QStringLiteral("C#");
    if (suffix == QStringLiteral("vcxproj"))
        return QStringLiteral("C++");
    if (info.fileName().compare(QStringLiteral("CMakeLists.txt"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("CMake");
    return QStringLiteral("PROJECT");
}

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

QColor projectAvatarColor(const QString& name) {
    static constexpr QColor kPalette[] = {
        QColor(0x35, 0x74, 0xF0), QColor(0x83, 0x4D, 0xF0), QColor(0x20, 0x8A, 0x5B),
        QColor(0xC9, 0x62, 0x2B), QColor(0x2A, 0x7B, 0x9B), QColor(0xB8, 0x3E, 0x65),
    };
    std::uint32_t hash = 2166136261U;
    for (QChar ch : name) {
        hash ^= static_cast<std::uint32_t>(ch.unicode());
        hash *= 16777619U;
    }
    return kPalette[hash % 6];
}

QIcon makeProjectAvatarIcon(const QString& name) {
    const QColor color = projectAvatarColor(name);
    QPixmap pix(32, 32);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(2.0, 2.0, 28.0, 28.0), 7.0, 7.0);
    // Subtle inner specular rim on project avatar
    p.setPen(QPen(QColor(255, 255, 255, 45), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(2.5, 2.5, 27.0, 27.0), 6.5, 6.5);
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
        const int h = std::max(52, option.fontMetrics.height() * 2 + 18);
        return QSize(option.rect.width(), h);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QRectF cardRect = QRectF(option.rect).adjusted(3.0, 2.0, -3.0, -2.0);
        const QRect rect = cardRect.toRect();
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
        const QColor winBg = themeColor(ui::ColorKey::MainWindowBackground, option.palette.color(QPalette::Window));
        const bool dark = winBg.lightness() < 128;

        QString title = index.data(kTitleRole).toString();
        QString subtitle = index.data(kSubtitleRole).toString();
        if (title.isEmpty()) {
            const QString combined = index.data(Qt::DisplayRole).toString();
            const qsizetype nl = combined.indexOf(u'\n');
            title = nl >= 0 ? combined.left(nl) : combined;
            subtitle = nl >= 0 ? combined.mid(nl + 1) : QString();
        }
        QColor avatarColor = qvariant_cast<QColor>(index.data(kAvatarColorRole));
        if (!avatarColor.isValid()) {
            avatarColor = projectAvatarColor(title);
        }

        // Rounded frosted glass project row card with per-project avatar color spill
        QPainterPath rowClip;
        rowClip.addRoundedRect(cardRect, 8.0, 8.0);
        painter->save();
        painter->setClipPath(rowClip);
        if (selected || hovered) {
            QColor fill = option.palette.color(QPalette::Highlight);
            fill.setAlpha(selected ? (dark ? 58 : 42) : (dark ? 32 : 22));
            painter->fillPath(rowClip, fill);
            paintLauncherEllipticalGlow(painter, QPointF(cardRect.left() + 28.0, cardRect.center().y()),
                                        cardRect.width() * 0.48, cardRect.height() * 1.9, avatarColor,
                                        selected ? (dark ? 54 : 32) : (dark ? 36 : 20));
        } else {
            QColor restFill = dark ? QColor(255, 255, 255, 7) : QColor(0, 0, 0, 6);
            painter->fillPath(rowClip, restFill);
            paintLauncherEllipticalGlow(painter, QPointF(cardRect.left() + 26.0, cardRect.center().y()),
                                        cardRect.width() * 0.32, cardRect.height() * 1.6, avatarColor, dark ? 20 : 10);
        }
        painter->restore();

        QColor cardBorder = selected ? avatarColor.lighter(130)
                                     : (dark ? QColor(255, 255, 255, hovered ? 32 : 14) : QColor(0, 0, 0, 22));
        if (selected) {
            cardBorder.setAlpha(dark ? 115 : 85);
        }
        painter->setPen(QPen(cardBorder, 1.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(cardRect, 8.0, 8.0);

        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const QRect iconRect(rect.left() + 10, rect.top() + (rect.height() - 32) / 2, 32, 32);
        icon.paint(painter, iconRect, Qt::AlignCenter);

        const int textLeft = iconRect.right() + 12;
        const QString badge = index.data(kBadgeRole).toString();
        const int badgeSpace = badge.isEmpty() ? 0 : 64;
        const int textWidth = std::max(40, rect.right() - textLeft - 12 - badgeSpace);
        QFont titleFont = option.font;
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(option.palette.color(QPalette::Text));
        const QFontMetrics titleFm(titleFont);
        const QRect titleRect(textLeft, rect.top() + 7, textWidth, titleFm.height());
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          titleFm.elidedText(title, Qt::ElideRight, textWidth));

        if (!badge.isEmpty()) {
            QFont badgeFont = option.font;
            if (badgeFont.pointSizeF() > 7.5)
                badgeFont.setPointSizeF(badgeFont.pointSizeF() * 0.82);
            badgeFont.setBold(true);
            painter->setFont(badgeFont);
            const QFontMetrics badgeFm(badgeFont);
            const int bw = std::max(38, badgeFm.horizontalAdvance(badge) + 16);
            const int bh = std::max(20, badgeFm.height() + 4);
            const QRectF badgeRect(rect.right() - bw - 10, rect.top() + (rect.height() - bh) / 2.0, bw, bh);
            QColor pillFill = avatarColor;
            pillFill.setAlpha(dark ? 48 : 34);
            painter->setPen(
                QPen(QColor(avatarColor.red(), avatarColor.green(), avatarColor.blue(), dark ? 95 : 70), 1.0));
            painter->setBrush(pillFill);
            painter->drawRoundedRect(badgeRect, 5.0, 5.0);
            painter->setPen(option.palette.color(QPalette::Text));
            painter->drawText(badgeRect, Qt::AlignCenter, badge);
        }

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

// Custom vector-drawn caption button for LauncherWindow ("Minimize" and "Close").
class LauncherCaptionButton final : public QToolButton {
public:
    enum class Kind { Minimize, Close };

    explicit LauncherCaptionButton(Kind kind, QWidget* parent = nullptr) : QToolButton(parent), m_kind(kind) {
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::ArrowCursor);
        setFixedSize(42, 34);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool hovered = underMouse();
        const bool pressed = isDown();
        if (hovered || pressed) {
            QColor bg;
            if (m_kind == Kind::Close) {
                bg = pressed ? QColor(0xB2, 0x3C, 0x39) : QColor(0xC7, 0x54, 0x50);
            } else {
                bg = themeColor(pressed ? ui::ColorKey::ButtonPressedBackground : ui::ColorKey::ButtonHoverBackground,
                                palette().color(QPalette::Midlight));
            }
            painter.fillRect(rect(), bg);
        }

        QColor fg = (m_kind == Kind::Close && (hovered || pressed))
                        ? QColor(0xFF, 0xFF, 0xFF)
                        : themeColor(ui::ColorKey::LabelForeground, palette().color(QPalette::WindowText));
        const QPointF c = QRectF(rect()).center();
        painter.setPen(QPen(fg, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        if (m_kind == Kind::Minimize) {
            painter.drawLine(QPointF(c.x() - 5.0, c.y() + 0.5), QPointF(c.x() + 5.0, c.y() + 0.5));
        } else {
            painter.drawLine(QPointF(c.x() - 4.5, c.y() - 4.5), QPointF(c.x() + 4.5, c.y() + 4.5));
            painter.drawLine(QPointF(c.x() + 4.5, c.y() - 4.5), QPointF(c.x() - 4.5, c.y() + 4.5));
        }
    }

private:
    Kind m_kind;
};

// Custom-painted frosted glass header bar for LauncherWindow.
class LauncherTitleBar final : public QWidget {
public:
    explicit LauncherTitleBar(QWidget* parent = nullptr) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor bg = themeColor(ui::ColorKey::ToolWindowBackground, palette().color(QPalette::Window));
        const QColor sep = themeColor(ui::ColorKey::SeparatorColor, palette().color(QPalette::Mid));
        const bool dark = bg.lightness() < 128;
        painter.fillRect(rect(), bg);

        const qreal w = std::max(1, width());
        const qreal h = std::max(1, height());
        paintLauncherEllipticalGlow(&painter, QPointF(32.0, h * 0.5), w * 0.35, h * 3.0, QColor(0x21, 0xD4, 0xFD),
                                    dark ? 26 : 14);
        paintLauncherEllipticalGlow(&painter, QPointF(w * 0.48, -h * 0.2), w * 0.45, h * 2.6, QColor(0x35, 0x74, 0xF0),
                                    dark ? 18 : 10);
        painter.fillRect(QRect(0, 0, width(), 1), dark ? QColor(255, 255, 255, 16) : QColor(255, 255, 255, 110));
        painter.fillRect(QRect(0, height() - 1, width(), 1), sep);
    }
};

// Custom-painted outer frame for LauncherWindow with rounded corners and frosted glass border.
class LauncherBorderFrame final : public QFrame {
public:
    explicit LauncherBorderFrame(QWidget* parent = nullptr) : QFrame(parent) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor bg = themeColor(ui::ColorKey::MainWindowBackground, palette().color(QPalette::Window));
        const QColor sep = themeColor(ui::ColorKey::SeparatorColor, palette().color(QPalette::Mid));
        painter.fillRect(rect(), bg);
        painter.setPen(QPen(sep, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8.0, 8.0);
    }
};

} // namespace

WelcomeScreen::WelcomeScreen(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("welcomeScreen"));
    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // --- Left Sidebar (Rider 2-Pane Welcome Navigation with Frosted Glass Ambient Spill) ---
    auto* sidebar = new WelcomeSidebarPane(this);
    sidebar->setObjectName(QStringLiteral("welcomeSidebar"));
    sidebar->setFixedWidth(232);
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
    auto* versionLabel = new QLabel(QString::fromUtf8(build_info::kMajorVersion), sidebar);
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

    // Rounded Frosted Glass Card for "Projects, Customize, Keymap Reference" navigation list box
    auto* navCard = new FrostedListBoxCard(sidebar, 10.0);
    navCard->setObjectName(QStringLiteral("welcomeNavCard"));
    auto* navCardLayout = new QVBoxLayout(navCard);
    navCardLayout->setContentsMargins(5, 5, 5, 5);
    navCardLayout->setSpacing(0);

    m_navList = new QListWidget(navCard);
    m_navList->setObjectName(QStringLiteral("welcomeNavList"));
    FrostedListBoxCard::makeListTransparent(m_navList);
    m_navList->setSpacing(2);
    m_navList->setItemDelegate(new WelcomeNavItemDelegate(m_navList));
    m_navList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navList->setFixedHeight(112);
    m_navList->viewport()->setAttribute(Qt::WA_Hover, true);
    for (const QString& navLabel : {text("Projects"), text("Customize"), text("Keymap Reference")}) {
        auto* navItem = new QListWidgetItem(navLabel, m_navList);
        navItem->setSizeHint(QSize(180, 34));
    }
    m_navList->setCurrentRow(0);
    navCardLayout->addWidget(m_navList);
    sidebarLayout->addWidget(navCard);
    sidebarLayout->addStretch(1);

    m_updateStatus = new QLabel(text("FrostIDE %1 • Ready").arg(QString::fromUtf8(build_info::kVersion)), sidebar);
    m_updateStatus->setObjectName(QStringLiteral("welcomeUpdateStatus"));
    m_updateStatus->setWordWrap(true);
    m_updateStatus->setFont(verFont);
    sidebarLayout->addWidget(m_updateStatus);

    m_checkUpdates = new QPushButton(text("Check for Updates..."), sidebar);
    m_checkUpdates->setObjectName(QStringLiteral("welcomeCheckUpdatesButton"));
    m_checkUpdates->setMinimumHeight(30);
    connect(m_checkUpdates, &QPushButton::clicked, this, &WelcomeScreen::checkForUpdatesRequested);
    sidebarLayout->addWidget(m_checkUpdates);

    m_switchVersion = new QPushButton(text("Switch IDE Version..."), sidebar);
    m_switchVersion->setObjectName(QStringLiteral("welcomeSwitchVersionButton"));
    m_switchVersion->setMinimumHeight(30);
    m_switchVersion->setToolTip(text("Switch between installed FrostIDE builds or apply a downloaded update"));
    connect(m_switchVersion, &QPushButton::clicked, this, &WelcomeScreen::switchVersionRequested);
    sidebarLayout->addWidget(m_switchVersion);

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

    // --- Right Content Stack (Frosted Glass Ambient Color Spill) ---
    m_pages = new WelcomeContentStack(this);
    m_pages->setObjectName(QStringLiteral("welcomeStack"));

    // Page 0: Projects Page
    auto* projectsPage = new QWidget(m_pages);
    auto* projectsLayout = new QVBoxLayout(projectsPage);
    projectsLayout->setContentsMargins(28, 22, 28, 22);
    projectsLayout->setSpacing(12);

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
    m_cloneRepo = new QPushButton(text("Clone Repository..."), projectsPage);
    m_cloneRepo->setObjectName(QStringLiteral("welcomeCloneRepo"));
    m_cloneRepo->setMinimumHeight(32);
    m_cloneRepo->setToolTip(text("Clone a Git repository from a URL and open it"));
    m_openFolder = new QPushButton(text("Open Folder..."), projectsPage);
    m_openFolder->setObjectName(QStringLiteral("welcomeOpenFolder"));
    m_openFolder->setMinimumHeight(32);
    m_openFolder->setToolTip(text("Open a project folder, including a CMake project"));
    if (const ui::FrostStyle* style = ui::FrostStyle::current(); style != nullptr) {
        if (const QIcon searchIcon = style->icon(ui::icons::kActionsSearch); !searchIcon.isNull()) {
            m_search->addAction(searchIcon, QLineEdit::LeadingPosition);
        }
        m_newProject->setIcon(style->icon(ui::icons::kActionsAdd));
        m_open->setIcon(style->icon(ui::icons::kNodesFolderOpen));
        m_cloneRepo->setIcon(style->icon(ui::icons::kVcsBranch));
        m_openFolder->setIcon(style->icon(ui::icons::kNodesFolder));
    }
    headerRow->addWidget(m_newProject);
    headerRow->addWidget(m_open);
    headerRow->addWidget(m_cloneRepo);
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

    // Rounded Frosted Glass Card for "Recent Solutions & Projects" list box
    auto* recentCard = new FrostedListBoxCard(projectsPage, 10.0);
    recentCard->setObjectName(QStringLiteral("welcomeRecentCard"));
    auto* recentCardLayout = new QVBoxLayout(recentCard);
    recentCardLayout->setContentsMargins(6, 6, 6, 6);
    recentCardLayout->setSpacing(0);

    m_recent = new QListWidget(recentCard);
    m_recent->setObjectName(QStringLiteral("welcomeRecent"));
    FrostedListBoxCard::makeListTransparent(m_recent);
    m_recent->setIconSize(QSize(32, 32));
    m_recent->setSpacing(3);
    m_recent->setItemDelegate(new RecentProjectItemDelegate(m_recent));
    m_recent->setContextMenuPolicy(Qt::CustomContextMenu);
    m_recent->setSelectionMode(QAbstractItemView::SingleSelection);
    m_recent->viewport()->setAttribute(Qt::WA_Hover, true);
    m_recent->installEventFilter(this);
    recentCardLayout->addWidget(m_recent, 1);
    projectsLayout->addWidget(recentCard, 1);
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
        new QLabel(text("Choose from 34 built-in color themes including Rider Dark, Rider Light, Rider Islands Dark, "
                        "Rider Islands Light, Darcula, Monokai Pro, Dracula, Nord, One Dark, Solarized, and Glacier."),
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

    auto* keymapCard = new FrostedListBoxCard(keymapPage, 10.0);
    auto* keymapCardLayout = new QVBoxLayout(keymapCard);
    keymapCardLayout->setContentsMargins(18, 14, 18, 14);
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
        auto* actLbl = new QLabel(text(entry.action), keymapCard);
        auto* keyLbl = new QLabel(QString::fromUtf8(entry.shortcut), keymapCard);
        QFont keyFont = keyLbl->font();
        keyFont.setBold(true);
        keyLbl->setFont(keyFont);
        shortcutGrid->addWidget(actLbl, gridRow, 0);
        shortcutGrid->addWidget(keyLbl, gridRow, 1);
        ++gridRow;
    }
    keymapCardLayout->addLayout(shortcutGrid);
    keymapLayout->addWidget(keymapCard);
    keymapLayout->addStretch(1);
    m_pages->addWidget(keymapPage);

    rootLayout->addWidget(m_pages, 1);

    connect(m_navList, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_search, &QLineEdit::textChanged, this, &WelcomeScreen::filterRecentProjects);
    connect(m_newProject, &QPushButton::clicked, this, &WelcomeScreen::newProjectRequested);
    connect(m_open, &QPushButton::clicked, this, &WelcomeScreen::openRequested);
    connect(m_cloneRepo, &QPushButton::clicked, this, &WelcomeScreen::cloneRepositoryRequested);
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
        item->setData(kBadgeRole, projectTypeBadge(info));
        item->setData(kAvatarColorRole, projectAvatarColor(name));
        item->setToolTip(native);
    }
    if (m_search != nullptr && !m_search->text().isEmpty()) {
        filterRecentProjects(m_search->text());
    }
}

void WelcomeScreen::setUpdateStatus(const QString& status) {
    if (m_updateStatus != nullptr)
        m_updateStatus->setText(status);
}

QString WelcomeScreen::updateStatus() const { return m_updateStatus != nullptr ? m_updateStatus->text() : QString(); }

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
    QAction* openFolder = menu.addAction(text("Open Containing Folder"));
    QAction* copyPath = menu.addAction(text("Copy Path"));
    menu.addSeparator();
    QAction* remove = menu.addAction(text("Remove from Recent Projects"));
    QAction* chosen = menu.exec(m_recent->viewport()->mapToGlobal(position));
    if (chosen == open) {
        activateRow(row);
    } else if (chosen == openFolder) {
        if (QListWidgetItem* item = m_recent->item(row); item != nullptr) {
            const QFileInfo info(item->data(kPathRole).toString());
            const QString folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        }
    } else if (chosen == copyPath) {
        if (QListWidgetItem* item = m_recent->item(row); item != nullptr) {
            QApplication::clipboard()->setText(QDir::toNativeSeparators(item->data(kPathRole).toString()));
        }
    } else if (chosen == remove) {
        removeRow(row);
    }
}

LauncherWindow::LauncherWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setObjectName(QStringLiteral("FrostLauncherWindow"));
    setWindowTitle(QStringLiteral("FrostIDE"));
    setWindowIcon(ui::glacialLogoIcon());
    setMinimumSize(740, 500);
    resize(860, 570);
    installEventFilter(this);

#ifdef _WIN32
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint |
                       Qt::WindowCloseButtonHint);
    }
#endif

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    auto* borderFrame = new LauncherBorderFrame(this);
    borderFrame->setObjectName(QStringLiteral("launcherBorderFrame"));
    auto* frameLayout = new QVBoxLayout(borderFrame);
    frameLayout->setContentsMargins(1, 1, 1, 1);
    frameLayout->setSpacing(0);

    m_titleBar = new LauncherTitleBar(borderFrame);
    m_titleBar->setObjectName(QStringLiteral("launcherTitleBar"));
    m_titleBar->setFixedHeight(34);
    m_titleBar->installEventFilter(this);

    auto* titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(12, 0, 0, 0);
    titleLayout->setSpacing(8);

    auto* iconLabel = new QLabel(m_titleBar);
    iconLabel->setPixmap(ui::glacialLogoPixmap(18, 2.0));
    iconLabel->setFixedSize(18, 18);
    iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    auto* titleLabel = new QLabel(text("Welcome to FrostIDE"), m_titleBar);
    QFont headerFont = titleLabel->font();
    headerFont.setWeight(QFont::DemiBold);
    titleLabel->setFont(headerFont);
    titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    titleLayout->addWidget(iconLabel);
    titleLayout->addWidget(titleLabel);
    titleLayout->addStretch(1);

    auto* minBtn = new LauncherCaptionButton(LauncherCaptionButton::Kind::Minimize, m_titleBar);
    minBtn->setObjectName(QStringLiteral("launcherMinimizeButton"));
    minBtn->setToolTip(text("Minimize"));
    connect(minBtn, &QToolButton::clicked, this, &QWidget::showMinimized);

    auto* closeBtn = new LauncherCaptionButton(LauncherCaptionButton::Kind::Close, m_titleBar);
    closeBtn->setObjectName(QStringLiteral("launcherCloseButton"));
    closeBtn->setToolTip(text("Close"));
    connect(closeBtn, &QToolButton::clicked, this, &QWidget::close);

    titleLayout->addWidget(minBtn);
    titleLayout->addWidget(closeBtn);

    m_welcome = new WelcomeScreen(borderFrame);
    frameLayout->addWidget(m_titleBar);
    frameLayout->addWidget(m_welcome, 1);
    outerLayout->addWidget(borderFrame, 1);
}

LauncherWindow::~LauncherWindow() = default;

void LauncherWindow::closeEvent(QCloseEvent* event) {
    QWidget::closeEvent(event);
    if (!m_transitioningToIde) {
        emit closedByUser();
    }
}

bool LauncherWindow::eventFilter(QObject* watched, QEvent* event) {
#ifdef _WIN32
    if (watched == this && (event->type() == QEvent::Show || event->type() == QEvent::PaletteChange ||
                            event->type() == QEvent::StyleChange)) {
        syncWin32LauncherDarkFrame(this);
    }
#endif
    if (watched == m_titleBar && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            if (QWindow* wh = windowHandle(); wh != nullptr) {
                wh->startSystemMove();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace frost::app
