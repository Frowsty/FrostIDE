// SPDX-License-Identifier: Apache-2.0
//
// QtOsAppearanceSource (qt_os_appearance.hpp).

#include "frost/ui/qt_os_appearance.hpp"

#include <QAccessibilityHints>
#include <QGuiApplication>
#include <QStyleHints>

#include <cstdlib>
#include <optional>
#include <string>

namespace frost::ui {

namespace {

OsAppearance fromQt() {
    OsAppearance result;
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr) {
        return result;
    }
    const QStyleHints* hints = QGuiApplication::styleHints();
    switch (hints->colorScheme()) {
    case Qt::ColorScheme::Dark: result.scheme = ColorScheme::Dark; break;
    case Qt::ColorScheme::Light: result.scheme = ColorScheme::Light; break;
    default: result.scheme = ColorScheme::Unknown; break;
    }
    result.highContrast = hints->accessibility()->contrastPreference() == Qt::ContrastPreference::HighContrast;
    return result;
}

} // namespace

QtOsAppearanceSource::QtOsAppearanceSource(QObject* parent) : QObject(parent), m_current(query()) {
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr) {
        const QStyleHints* hints = QGuiApplication::styleHints();
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this] { refresh(); });
        connect(hints->accessibility(), &QAccessibilityHints::contrastPreferenceChanged, this, [this] { refresh(); });
    }
}

QtOsAppearanceSource::~QtOsAppearanceSource() = default;

OsAppearance QtOsAppearanceSource::query() {
#ifdef _WIN32
    return combineAppearance(readWindowsAppearance(), fromQt());
#else
    return combineAppearance(fromQt(), appearanceFromLinuxEnvironment([](std::string_view name) {
                                 // getenv here runs on the GUI thread only; nothing in FrostIDE calls setenv after
                                 // start-up.
                                 const std::string key(name);
                                 const char* value = std::getenv(key.c_str()); // NOLINT(concurrency-mt-unsafe)
                                 return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
                             }));
#endif
}

void QtOsAppearanceSource::refresh() {
    const OsAppearance now = query();
    if (now == m_current) {
        return;
    }
    m_current = now;
    m_listeners.notify(now);
}

ListenerToken QtOsAppearanceSource::subscribe(std::function<void(OsAppearance)> listener) {
    return m_listeners.add(std::move(listener));
}

} // namespace frost::ui
