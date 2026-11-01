// SPDX-License-Identifier: Apache-2.0
//
// FakeOsAppearance: a scriptable IOsAppearanceSource for the AppearanceManager tests (Sync with
// OS) and the screenshot tests, which must not depend on the machine's colour scheme.
#pragma once

#include "frost/ui/model/listener.hpp"
#include "frost/ui/model/os_appearance.hpp"

#include <functional>

namespace frost::ui::test {

class FakeOsAppearance final : public IOsAppearanceSource {
public:
    explicit FakeOsAppearance(OsAppearance initial = {}) : m_current(initial) {}

    [[nodiscard]] OsAppearance current() const override { return m_current; }
    [[nodiscard]] ListenerToken subscribe(std::function<void(OsAppearance)> listener) override {
        return m_listeners.add(std::move(listener));
    }

    // Changes the value and notifies every listener when it differs (like QtOsAppearanceSource).
    void set(OsAppearance value) {
        if (value == m_current) {
            return;
        }
        m_current = value;
        m_listeners.notify(value);
    }

    [[nodiscard]] std::size_t listenerCount() const { return m_listeners.size(); }

private:
    OsAppearance m_current;
    ListenerList<OsAppearance> m_listeners;
};

} // namespace frost::ui::test
