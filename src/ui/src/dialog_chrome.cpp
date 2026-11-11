// SPDX-License-Identifier: Apache-2.0
//
// Custom themed Rider-style window header and frame chrome for FrostIDE sub-windows and dialogs
// (frost/ui/dialog_chrome.hpp).

#include "frost/ui/dialog_chrome.hpp"

#include "frost/ui/frost_style.hpp"
#include "frost/ui/glacial_logo.hpp"

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QProgressDialog>
#include <QRadialGradient>
#include <QToolButton>
#include <QWindow>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>

namespace frost::ui {

namespace {

int scaledPx(int logical) {
    if (const FrostStyle* style = FrostStyle::current()) {
        return style->scale().px(logical);
    }
    return logical;
}

QColor themeColor(ColorKey key, const QColor& fallback) {
    if (const FrostStyle* style = FrostStyle::current()) {
        return style->color(key);
    }
    return fallback;
}

void paintDialogEllipticalGlow(QPainter& painter, QPointF center, qreal radiusX, qreal radiusY, QColor color,
                               int peakAlpha) {
    if (radiusX <= 1.0 || radiusY <= 1.0 || peakAlpha <= 0) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(center);
    painter.scale(radiusX, radiusY);

    QRadialGradient grad(QPointF(0.0, 0.0), 1.0);
    const auto withAlpha = [&](qreal factor) {
        QColor c = color;
        c.setAlpha(std::clamp(static_cast<int>(peakAlpha * factor), 0, 255));
        return c;
    };
    grad.setColorAt(0.00, withAlpha(1.00));
    grad.setColorAt(0.25, withAlpha(0.78));
    grad.setColorAt(0.50, withAlpha(0.45));
    grad.setColorAt(0.75, withAlpha(0.16));
    grad.setColorAt(1.00, withAlpha(0.00));

    painter.setPen(Qt::NoPen);
    painter.setBrush(grad);
    painter.drawRect(QRectF(-1.0, -1.0, 2.0, 2.0));
    painter.restore();
}

enum class CaptionButtonKind { Minimize, MaximizeRestore, Close };

class DialogCaptionButton final : public QToolButton {
public:
    DialogCaptionButton(CaptionButtonKind kind, QDialog* dialog, QWidget* parent)
        : QToolButton(parent), m_kind(kind), m_dialog(dialog) {
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover, true);
        setFixedSize(scaledPx(40), DialogTitleBar::preferredHeight());
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);

        const bool hovered = underMouse() && isEnabled();
        const bool pressed = isDown() && isEnabled();
        if (m_kind == CaptionButtonKind::Close) {
            if (pressed) {
                painter.fillRect(rect(), QColor(0xC5, 0x0F, 0x1F));
            } else if (hovered) {
                painter.fillRect(rect(), QColor(0xE8, 0x11, 0x23));
            }
        } else if (pressed || hovered) {
            const QColor hoverBg =
                themeColor(pressed ? ColorKey::ButtonPressedBackground : ColorKey::ButtonHoverBackground,
                           QColor(255, 255, 255, 28));
            painter.fillRect(rect(), hoverBg);
        }

        QColor iconColor = (m_kind == CaptionButtonKind::Close && (hovered || pressed))
                               ? QColor(0xFF, 0xFF, 0xFF)
                               : themeColor(ColorKey::LabelForeground, palette().color(QPalette::WindowText));
        if (m_dialog != nullptr && !m_dialog->isActiveWindow() && !hovered && !pressed) {
            if (const FrostStyle* style = FrostStyle::current()) {
                iconColor.setAlphaF(iconColor.alphaF() * style->theme().metric(MetricKey::WindowInactiveAlpha));
            }
        }

        const int cx = width() / 2;
        const int cy = height() / 2;
        painter.setPen(QPen(iconColor, 1));
        painter.setBrush(Qt::NoBrush);

        if (m_kind == CaptionButtonKind::Minimize) {
            painter.drawLine(cx - 5, cy, cx + 5, cy);
        } else if (m_kind == CaptionButtonKind::MaximizeRestore) {
            const bool maximized = m_dialog != nullptr && m_dialog->isMaximized();
            if (maximized) {
                painter.drawRect(cx - 3, cy - 5, 8, 8);
                painter.fillRect(cx - 5, cy - 3, 8, 8,
                                 themeColor(ColorKey::MainWindowBackground, palette().color(QPalette::Window)));
                painter.drawRect(cx - 5, cy - 3, 8, 8);
            } else {
                painter.drawRect(cx - 5, cy - 5, 9, 9);
            }
        } else {
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(iconColor, 1.35, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(cx - 4.5, cy - 4.5), QPointF(cx + 4.5, cy + 4.5));
            painter.drawLine(QPointF(cx + 4.5, cy - 4.5), QPointF(cx - 4.5, cy + 4.5));
        }
    }

private:
    CaptionButtonKind m_kind;
    QPointer<QDialog> m_dialog;
};

#ifdef _WIN32
void syncWin32DialogDarkFrame(QDialog* dialog) {
    if (dialog == nullptr || QGuiApplication::platformName() != QStringLiteral("windows") ||
        !dialog->testAttribute(Qt::WA_WState_Created)) {
        return;
    }
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    if (dwm == nullptr) {
        dwm = LoadLibraryW(L"dwmapi.dll");
    }
    if (dwm == nullptr) {
        return;
    }
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    const auto setAttr = reinterpret_cast<DwmSetWindowAttributeFn>(
        reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
    if (setAttr == nullptr) {
        return;
    }
    const HWND hwnd = reinterpret_cast<HWND>(dialog->winId());
    if (hwnd == nullptr) {
        return;
    }
    const QColor bg = themeColor(ColorKey::PanelBackground, dialog->palette().color(QPalette::Window));
    const QColor border = themeColor(ColorKey::PopupBorderColor, dialog->palette().color(QPalette::Mid));
    const BOOL dark = bg.lightness() < 128 ? TRUE : FALSE;
    constexpr DWORD kDwmUseImmersiveDarkMode = 20;
    constexpr DWORD kDwmWindowCornerPreference = 33;
    constexpr DWORD kDwmBorderColor = 34;
    const DWORD cornerPref = dialog->isMaximized() ? 1U : 2U; // DWMWCP_ROUND = 2
    const COLORREF borderRef = RGB(border.red(), border.green(), border.blue());
    static_cast<void>(setAttr(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark)));
    static_cast<void>(setAttr(hwnd, kDwmWindowCornerPreference, &cornerPref, sizeof(cornerPref)));
    static_cast<void>(setAttr(hwnd, kDwmBorderColor, &borderRef, sizeof(borderRef)));
}
#endif

class DialogChromeFilter final : public QObject {
public:
    DialogChromeFilter(QDialog* dialog, DialogTitleBar* titleBar, bool resizable)
        : QObject(dialog), m_dialog(dialog), m_titleBar(titleBar), m_resizable(resizable) {
        setObjectName(QStringLiteral("dialogChromeFilter"));
        syncGeometry();
    }

    void setResizable(bool resizable) {
        m_resizable = resizable;
        syncGeometry();
    }

    void syncGeometry() {
        if (m_dialog == nullptr || m_titleBar == nullptr || m_updatingMargins) {
            return;
        }
        m_updatingMargins = true;
        const bool maximized = m_dialog->isMaximized();
        const int border = maximized ? 0 : 1;
        const int headerH = DialogTitleBar::preferredHeight();
        m_dialog->setContentsMargins(border, headerH + border, border, border);
        if (!m_expandedFixedDialog &&
            (qobject_cast<QMessageBox*>(m_dialog) != nullptr || qobject_cast<QProgressDialog*>(m_dialog) != nullptr) &&
            m_dialog->height() > 0) {
            m_expandedFixedDialog = true;
            m_dialog->setMinimumHeight(m_dialog->minimumHeight() + headerH);
            m_dialog->resize(std::max(m_dialog->width(), scaledPx(380)), m_dialog->height() + headerH);
        }
        m_titleBar->setGeometry(border, border, std::max(0, m_dialog->width() - border * 2), headerH);
        m_titleBar->syncState();
        m_titleBar->raise();
        m_updatingMargins = false;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != m_dialog || m_dialog == nullptr || event == nullptr) {
            return QObject::eventFilter(watched, event);
        }

        switch (event->type()) {
        case QEvent::Show:
#ifdef _WIN32
            syncWin32DialogDarkFrame(m_dialog);
#endif
            syncGeometry();
            break;
        case QEvent::Resize:
        case QEvent::LayoutRequest:
        case QEvent::StyleChange:
        case QEvent::WindowStateChange:
            syncGeometry();
            m_dialog->update();
            break;
        case QEvent::WindowTitleChange:
        case QEvent::WindowActivate:
        case QEvent::WindowDeactivate:
            if (m_titleBar != nullptr) {
                m_titleBar->update();
            }
            m_dialog->update();
            break;
        case QEvent::Paint: {
            QPainter painter(m_dialog);
            painter.setRenderHint(QPainter::Antialiasing, true);
            const QColor bg = themeColor(ColorKey::PanelBackground, m_dialog->palette().color(QPalette::Window));
            const QColor accent = themeColor(ColorKey::TabbedPaneUnderlineColor, QColor(0x35, 0x74, 0xF0));
            const bool isDark = bg.lightness() < 128;
            painter.fillRect(m_dialog->rect(), bg);

            // Frosted glass ambient color spill across the dialog surface.
            paintDialogEllipticalGlow(painter, QPointF(m_dialog->width() * 0.22, DialogTitleBar::preferredHeight()),
                                      m_dialog->width() * 0.65, std::max(180, m_dialog->height() / 2), accent,
                                      isDark ? 32 : 18);
            paintDialogEllipticalGlow(painter, QPointF(m_dialog->width() * 0.82, m_dialog->height() * 0.85),
                                      m_dialog->width() * 0.55, std::max(160, m_dialog->height() / 2),
                                      QColor(0x7C, 0x4D, 0xFF), isDark ? 22 : 12);

            QLinearGradient sheen(m_dialog->rect().topLeft(), m_dialog->rect().bottomLeft());
            sheen.setColorAt(0.0, isDark ? QColor(255, 255, 255, 7) : QColor(255, 255, 255, 28));
            sheen.setColorAt(0.5, QColor(255, 255, 255, 2));
            sheen.setColorAt(1.0, isDark ? QColor(0, 0, 0, 12) : QColor(0, 0, 0, 6));
            painter.fillRect(m_dialog->rect(), sheen);

            if (!m_dialog->isMaximized()) {
                QColor borderColor = themeColor(ColorKey::PopupBorderColor, m_dialog->palette().color(QPalette::Mid));
                if (!m_dialog->isActiveWindow()) {
                    if (const FrostStyle* style = FrostStyle::current()) {
                        borderColor.setAlphaF(borderColor.alphaF() *
                                              style->theme().metric(MetricKey::WindowInactiveAlpha));
                    }
                }
                painter.setPen(QPen(borderColor, 1));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(QRectF(m_dialog->rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8.0, 8.0);
            }
            break;
        }
        case QEvent::MouseMove: {
            if (m_resizable && !m_dialog->isMaximized()) {
                const auto* mouse = static_cast<const QMouseEvent*>(event);
                const Qt::Edges edges = hitTestEdges(mouse->position().toPoint());
                updateCursorForEdges(edges);
            }
            break;
        }
        case QEvent::MouseButtonPress: {
            if (m_resizable && !m_dialog->isMaximized()) {
                const auto* mouse = static_cast<const QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    const Qt::Edges edges = hitTestEdges(mouse->position().toPoint());
                    if (edges != Qt::Edges{}) {
                        if (QWindow* win = m_dialog->windowHandle()) {
                            if (win->startSystemResize(edges)) {
                                return true;
                            }
                        }
                    }
                }
            }
            break;
        }
        case QEvent::Leave:
            if (m_cursorOverridden) {
                m_dialog->unsetCursor();
                m_cursorOverridden = false;
            }
            break;
        default: break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    [[nodiscard]] Qt::Edges hitTestEdges(const QPoint& pos) const {
        constexpr int kGrip = 5;
        Qt::Edges edges{};
        if (pos.x() >= 0 && pos.x() < kGrip) {
            edges |= Qt::LeftEdge;
        } else if (pos.x() <= m_dialog->width() && pos.x() > m_dialog->width() - kGrip) {
            edges |= Qt::RightEdge;
        }
        if (pos.y() >= 0 && pos.y() < kGrip) {
            edges |= Qt::TopEdge;
        } else if (pos.y() <= m_dialog->height() && pos.y() > m_dialog->height() - kGrip) {
            edges |= Qt::BottomEdge;
        }
        return edges;
    }

    void updateCursorForEdges(Qt::Edges edges) {
        if (edges == Qt::Edges{}) {
            if (m_cursorOverridden) {
                m_dialog->unsetCursor();
                m_cursorOverridden = false;
            }
            return;
        }
        m_cursorOverridden = true;
        if ((edges == (Qt::TopEdge | Qt::LeftEdge)) || (edges == (Qt::BottomEdge | Qt::RightEdge))) {
            m_dialog->setCursor(Qt::SizeFDiagCursor);
        } else if ((edges == (Qt::TopEdge | Qt::RightEdge)) || (edges == (Qt::BottomEdge | Qt::LeftEdge))) {
            m_dialog->setCursor(Qt::SizeBDiagCursor);
        } else if ((edges & (Qt::LeftEdge | Qt::RightEdge)) != 0) {
            m_dialog->setCursor(Qt::SizeHorCursor);
        } else {
            m_dialog->setCursor(Qt::SizeVerCursor);
        }
    }

    QPointer<QDialog> m_dialog;
    QPointer<DialogTitleBar> m_titleBar;
    bool m_resizable = true;
    bool m_updatingMargins = false;
    bool m_cursorOverridden = false;
    bool m_expandedFixedDialog = false;
};

} // namespace

DialogTitleBar::DialogTitleBar(QDialog* dialog, bool resizable)
    : QWidget(dialog), m_dialog(dialog),
      m_minimizeButton(new DialogCaptionButton(CaptionButtonKind::Minimize, dialog, this)),
      m_maximizeButton(new DialogCaptionButton(CaptionButtonKind::MaximizeRestore, dialog, this)),
      m_closeButton(new DialogCaptionButton(CaptionButtonKind::Close, dialog, this)), m_resizable(resizable) {
    setObjectName(QStringLiteral("dialogTitleBar"));
    setFixedHeight(preferredHeight());

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addStretch(1);
    m_minimizeButton->setObjectName(QStringLiteral("dialogMinimizeButton"));
    m_minimizeButton->setToolTip(tr("Minimize"));
    connect(m_minimizeButton, &QToolButton::clicked, this, [this] {
        if (m_dialog != nullptr) {
            m_dialog->showMinimized();
        }
    });
    layout->addWidget(m_minimizeButton);
    m_maximizeButton->setObjectName(QStringLiteral("dialogMaximizeButton"));
    m_maximizeButton->setToolTip(tr("Maximize / Restore"));
    m_maximizeButton->setVisible(m_resizable);
    connect(m_maximizeButton, &QToolButton::clicked, this, [this] {
        if (m_dialog == nullptr || !m_resizable) {
            return;
        }
        if (m_dialog->isMaximized()) {
            m_dialog->showNormal();
        } else {
            m_dialog->showMaximized();
        }
        update();
        m_maximizeButton->update();
    });
    layout->addWidget(m_maximizeButton);
    m_closeButton->setObjectName(QStringLiteral("dialogCloseButton"));
    m_closeButton->setToolTip(tr("Close"));
    connect(m_closeButton, &QToolButton::clicked, this, [this] {
        if (m_dialog != nullptr) {
            m_dialog->reject();
        }
    });
    layout->addWidget(m_closeButton);

    syncState();
}

DialogTitleBar::~DialogTitleBar() = default;

int DialogTitleBar::preferredHeight() { return scaledPx(34); }

void DialogTitleBar::setResizable(bool resizable) {
    m_resizable = resizable;
    syncState();
    update();
}

void DialogTitleBar::syncState() {
    const int h = preferredHeight();
    setFixedHeight(h);
    const int btnW = scaledPx(40);
    if (m_minimizeButton != nullptr) {
        m_minimizeButton->setFixedSize(btnW, h);
        m_minimizeButton->setVisible(m_dialog != nullptr && !m_dialog->isModal());
    }
    if (m_maximizeButton != nullptr) {
        m_maximizeButton->setFixedSize(btnW, h);
        m_maximizeButton->setVisible(m_resizable);
    }
    if (m_closeButton != nullptr) {
        m_closeButton->setFixedSize(btnW, h);
    }
}

void DialogTitleBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor bg = themeColor(ColorKey::MainWindowBackground, palette().color(QPalette::Window));
    const QColor sep = themeColor(ColorKey::SeparatorColor, palette().color(QPalette::Mid));
    const bool dark = bg.lightness() < 128;
    painter.fillRect(rect(), bg);

    // Frosted glass ambient glow across dialog header (glacial cyan at logo + sapphire center)
    const qreal w = std::max(1, width());
    const qreal h = std::max(1, height());
    paintDialogEllipticalGlow(painter, QPointF(scaledPx(28), h * 0.5), w * 0.36, h * 3.0, QColor(0x21, 0xD4, 0xFD),
                              dark ? 22 : 12);
    paintDialogEllipticalGlow(painter, QPointF(w * 0.52, -h * 0.2), w * 0.48, h * 2.6, QColor(0x35, 0x74, 0xF0),
                              dark ? 16 : 9);
    QColor topSpec = dark ? QColor(255, 255, 255, 16) : QColor(255, 255, 255, 110);
    painter.fillRect(QRect(0, 0, width(), 1), topSpec);
    painter.fillRect(QRect(0, height() - 1, width(), 1), sep);

    const int padLeft = scaledPx(10);
    const int logoSide = scaledPx(18);
    const QRectF logoRect(padLeft, (height() - logoSide) * 0.5, logoSide, logoSide);
    paintGlacialLogo(painter, logoRect);

    if (m_dialog == nullptr) {
        return;
    }

    QColor textColor = themeColor(ColorKey::LabelForeground, palette().color(QPalette::WindowText));
    if (!m_dialog->isActiveWindow()) {
        if (const FrostStyle* style = FrostStyle::current()) {
            textColor.setAlphaF(textColor.alphaF() * style->theme().metric(MetricKey::WindowInactiveAlpha));
        }
    }

    int rightButtonsWidth = scaledPx(40);
    if (m_maximizeButton != nullptr && m_maximizeButton->isVisible()) {
        rightButtonsWidth += m_maximizeButton->width();
    }
    if (m_minimizeButton != nullptr && m_minimizeButton->isVisible()) {
        rightButtonsWidth += m_minimizeButton->width();
    }

    const int textLeft = padLeft + logoSide + scaledPx(8);
    const int textRight = std::max(textLeft, width() - rightButtonsWidth - scaledPx(8));
    const QRect titleRect(textLeft, 0, textRight - textLeft, height() - 1);

    QFont titleFont = font();
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(textColor);
    const QFontMetrics fm(titleFont);
    const QString elided = fm.elidedText(m_dialog->windowTitle(), Qt::ElideRight, titleRect.width());
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, elided);
}

void DialogTitleBar::mousePressEvent(QMouseEvent* event) {
    if (event != nullptr && event->button() == Qt::LeftButton && m_dialog != nullptr) {
        if (QWindow* win = m_dialog->windowHandle();
            win != nullptr && QGuiApplication::platformName() == QStringLiteral("windows")) {
            if (win->startSystemMove()) {
                event->accept();
                return;
            }
        }
        if (!m_dialog->isMaximized()) {
            m_dragging = true;
            m_dragOffset = event->globalPosition().toPoint() - m_dialog->frameGeometry().topLeft();
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void DialogTitleBar::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && event != nullptr && (event->buttons() & Qt::LeftButton) != 0 && m_dialog != nullptr &&
        !m_dialog->isMaximized()) {
        m_dialog->move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void DialogTitleBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event != nullptr && event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void DialogTitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event != nullptr && event->button() == Qt::LeftButton && m_dialog != nullptr && m_resizable) {
        if (m_dialog->isMaximized()) {
            m_dialog->showNormal();
        } else {
            m_dialog->showMaximized();
        }
        update();
        if (m_maximizeButton != nullptr) {
            m_maximizeButton->update();
        }
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

DialogTitleBar* installDialogChrome(QDialog* dialog, bool resizable) {
    if (dialog == nullptr) {
        return nullptr;
    }
    if (auto* existing =
            dialog->findChild<DialogTitleBar*>(QStringLiteral("dialogTitleBar"), Qt::FindDirectChildrenOnly)) {
        existing->setResizable(resizable);
        if (auto* filterObj =
                dialog->findChild<QObject*>(QStringLiteral("dialogChromeFilter"), Qt::FindDirectChildrenOnly)) {
            static_cast<DialogChromeFilter*>(filterObj)->setResizable(resizable);
        }
        if (!dialog->isVisible() && resizable) {
            dialog->setWindowFlags(dialog->windowFlags() | Qt::WindowMaximizeButtonHint);
        }
        return existing;
    }

    dialog->setWindowIcon(glacialLogoIcon());
    if (!dialog->isVisible()) {
        Qt::WindowFlags flags = dialog->windowFlags() | Qt::Dialog | Qt::FramelessWindowHint |
                                Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint;
        if (resizable) {
            flags |= Qt::WindowMaximizeButtonHint;
        }
        dialog->setWindowFlags(flags);
    }

    auto* titleBar = new DialogTitleBar(dialog, resizable);
    auto* filter = new DialogChromeFilter(dialog, titleBar, resizable);
    dialog->installEventFilter(filter);
    return titleBar;
}

} // namespace frost::ui
