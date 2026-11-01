// SPDX-License-Identifier: Apache-2.0
//
// The appearance settings seam: theme choice (including Sync with OS), compact mode and IDE
// zoom, behind an interface that m1/settings binds to the typed settings registry later
// (ADR 0015 decision 12). InMemoryAppearanceSettings is the store until then and the test
// double. Qt-free.
//
/*
 * The keys m1/settings registers (application scope only; none of them executes anything):
 *   ui.theme                 a theme id or "sync-with-os"; default "glacier-dark"
 *   ui.theme.syncLight       default "glacier-light"
 *   ui.theme.syncDark        default "glacier-dark"
 *   ui.theme.syncHighContrast  default "glacier-high-contrast"
 *   ui.compactMode           bool, default false
 *   ui.zoom                  int percent, 50..300, default 100
 * The adapter implementing IAppearanceSettings over the registry lives in src/ui, because
 * src/settings (layer rank 5) cannot depend on frost::ui_model (rank 9).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/ui/model/listener.hpp"
#include "frost/ui/model/ui_scale.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace frost::ui {

// The ui.theme value that follows the operating system's light, dark and high-contrast state.
inline constexpr std::string_view kSyncWithOs = "sync-with-os";
// The theme used when no other can be loaded at start-up.
inline constexpr std::string_view kDefaultThemeId = "glacier-dark";

struct AppearanceSettings {
    std::string theme = std::string(kDefaultThemeId);
    std::string syncLightTheme = "glacier-light";
    std::string syncDarkTheme = "glacier-dark";
    std::string syncHighContrastTheme = "glacier-high-contrast";
    Density density = Density::Normal;
    std::uint16_t zoomPercent = 100;

    friend bool operator==(const AppearanceSettings&, const AppearanceSettings&) = default;
};

/*
 * Checks every field: theme ids match [a-z0-9][a-z0-9-]{0,63}; `theme` may also be kSyncWithOs,
 * the sync* fields may not (so no installed theme can be called "sync-with-os"); zoomPercent lies in [kMinZoomPercent,
 * kMaxZoomPercent]; density is a known enumerator. A valid id may still name a theme that is not installed (install()
 * falls back, apply() fails). Errors: InvalidArgument naming the field. Pure, O(1).
 */
[[nodiscard]] Result<void> validate(const AppearanceSettings& settings);

/*
 * The appearance settings store. GUI thread only. Passed by reference and never deleted
 * through this interface (protected non-virtual destructor, as frost::io's seams).
 */
class IAppearanceSettings {
public:
    // The current settings. Always passes validate(): an adapter sanitises field by field.
    [[nodiscard]] virtual AppearanceSettings load() const = 0;

    /*
     * Validates, commits, then notifies the listeners synchronously in registration order over
     * a snapshot of the listener table (listener.hpp). A store from inside a listener fails
     * with Busy and changes nothing. Errors: InvalidArgument (validate), Busy, and whatever the
     * backing store reports.
     */
    [[nodiscard]] virtual Result<void> store(const AppearanceSettings& settings) = 0;

    // Registers a listener for committed changes; destroying the token unsubscribes.
    [[nodiscard]] virtual ListenerToken subscribe(std::function<void(const AppearanceSettings&)> listener) = 0;

protected:
    IAppearanceSettings() = default;
    IAppearanceSettings(const IAppearanceSettings&) = default;
    IAppearanceSettings& operator=(const IAppearanceSettings&) = default;
    ~IAppearanceSettings() = default;
};

// An in-memory store starting from the defaults (or `initial`, which must pass validate()).
// GUI thread only.
class InMemoryAppearanceSettings final : public IAppearanceSettings {
public:
    InMemoryAppearanceSettings() = default;
    explicit InMemoryAppearanceSettings(AppearanceSettings initial);

    [[nodiscard]] AppearanceSettings load() const override { return m_settings; }
    [[nodiscard]] Result<void> store(const AppearanceSettings& settings) override;
    [[nodiscard]] ListenerToken subscribe(std::function<void(const AppearanceSettings&)> listener) override;

private:
    AppearanceSettings m_settings;
    ListenerList<const AppearanceSettings&> m_listeners;
    bool m_notifying = false;
};

} // namespace frost::ui
