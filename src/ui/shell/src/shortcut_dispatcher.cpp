// SPDX-License-Identifier: Apache-2.0
//
// The shell's shortcut dispatch filter (frost/ui/shell/shortcut_dispatcher.hpp).
/*
 * Propagation. QApplication::notify sends an ignored key event to each parent of the widget that
 * ignored it, and every send passes the application's event filters again, with the same QKeyEvent
 * object. The matcher must see each key event once, so the filter remembers the last key event it
 * handled and its receiver: the same event object, of the same type, arriving at that receiver's
 * parent is a re-send and passes untouched. A new event at the same stack address goes to the
 * focus widget again, never to the parent, so it is not mistaken for a re-send.
 */

#include "frost/ui/shell/shortcut_dispatcher.hpp"

#include "frost/input/keymap.hpp"
#include "frost/input/layout.hpp"
#include "frost/ui/key_translation.hpp"
#include "frost/ui/shell/popup.hpp"

#include "shell_util.hpp"
#include <QApplication>
#include <QKeyEvent>
#include <QWidget>

#include <optional>
#include <string>
#include <utility>

namespace frost::ui {

ShortcutDispatcher::ShortcutDispatcher(QWidget& window, input::ActionRegistry& registry, input::KeymapService& keymap,
                                       ContextProvider context, bool followActiveLayout, QObject* parent)
    : QObject(parent), m_window(window), m_registry(registry), m_keymap(keymap), m_context(std::move(context)),
      m_followActiveLayout(followActiveLayout), m_platform(currentKeyPlatform()) {
    setObjectName(QStringLiteral("shortcutDispatcher"));
}

ShortcutDispatcher::~ShortcutDispatcher() = default;

bool ShortcutDispatcher::belongsToWindow(const QObject* watched) const {
    if (!watched->isWidgetType()) {
        return false;
    }
    // A popup or dialog is its own window; follow its owner chain up to a top-level window.
    const QWidget* widget = static_cast<const QWidget*>(watched)->window();
    while (widget != nullptr) {
        if (widget == &m_window) {
            return true;
        }
        const QWidget* owner = widget->parentWidget();
        widget = owner != nullptr ? owner->window() : nullptr;
    }
    return false;
}

bool ShortcutDispatcher::isRedelivery(const QObject* watched, const QEvent* event) const {
    if (event != m_lastEvent || static_cast<int>(event->type()) != m_lastType || m_lastReceiver == nullptr ||
        !m_lastReceiver->isWidgetType()) {
        return false;
    }
    return static_cast<const QWidget*>(m_lastReceiver.data())->parentWidget() == watched;
}

void ShortcutDispatcher::remember(const QObject* watched, const QEvent* event) {
    m_lastEvent = event;
    m_lastType = static_cast<int>(event->type());
    m_lastReceiver = const_cast<QObject*>(watched);
}

void ShortcutDispatcher::resetMatcher() {
    m_keymap.reset();
    reportPending();
}

void ShortcutDispatcher::reportPending() {
    const std::optional<input::Stroke> pending = m_keymap.pendingPrefix();
    if (pending) {
        const std::string text =
            input::toDisplayText(input::Sequence::of(*pending), m_keymap.layout(), m_keymap.platform());
        m_pendingShown = true;
        emit pendingChanged(shell_detail::fromUtf8(text));
    } else if (m_pendingShown) {
        m_pendingShown = false;
        emit pendingChanged(QString());
    }
}

bool ShortcutDispatcher::eventFilter(QObject* watched, QEvent* event) {
    m_ime.observe(*event);
    const QEvent::Type type = event->type();
    switch (type) {
    case QEvent::InputMethod:
        if (m_ime.takeCompositionStarted()) {
            resetMatcher();
        }
        return false;
    case QEvent::FocusIn:
    case QEvent::FocusOut:
    case QEvent::MouseButtonPress:
    case QEvent::Shortcut:
    case QEvent::WindowDeactivate:
        if (belongsToWindow(watched)) {
            resetMatcher();
        }
        return false;
    case QEvent::KeyboardLayoutChange:
        if (m_followActiveLayout) {
            if (Result<input::LayoutTable> layout = input::readActiveLayout(); layout) {
                m_keymap.setLayout(*layout);
            }
            reportPending();
        }
        return false;
    case QEvent::ShortcutOverride:
    case QEvent::KeyPress:
    case QEvent::KeyRelease: break;
    default: return false;
    }

    if (!belongsToWindow(watched)) {
        return false;
    }
    // Menus, combo box lists and completers handle their own keys.
    if (QWidget* popup = QApplication::activePopupWidget();
        popup != nullptr && qobject_cast<Popup*>(popup) == nullptr) {
        return false;
    }
    if (isRedelivery(watched, event)) {
        remember(watched, event);
        return false;
    }
    remember(watched, event);

    auto* key = static_cast<QKeyEvent*>(event);
    const KeyTranslationContext translation{m_platform, m_keymap.layout(), m_ime, systemClock()};
    const input::KeyEvent translated = translateKeyEvent(*key, translation);
    const input::ActionContext context = m_context();

    if (type == QEvent::ShortcutOverride) {
        if (m_keymap.peek(translated, context) != input::MatchOutcome::PassThrough) {
            key->accept();
            return true;
        }
        return false;
    }

    const input::MatchResult result = m_keymap.handle(translated, context);
    switch (result.outcome) {
    case input::MatchOutcome::Fire: {
        reportPending();
        const input::InvokeInfo info{input::InvokeSource::Keyboard, key->isAutoRepeat()};
        if (m_registry.tryInvoke(result.action, context, info) == input::InvokeResult::Ran) {
            emit invoked(result.action.value());
        }
        key->accept();
        return true;
    }
    case input::MatchOutcome::Pending:
    case input::MatchOutcome::Consumed:
        reportPending();
        key->accept();
        return true;
    case input::MatchOutcome::PassThrough:
        if (result.cancelledPending) {
            reportPending();
        }
        return false;
    }
    return false;
}

} // namespace frost::ui
