// SPDX-License-Identifier: Apache-2.0
//
// TerminalView (frost/ui/terminal/terminal_view.hpp): painting the cell grid, the keys, the IME, the
// mouse, the wheel, the selection and the clipboard.

#include "frost/ui/terminal/terminal_view.hpp"

#include "frost/core/log.hpp"
#include "frost/term/keys.hpp"
#include "frost/ui/key_translation.hpp"

#include <QClipboard>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::ui {

namespace {

using term::KeyMods;
using term::ScreenPos;
using term::TermKey;

std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString fromUtf8(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

bool isModifierKey(int key) noexcept {
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock: return true;
    default: return false;
    }
}

#ifdef _WIN32
term::Win32KeyEvent nativeKey(const QKeyEvent& event, bool down) {
    DWORD state = 0;
    const auto native = event.nativeModifiers();
    if ((native & windows_native::kAltLeft) != 0)
        state |= LEFT_ALT_PRESSED;
    if ((native & windows_native::kAltRight) != 0)
        state |= RIGHT_ALT_PRESSED;
    if ((native & windows_native::kControlLeft) != 0)
        state |= LEFT_CTRL_PRESSED;
    if ((native & windows_native::kControlRight) != 0)
        state |= RIGHT_CTRL_PRESSED;
    if ((event.modifiers() & Qt::ShiftModifier) != 0)
        state |= SHIFT_PRESSED;
    if ((GetKeyState(VK_CAPITAL) & 1) != 0)
        state |= CAPSLOCK_ON;
    if ((GetKeyState(VK_NUMLOCK) & 1) != 0)
        state |= NUMLOCK_ON;
    if ((GetKeyState(VK_SCROLL) & 1) != 0)
        state |= SCROLLLOCK_ON;
    if ((event.nativeScanCode() & 0x100U) != 0)
        state |= ENHANCED_KEY;
    const QString text = event.text();
    const auto character = text.isEmpty() ? 0U : static_cast<unsigned>(text.at(0).unicode());
    return {static_cast<std::uint16_t>(event.nativeVirtualKey()),
            static_cast<std::uint16_t>(event.nativeScanCode() & 0xffU),
            static_cast<std::uint16_t>(character),
            down,
            state,
            static_cast<std::uint16_t>(std::max(event.count(), 1))};
}
#endif

KeyMods modsOf(const QKeyEvent& event) noexcept {
    const Qt::KeyboardModifiers modifiers = event.modifiers();
    return KeyMods{(modifiers & Qt::ShiftModifier) != 0 || event.key() == Qt::Key_Backtab,
                   (modifiers & Qt::ControlModifier) != 0, (modifiers & Qt::AltModifier) != 0};
}

KeyMods modsOf(Qt::KeyboardModifiers modifiers) noexcept {
    return KeyMods{(modifiers & Qt::ShiftModifier) != 0, (modifiers & Qt::ControlModifier) != 0,
                   (modifiers & Qt::AltModifier) != 0};
}

std::optional<TermKey> termKeyOf(int key) noexcept {
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter: return TermKey::Enter;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return TermKey::Tab;
    case Qt::Key_Backspace: return TermKey::Backspace;
    case Qt::Key_Escape: return TermKey::Escape;
    case Qt::Key_Up: return TermKey::Up;
    case Qt::Key_Down: return TermKey::Down;
    case Qt::Key_Left: return TermKey::Left;
    case Qt::Key_Right: return TermKey::Right;
    case Qt::Key_Insert: return TermKey::Insert;
    case Qt::Key_Delete: return TermKey::Delete;
    case Qt::Key_Home: return TermKey::Home;
    case Qt::Key_End: return TermKey::End;
    case Qt::Key_PageUp: return TermKey::PageUp;
    case Qt::Key_PageDown: return TermKey::PageDown;
    default: break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        return static_cast<TermKey>(static_cast<int>(TermKey::F1) + (key - Qt::Key_F1));
    }
    return std::nullopt;
}

// The unshifted US-QWERTY character of a character key (letters lower-case), or nullopt.
std::optional<char32_t> baseCharOf(int key) noexcept {
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<char32_t>(U'a' + static_cast<char32_t>(key - Qt::Key_A));
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return static_cast<char32_t>(U'0' + static_cast<char32_t>(key - Qt::Key_0));
    }
    switch (key) {
    case Qt::Key_Space: return U' ';
    case Qt::Key_BracketLeft: return U'[';
    case Qt::Key_BracketRight: return U']';
    case Qt::Key_Backslash: return U'\\';
    case Qt::Key_Slash: return U'/';
    case Qt::Key_Minus: return U'-';
    case Qt::Key_Underscore: return U'_';
    case Qt::Key_At: return U'@';
    case Qt::Key_AsciiCircum: return U'^';
    case Qt::Key_Question: return U'?';
    case Qt::Key_Comma: return U',';
    case Qt::Key_Period: return U'.';
    case Qt::Key_Semicolon: return U';';
    case Qt::Key_Apostrophe: return U'\'';
    case Qt::Key_QuoteLeft: return U'`';
    case Qt::Key_Equal: return U'=';
    default: return std::nullopt;
    }
}

// Text a key produced that is not a control character (an AltGr combination, Alt+letter).
bool isPrintable(const QString& text) noexcept { return !text.isEmpty() && text.at(0).isPrint(); }

char32_t firstCodePoint(const QString& text) {
    const QList<uint> points = text.toUcs4();
    return points.isEmpty() ? U'\0' : static_cast<char32_t>(points.front());
}

term::MouseButton mouseButtonOf(Qt::MouseButton button) noexcept {
    switch (button) {
    case Qt::MiddleButton: return term::MouseButton::Middle;
    case Qt::RightButton: return term::MouseButton::Right;
    default: return term::MouseButton::Left;
    }
}

// Characters that end a word for double-click selection.
bool isWordBreak(const std::string& text) {
    if (text.empty() || text == " ") {
        return true;
    }
    return text.size() == 1 && std::string_view("\"'`()[]{}<>|;,").find(text.front()) != std::string_view::npos;
}

} // namespace

TerminalView::TerminalView(QWidget* parent) : QWidget(parent), m_colors(TerminalColors::defaults()) {
    setObjectName(QStringLiteral("terminalView"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);
    setCursor(Qt::IBeamCursor);
    m_scrollBar = new QScrollBar(Qt::Vertical, this);
    m_scrollBar->setObjectName(QStringLiteral("terminalScrollBar"));
    m_scrollBar->setFocusPolicy(Qt::NoFocus);
    m_scrollBar->setCursor(Qt::ArrowCursor);
    m_scrollBar->setRange(0, 0);
    connect(m_scrollBar, &QScrollBar::valueChanged, this,
            [this](int value) { setScrollOffset(m_scrollBar->maximum() - value); });
    QFont font(QStringLiteral("JetBrains Mono"));
    font.setStyleHint(QFont::TypeWriter, QFont::PreferDefault);
    font.setFixedPitch(true);
    font.setPointSizeF(13);
    m_font = font;
    updateMetrics();
}

TerminalView::~TerminalView() {
    // Joins the session's threads (no notification is posted afterwards) and ends the shell's tree.
    if (m_session) {
        m_session->close();
    }
}

Result<void> TerminalView::start(term::IPtyLauncher& launcher, const io::ProcessSpec& spec,
                                 std::size_t scrollbackLines) {
    if (m_session) {
        return fail(ErrorCode::AlreadyExists, "the terminal view already runs a session");
    }
    // The observer runs on the session's threads: everything is posted to this widget, which
    // outlives the threads (the destructor joins them first).
    term::SessionObserver observer;
    observer.changed = [this] { QMetaObject::invokeMethod(this, [this] { onScreenChanged(); }, Qt::QueuedConnection); };
    observer.titleChanged = [this](const std::string& title) {
        QMetaObject::invokeMethod(
            this,
            [this, text = fromUtf8(title)] {
                m_title = text;
                emit titleChanged(text);
            },
            Qt::QueuedConnection);
    };
    observer.bell = [this] { QMetaObject::invokeMethod(this, [this] { emit bellRang(); }, Qt::QueuedConnection); };
    observer.exited = [this](io::ExitStatus status) {
        QMetaObject::invokeMethod(this, [this, code = status.exitCode] { emit exited(code); }, Qt::QueuedConnection);
    };
    auto session = term::TerminalSession::start(launcher, spec, m_grid, scrollbackLines, std::move(observer));
    if (!session) {
        return std::unexpected(std::move(session.error()));
    }
    m_session = std::move(*session);
    if (hasFocus()) {
        m_session->focusChanged(true);
    }
    update();
    return {};
}

void TerminalView::setColors(const TerminalColors& colors) {
    m_colors = colors;
    update();
}

void TerminalView::setFontSettings(const QString& family, double pointSize) {
    QFont font(family);
    font.setStyleHint(QFont::TypeWriter, QFont::PreferDefault);
    font.setFixedPitch(true);
    font.setPointSizeF(std::max(1.0, pointSize));
    m_font = font;
    updateMetrics();
    update();
}

void TerminalView::updateMetrics() {
    const QFontMetricsF metrics(m_font);
    m_cellWidth = std::max(1.0, metrics.horizontalAdvance(QLatin1Char('M')));
    m_cellHeight = std::max(1.0, std::ceil(metrics.lineSpacing()));
    m_ascent = metrics.ascent();
    updateGrid();
}

void TerminalView::updateGrid() {
    const int barWidth = m_scrollBar->sizeHint().width();
    m_scrollBar->setGeometry(width() - barWidth, 0, barWidth, height());
    const double usable = std::max(0, width() - barWidth);
    const auto columns = static_cast<std::uint16_t>(std::clamp(std::floor(usable / m_cellWidth), 1.0, 4096.0));
    const auto rows = static_cast<std::uint16_t>(std::clamp(std::floor(height() / m_cellHeight), 1.0, 4096.0));
    const term::PtySize grid{columns, rows};
    if (grid == m_grid) {
        return;
    }
    m_grid = grid;
    if (m_session) {
        if (Result<void> resized = m_session->resize(grid); !resized) {
            log::warn("ui", "terminal resize: {}", resized.error().message());
        }
        updateScrollBar();
    }
}

void TerminalView::updateScrollBar() {
    const int scrollback =
        m_session
            ? m_session->read([](const term::VtScreen& screen) { return static_cast<int>(screen.scrollbackSize()); })
            : 0;
    m_scrollOffset = std::clamp(m_scrollOffset, 0, scrollback);
    const QSignalBlocker blocker(m_scrollBar);
    m_scrollBar->setRange(0, scrollback);
    m_scrollBar->setPageStep(m_grid.rows);
    m_scrollBar->setValue(scrollback - m_scrollOffset);
}

void TerminalView::onScreenChanged() {
    if (!m_session) {
        return;
    }
    // Re-arm first: output arriving while this runs notifies again.
    m_session->acknowledge();
    const auto [pushed, scrollback] = m_session->read([](const term::VtScreen& screen) {
        return std::pair{screen.linesPushed(), static_cast<int>(screen.scrollbackSize())};
    });
    const auto delta = static_cast<int>(std::min<std::uint64_t>(pushed - m_linesPushed, 1U << 30U));
    m_linesPushed = pushed;
    if (delta > 0) {
        // A view scrolled back keeps showing the same lines; the selection moves with its text.
        if (m_scrollOffset > 0) {
            m_scrollOffset = std::min(m_scrollOffset + delta, scrollback);
        }
        if (m_selection) {
            m_selection->anchor.row -= delta;
            m_selection->end.row -= delta;
        }
    }
    updateScrollBar();
    update();
}

void TerminalView::setScrollOffset(int lines) {
    const int scrollback =
        m_session
            ? m_session->read([](const term::VtScreen& screen) { return static_cast<int>(screen.scrollbackSize()); })
            : 0;
    const int clamped = std::clamp(lines, 0, scrollback);
    if (clamped == m_scrollOffset) {
        return;
    }
    m_scrollOffset = clamped;
    updateScrollBar();
    update();
}

// --- painting ---

void TerminalView::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), m_colors.background);
    if (!m_session) {
        return;
    }
    struct Snapshot {
        std::vector<std::vector<term::Cell>> lines;
        term::CursorState cursor;
    };
    const int offset = m_scrollOffset;
    const Snapshot snapshot = m_session->read([offset](const term::VtScreen& screen) {
        Snapshot out;
        out.lines.reserve(static_cast<std::size_t>(screen.rows()));
        for (int row = 0; row < screen.rows(); ++row) {
            out.lines.push_back(screen.line(row - offset));
        }
        out.cursor = screen.cursor();
        return out;
    });

    std::optional<std::pair<ScreenPos, ScreenPos>> selection;
    if (m_selection && (!m_selecting || m_selection->anchor != m_selection->end)) {
        selection = std::minmax(m_selection->anchor, m_selection->end);
    }
    const auto selected = [&selection](int row, int column) {
        const ScreenPos pos{row, column};
        return selection && !(pos < selection->first) && !(selection->second < pos);
    };

    QFont bold = m_font;
    bold.setBold(true);
    QFont italic = m_font;
    italic.setItalic(true);
    QFont boldItalic = bold;
    boldItalic.setItalic(true);

    for (std::size_t visible = 0; visible < snapshot.lines.size(); ++visible) {
        const int row = static_cast<int>(visible) - offset;
        const double y = static_cast<double>(visible) * m_cellHeight;
        const std::vector<term::Cell>& line = snapshot.lines[visible];
        for (std::size_t column = 0; column < line.size(); ++column) {
            const term::Cell& cell = line[column];
            if (cell.width == 0) {
                continue;
            }
            const double x = static_cast<double>(column) * m_cellWidth;
            const QRectF box(x, y, m_cellWidth * cell.width, m_cellHeight);
            QColor foreground = m_colors.resolve(cell.foreground, true);
            QColor background = m_colors.resolve(cell.background, false);
            if (cell.style.inverse) {
                std::swap(foreground, background);
            }
            if (selected(row, static_cast<int>(column))) {
                background = m_colors.selection;
            }
            if (background != m_colors.background) {
                painter.fillRect(box, background);
            }
            if (cell.text.empty() || cell.style.conceal) {
                continue;
            }
            painter.setFont(cell.style.bold && cell.style.italic ? boldItalic
                            : cell.style.bold                    ? bold
                            : cell.style.italic                  ? italic
                                                                 : m_font);
            painter.setPen(foreground);
            painter.drawText(QPointF(x, y + m_ascent), fromUtf8(cell.text));
            if (cell.style.underline != term::Underline::None) {
                const double underlineY = y + m_ascent + 1.5;
                painter.drawLine(QPointF(x, underlineY), QPointF(box.right(), underlineY));
            }
            if (cell.style.strike) {
                const double strikeY = y + (m_cellHeight / 2);
                painter.drawLine(QPointF(x, strikeY), QPointF(box.right(), strikeY));
            }
        }
    }

    // The cursor, on the live screen only.
    if (offset == 0 && snapshot.cursor.visible && snapshot.cursor.row >= 0 &&
        snapshot.cursor.row < static_cast<int>(snapshot.lines.size())) {
        const QRectF box = cursorRect();
        if (!m_preedit.isEmpty()) {
            painter.setFont(m_font);
            painter.setPen(m_colors.foreground);
            const QFontMetricsF metrics(m_font);
            const QRectF area(box.topLeft(), QSizeF(metrics.horizontalAdvance(m_preedit), m_cellHeight));
            painter.fillRect(area, m_colors.background);
            painter.drawText(QPointF(box.left(), box.top() + m_ascent), m_preedit);
            painter.drawLine(QPointF(area.left(), area.bottom() - 1), QPointF(area.right(), area.bottom() - 1));
        } else if (!hasFocus()) {
            painter.setPen(m_colors.cursor);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
        } else if (snapshot.cursor.shape == term::CursorShape::Bar) {
            painter.fillRect(QRectF(box.left(), box.top(), 2, box.height()), m_colors.cursor);
        } else if (snapshot.cursor.shape == term::CursorShape::Underline) {
            painter.fillRect(QRectF(box.left(), box.bottom() - 2, box.width(), 2), m_colors.cursor);
        } else {
            painter.fillRect(box, m_colors.cursor);
            const auto& line = snapshot.lines[static_cast<std::size_t>(snapshot.cursor.row)];
            const auto column = static_cast<std::size_t>(std::max(snapshot.cursor.column, 0));
            if (column < line.size() && !line[column].text.empty()) {
                painter.setFont(m_font);
                painter.setPen(m_colors.background);
                painter.drawText(QPointF(box.left(), box.top() + m_ascent), fromUtf8(line[column].text));
            }
        }
    }
}

QRectF TerminalView::cursorRect() const {
    const term::CursorState cursor =
        m_session ? m_session->read([](const term::VtScreen& screen) { return screen.cursor(); }) : term::CursorState{};
    return QRectF(cursor.column * m_cellWidth, (cursor.row + m_scrollOffset) * m_cellHeight, m_cellWidth, m_cellHeight);
}

void TerminalView::resizeEvent(QResizeEvent* /*event*/) {
    updateGrid();
    update();
}

// --- keys ---

bool TerminalView::keepsKey(const QKeyEvent& event) const {
    const int key = event.key();
    if (isModifierKey(key)) {
        return false;
    }
    const KeyMods mods = modsOf(event);
    if (const std::optional<TermKey> termKey = termKeyOf(key)) {
        return term::terminalKeepsKey(*termKey, mods);
    }
    if (mods.ctrl && mods.alt && isPrintable(event.text())) {
        return true; // AltGr text.
    }
    if (const std::optional<char32_t> base = baseCharOf(key)) {
        return term::terminalKeepsChar(*base, mods);
    }
    return !mods.ctrl && !mods.alt;
}

bool TerminalView::focusNextPrevChild(bool /*next*/) { return false; }

void TerminalView::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    const KeyMods mods = modsOf(*event);
    if (mods.ctrl && !mods.alt && !mods.shift && key == Qt::Key_F) {
        emit findRequested();
        event->accept();
        return;
    }
    if (!m_session) {
        event->ignore();
        return;
    }
    const QString text = event->text();
    // The terminal's own copy and paste.
    if (mods.ctrl && !mods.alt && key == Qt::Key_C && (mods.shift || hasSelection())) {
        copySelection();
        if (!mods.shift) {
            clearSelection();
        }
        event->accept();
        return;
    }
    if ((mods.ctrl && !mods.alt && key == Qt::Key_V) ||
        (mods.shift && !mods.ctrl && !mods.alt && key == Qt::Key_Insert)) {
        pasteClipboard();
        event->accept();
        return;
    }

#ifdef _WIN32
    term::Win32KeyEvent record = nativeKey(*event, true);
    if (event->nativeVirtualKey() != 0 && m_session->sendWin32Key(record)) {
        // Win32's wire format carries one UTF-16 code unit per key record.
        for (qsizetype i = 1; i < text.size(); ++i) {
            record.unicode = text.at(i).unicode();
            static_cast<void>(m_session->sendWin32Key(record));
        }
        m_nativeKeysDown[event->nativeVirtualKey()] = nativeKey(*event, true);
        clearSelection();
        scrollToBottom();
        event->accept();
        return;
    }
#endif
    if (isModifierKey(key)) {
        event->ignore();
        return;
    }
    if (const std::optional<TermKey> termKey = termKeyOf(key)) {
        m_session->sendKey(*termKey, mods);
    } else if (mods.ctrl && mods.alt && isPrintable(text)) {
        m_session->sendText(toUtf8(text)); // AltGr text.
    } else if (mods.ctrl || mods.alt) {
        const std::optional<char32_t> base = baseCharOf(key);
        if (mods.alt && !mods.ctrl && isPrintable(text)) {
            m_session->sendChar(firstCodePoint(text), mods); // Alt with the layout's (shifted) character.
        } else if (base) {
            m_session->sendChar(*base, mods);
        } else if (isPrintable(text)) {
            m_session->sendChar(firstCodePoint(text), mods);
        } else {
            event->ignore();
            return;
        }
    } else if (!text.isEmpty()) {
        const auto points = text.toUcs4();
        if (points.size() == 1)
            m_session->sendChar(static_cast<char32_t>(points.front()), mods);
        else
            m_session->sendText(toUtf8(text));
    } else {
        event->ignore();
        return;
    }
    clearSelection();
    scrollToBottom();
    event->accept();
}

void TerminalView::keyReleaseEvent(QKeyEvent* event) {
#ifdef _WIN32
    if (m_session && m_nativeKeysDown.erase(event->nativeVirtualKey()) != 0) {
        term::Win32KeyEvent record = nativeKey(*event, false);
        if (m_session->sendWin32Key(record)) {
            const QString text = event->text();
            for (qsizetype i = 1; i < text.size(); ++i) {
                record.unicode = text.at(i).unicode();
                static_cast<void>(m_session->sendWin32Key(record));
            }
            event->accept();
            return;
        }
    }
#endif
    event->ignore();
}

void TerminalView::inputMethodEvent(QInputMethodEvent* event) {
    if (m_session && !event->commitString().isEmpty()) {
        m_session->sendText(toUtf8(event->commitString()));
        clearSelection();
        scrollToBottom();
    }
    m_preedit = event->preeditString();
    update();
    event->accept();
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const {
    switch (query) {
    case Qt::ImEnabled: return true;
    case Qt::ImCursorRectangle: return cursorRect().toAlignedRect();
    case Qt::ImFont: return m_font;
    case Qt::ImCursorPosition: return static_cast<int>(m_preedit.size());
    case Qt::ImSurroundingText: return QString();
    default: return QWidget::inputMethodQuery(query);
    }
}

// --- mouse ---

ScreenPos TerminalView::cellAt(QPointF point) const {
    const int column = std::clamp(static_cast<int>(std::floor(point.x() / m_cellWidth)), 0, m_grid.columns - 1);
    const int visible = std::clamp(static_cast<int>(std::floor(point.y() / m_cellHeight)), 0, m_grid.rows - 1);
    return ScreenPos{visible - m_scrollOffset, column};
}

bool TerminalView::reportsMouse(Qt::KeyboardModifiers modifiers) const {
    if (!m_session || (modifiers & Qt::ShiftModifier) != 0) {
        return false;
    }
    return m_session->read([](const term::VtScreen& screen) { return screen.mouseMode(); }) != term::MouseMode::None;
}

void TerminalView::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    if (!m_session) {
        return;
    }
    if (reportsMouse(event->modifiers())) {
        scrollToBottom();
        const ScreenPos pos = cellAt(event->position());
        const term::MouseButton button = mouseButtonOf(event->button());
        m_session->mouseButton(button, true, pos.row, pos.column, modsOf(event->modifiers()));
        m_reportedButton = button;
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const ScreenPos pos = cellAt(event->position());
        m_selection = Selection{pos, pos};
        m_selecting = true;
        update();
    } else if (event->button() == Qt::MiddleButton) {
        pasteClipboard();
    }
    event->accept();
}

void TerminalView::mouseMoveEvent(QMouseEvent* event) {
    if (!m_session) {
        return;
    }
    if (m_selecting && m_selection) {
        m_selection->end = cellAt(event->position());
        update();
        return;
    }
    if (reportsMouse(event->modifiers())) {
        const ScreenPos pos = cellAt(event->position());
        m_session->mouseMove(pos.row, pos.column, modsOf(event->modifiers()));
    }
}

void TerminalView::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_session) {
        return;
    }
    if (m_reportedButton) {
        const ScreenPos pos = cellAt(event->position());
        m_session->mouseButton(*m_reportedButton, false, pos.row, pos.column, modsOf(event->modifiers()));
        m_reportedButton.reset();
        return;
    }
    if (m_selecting) {
        m_selecting = false;
        if (m_selection && m_selection->anchor == m_selection->end) {
            m_selection.reset(); // A click without a drag selects nothing.
        }
        update();
    }
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!m_session || reportsMouse(event->modifiers()) || event->button() != Qt::LeftButton) {
        mousePressEvent(event);
        return;
    }
    const ScreenPos pos = cellAt(event->position());
    const std::vector<term::Cell> line =
        m_session->read([&pos](const term::VtScreen& screen) { return screen.line(pos.row); });
    if (line.empty() || isWordBreak(line[static_cast<std::size_t>(pos.column)].text)) {
        return;
    }
    int first = pos.column;
    while (first > 0 && (line[static_cast<std::size_t>(first - 1)].width == 0 ||
                         !isWordBreak(line[static_cast<std::size_t>(first - 1)].text))) {
        --first;
    }
    int last = pos.column;
    while (last + 1 < static_cast<int>(line.size()) && (line[static_cast<std::size_t>(last + 1)].width == 0 ||
                                                        !isWordBreak(line[static_cast<std::size_t>(last + 1)].text))) {
        ++last;
    }
    m_selection = Selection{ScreenPos{pos.row, first}, ScreenPos{pos.row, last}};
    m_selecting = false;
    update();
}

void TerminalView::wheelEvent(QWheelEvent* event) {
    if (!m_session) {
        return;
    }
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        return;
    }
    // Three lines per notch (120 units); a small touchpad step still moves one line.
    int lines = delta / 40;
    if (lines == 0) {
        lines = delta > 0 ? 1 : -1;
    }
    const KeyMods mods = modsOf(event->modifiers());
    if (reportsMouse(event->modifiers())) {
        const ScreenPos pos = cellAt(event->position());
        const term::MouseButton button = lines > 0 ? term::MouseButton::WheelUp : term::MouseButton::WheelDown;
        for (int i = 0; i < std::abs(lines); ++i) {
            m_session->mouseButton(button, true, pos.row, pos.column, mods);
        }
    } else if (m_session->read([](const term::VtScreen& screen) { return screen.altScreen(); })) {
        for (int i = 0; i < std::abs(lines); ++i) {
            m_session->sendKey(lines > 0 ? TermKey::Up : TermKey::Down, {});
        }
    } else {
        setScrollOffset(m_scrollOffset + lines);
    }
    event->accept();
}

void TerminalView::focusInEvent(QFocusEvent* event) {
    if (m_session) {
        m_session->focusChanged(true);
    }
    update();
    QWidget::focusInEvent(event);
}

void TerminalView::focusOutEvent(QFocusEvent* event) {
    if (m_session) {
        // A release may be delivered to another widget after focus changes. End every
        // native key held by this terminal before reporting focus loss.
        for (auto& [key, record] : m_nativeKeysDown) {
            static_cast<void>(key);
            record.down = false;
            record.unicode = 0;
            record.controlState = 0;
            static_cast<void>(m_session->sendWin32Key(record));
        }
        m_nativeKeysDown.clear();
        m_session->focusChanged(false);
    }
    update();
    QWidget::focusOutEvent(event);
}

// --- selection and the clipboard ---

QString TerminalView::selectedText() const {
    if (!m_session || !m_selection) {
        return {};
    }
    const Selection selection = *m_selection;
    return fromUtf8(m_session->read(
        [&selection](const term::VtScreen& screen) { return screen.text(selection.anchor, selection.end); }));
}

void TerminalView::select(ScreenPos from, ScreenPos to) {
    m_selection = Selection{from, to};
    m_selecting = false;
    update();
}

void TerminalView::selectAll() {
    if (!m_session) {
        return;
    }
    const auto [scrollback, rows, columns] = m_session->read([](const term::VtScreen& screen) {
        return std::tuple{static_cast<int>(screen.scrollbackSize()), screen.rows(), screen.columns()};
    });
    select(ScreenPos{-scrollback, 0}, ScreenPos{rows - 1, columns - 1});
}

void TerminalView::clearSelection() {
    if (m_selection) {
        m_selection.reset();
        m_selecting = false;
        update();
    }
}

bool TerminalView::copySelection() {
    if (!m_selection) {
        return false;
    }
    QGuiApplication::clipboard()->setText(selectedText());
    return true;
}

void TerminalView::pasteClipboard() {
    if (!m_session) {
        return;
    }
    const QString text = QGuiApplication::clipboard()->text();
    if (!text.isEmpty()) {
        m_session->paste(toUtf8(text));
        scrollToBottom();
    }
}

void appendRowMatches(int row, const std::vector<term::Cell>& cells, const QString& query,
                      std::vector<std::pair<ScreenPos, ScreenPos>>& found) {
    QString rowText;
    std::vector<int> charToCol;
    rowText.reserve(static_cast<qsizetype>(cells.size()));
    charToCol.reserve(cells.size());
    for (std::size_t col = 0; col < cells.size(); ++col) {
        if (cells[col].width == 0) {
            continue;
        }
        const QString cellStr = cells[col].text.empty() ? QStringLiteral(" ") : fromUtf8(cells[col].text);
        rowText.append(cellStr);
        for (qsizetype k = 0; k < cellStr.size(); ++k) {
            charToCol.push_back(static_cast<int>(col));
        }
    }
    qsizetype searchFrom = 0;
    while (searchFrom <= rowText.size() - query.size()) {
        const qsizetype hit = rowText.indexOf(query, searchFrom, Qt::CaseInsensitive);
        if (hit < 0) {
            break;
        }
        const int startCol = charToCol[static_cast<std::size_t>(hit)];
        const int endCol = charToCol[static_cast<std::size_t>(hit + query.size() - 1)];
        found.emplace_back(ScreenPos{row, startCol}, ScreenPos{row, endCol});
        searchFrom = hit + 1;
    }
}

bool TerminalView::findText(const QString& query, bool forward, bool stayAtCurrent) {
    if (!m_session || query.isEmpty()) {
        clearSelection();
        return false;
    }
    const auto matches = m_session->read([&query](const term::VtScreen& screen) {
        std::vector<std::pair<ScreenPos, ScreenPos>> found;
        const int firstRow = -static_cast<int>(screen.scrollbackSize());
        const int lastRow = screen.rows() - 1;
        for (int row = firstRow; row <= lastRow; ++row) {
            appendRowMatches(row, screen.line(row), query, found);
        }
        return found;
    });
    if (matches.empty()) {
        clearSelection();
        return false;
    }

    std::size_t chosen = forward ? 0 : matches.size() - 1;
    if (m_selection) {
        const ScreenPos curStart = std::min(m_selection->anchor, m_selection->end);
        if (forward) {
            for (std::size_t i = 0; i < matches.size(); ++i) {
                if (stayAtCurrent ? !(matches[i].first < curStart) : (curStart < matches[i].first)) {
                    chosen = i;
                    break;
                }
            }
        } else {
            for (std::size_t i = matches.size(); i > 0; --i) {
                if (matches[i - 1].first < curStart) {
                    chosen = i - 1;
                    break;
                }
            }
        }
    }
    const auto& [matchStart, matchEnd] = matches[chosen];
    select(matchStart, matchEnd);
    const int visibleRow = matchStart.row + m_scrollOffset;
    if (visibleRow < 0 || visibleRow >= static_cast<int>(m_grid.rows)) {
        setScrollOffset((static_cast<int>(m_grid.rows) / 2) - matchStart.row);
    }
    return true;
}

} // namespace frost::ui
