// SPDX-License-Identifier: Apache-2.0
//
// ImeTracker (frost/ui/ime_tracker.hpp): composition state from observed Qt events.

#include "frost/ui/ime_tracker.hpp"

#include <QEvent>
#include <QInputMethodEvent>

namespace frost::ui {

void ImeTracker::observe(const QEvent& event) noexcept {
    switch (event.type()) {
    case QEvent::InputMethod: {
        const bool composing = !static_cast<const QInputMethodEvent&>(event).preeditString().isEmpty();
        if (composing && !m_composing) {
            m_started = true;
        }
        m_composing = composing;
        break;
    }
    case QEvent::FocusIn:
    case QEvent::FocusOut:
    case QEvent::WindowDeactivate: m_composing = false; break;
    case QEvent::ApplicationStateChange:
        if (static_cast<const QApplicationStateChangeEvent&>(event).applicationState() != Qt::ApplicationActive) {
            m_composing = false;
        }
        break;
    default: break;
    }
}

bool ImeTracker::takeCompositionStarted() noexcept {
    const bool started = m_started;
    m_started = false;
    return started;
}

void ImeTracker::reset() noexcept {
    m_composing = false;
    m_started = false;
}

} // namespace frost::ui
