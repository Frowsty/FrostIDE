// SPDX-License-Identifier: Apache-2.0
//
// QtOsAppearanceSource: the production IOsAppearanceSource, combining the platform's own
// colour-scheme and high-contrast settings with Qt's (ADR 0015 decision 5).
#pragma once

#include "frost/ui/model/listener.hpp"
#include "frost/ui/model/os_appearance.hpp"

#include <QObject>

#include <functional>

namespace frost::ui {

/*
 * Windows: readWindowsAppearance() (the registry and SPI_GETHIGHCONTRAST) first, then
 * QStyleHints::colorScheme() and QAccessibilityHints::contrastPreference().
 * Linux: Qt first (the desktop and portal themes inside QtGui), then
 * appearanceFromLinuxEnvironment(getenv), a GTK_THEME fallback. Real Linux desktops are not
 * covered by a test leg (WSL has no session).
 * Re-queries on QStyleHints::colorSchemeChanged and QAccessibilityHints::contrastPreferenceChanged
 * and notifies the listeners only when the combined value changes. Never calls
 * QStyleHints::setColorScheme(). Needs a QGuiApplication; GUI thread only.
 */
class QtOsAppearanceSource final : public QObject, public IOsAppearanceSource {
    Q_OBJECT

public:
    explicit QtOsAppearanceSource(QObject* parent = nullptr);
    ~QtOsAppearanceSource() override;

    [[nodiscard]] OsAppearance current() const override { return m_current; }
    [[nodiscard]] ListenerToken subscribe(std::function<void(OsAppearance)> listener) override;

    // Re-reads the platform and Qt, notifying on a change (the signal handlers call it).
    void refresh();

private:
    [[nodiscard]] static OsAppearance query();

    OsAppearance m_current;
    ListenerList<OsAppearance> m_listeners;
};

} // namespace frost::ui
