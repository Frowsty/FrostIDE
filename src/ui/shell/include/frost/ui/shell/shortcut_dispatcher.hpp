// SPDX-License-Identifier: Apache-2.0
//
// ShortcutDispatcher: the shell's application event filter that routes key events to
// frost::input's matcher (ADR 0016 Consequences, ADR 0038). Qt's own shortcut machinery is never
// used for keymap-bound actions.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/input/action.hpp"
#include "frost/input/key_event.hpp"
#include "frost/input/keymap_service.hpp"
#include "frost/ui/ime_tracker.hpp"

#include <QObject>
#include <QPointer>

#include <functional>

class QWidget;

namespace frost::ui {

/*
 * Installed on qApp by the shell window, for the events of that window and the popups and dialogs
 * it owns (other windows' events are ignored):
 *   - every event goes through ImeTracker::observe; a composition start resets the matcher;
 *   - ShortcutOverride is accepted when KeymapService::peek is not PassThrough, so Qt sends the
 *     key press instead of firing a QShortcut or a menu mnemonic;
 *   - every key press and release goes through KeymapService::handle exactly once (Qt re-sends an
 *     ignored key event to each parent; the re-sends are recognised and skipped);
 *   - Fire runs ActionRegistry::tryInvoke with InvokeSource::Keyboard and emits invoked(); Pending
 *     and Consumed are swallowed, PassThrough is delivered as usual;
 *   - QEvent::Shortcut, mouse presses, focus changes and window deactivation reset the matcher;
 *   - KeyboardLayoutChange re-reads the active layout when followActiveLayout is set;
 *   - while a Qt popup that is not a frost Popup is open (a menu, a combo box list) keys pass to it;
 *   - a key the receiving widget keeps (frost/ui/shell/key_keeper.hpp) passes to it, its
 *     ShortcutOverride accepted, without reaching the matcher.
 * The context comes from `context` at the moment of each event. The registry and the service must
 * outlive the dispatcher. GUI thread only. Budget: B5.
 */
class ShortcutDispatcher final : public QObject {
    Q_OBJECT

public:
    using ContextProvider = std::function<input::ActionContext()>;

    ShortcutDispatcher(QWidget& window, input::ActionRegistry& registry, input::KeymapService& keymap,
                       ContextProvider context, bool followActiveLayout, QObject* parent = nullptr);
    ~ShortcutDispatcher() override;

    bool eventFilter(QObject* watched, QEvent* event) override;

    [[nodiscard]] const ImeTracker& ime() const noexcept { return m_ime; }

signals:
    // An action fired from the keyboard (after it ran).
    void invoked(quint32 actionId);
    // The first stroke of a two-stroke sequence is pending ("" when no stroke is pending).
    void pendingChanged(const QString& prefixText);

private:
    [[nodiscard]] bool belongsToWindow(const QObject* watched) const;
    [[nodiscard]] bool isRedelivery(const QObject* watched, const QEvent* event) const;
    void remember(const QObject* watched, const QEvent* event);
    void resetMatcher();
    void reportPending();

    QWidget& m_window;
    input::ActionRegistry& m_registry;
    input::KeymapService& m_keymap;
    ContextProvider m_context;
    bool m_followActiveLayout;
    input::KeyPlatform m_platform;
    ImeTracker m_ime;
    // The last key event handled and the widget it was delivered to (propagation detection).
    const QEvent* m_lastEvent = nullptr;
    QPointer<QObject> m_lastReceiver;
    int m_lastType = 0;
    bool m_pendingShown = false;
};

} // namespace frost::ui
