// SPDX-License-Identifier: Apache-2.0
//
// AppearanceManager: installs and switches FrostIDE's appearance (theme, density, zoom, icons,
// fonts) on a QApplication, driven by the IAppearanceSettings seam that m1/settings binds later
// and by the OS appearance in Sync with OS mode (ADR 0015 decisions 5 and 12).
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/ui/fonts.hpp"
#include "frost/ui/icon_atlas_set.hpp"
#include "frost/ui/model/appearance_settings.hpp"
#include "frost/ui/model/os_appearance.hpp"
#include "frost/ui/model/theme.hpp"

#include <QObject>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QApplication;

namespace frost::ui {

/*
 * What the manager reads from. The referenced objects must outlive the manager. `os` may be
 * null: Sync with OS then resolves as Unknown (syncDarkTheme). `resourceDir` is absolute UTF-8:
 * the app passes applicationDirPath() + "/resources", tests FROST_RESOURCE_DIR. No environment
 * variable can redirect resources.
 */
struct AppearanceEnvironment {
    io::IFileSystem& fileSystem;
    std::string resourceDir;
    IAppearanceSettings& settings;
    IOsAppearanceSource* os = nullptr;
};

struct InstallReport {
    std::string requestedThemeId; // what the settings and the OS resolved to
    std::string themeId;          // what was installed (kDefaultThemeId after a fallback)
    UiScale scale;
    QStringList uiFamilies;
    std::vector<std::string> warnings; // a theme fallback, a font registration failure
};

/*
 * GUI thread only. apply() must never be called from inside QStyle, paint or QIconEngine code;
 * calling it from a slot connected to a widget's signal is fine. Never calls
 * QStyleHints::setColorScheme().
 */
class AppearanceManager final : public QObject {
    Q_OBJECT

public:
    explicit AppearanceManager(AppearanceEnvironment environment, QObject* parent = nullptr);
    ~AppearanceManager() override;

    /*
     * Installs the appearance; call once, after QApplication exists and before the first widget
     * is shown. In order:
     *   1. load the settings;  2. resolve the theme id (settings and OS);
     *   3. load <res>/themes/<id>.ftheme; on any error fall back to kDefaultThemeId with a
     *      warning naming the error; if the default fails too, return that error;
     *   4. IconAtlasSet::open(<res>/icons) preloading the primary screen's effective scale;
     *   5. register the UI fonts (<res>/fonts/inter); a failure is a warning;
     *   6. setStyle(new FrostStyle), then QApplication::setPalette(standardPalette());
     *   7. QApplication::setFont(the Inter family at Font.uiSize x zoom);
     *   8. subscribe to settings and OS changes.
     * Steps 1-4 have no global side effects, so an error leaves the application untouched.
     * Errors: InvalidArgument (a second call), and those of loadThemeFile and IconAtlasSet::open.
     * Budget: B1.
     */
    [[nodiscard]] Result<InstallReport> install(QApplication& app);

    /*
     * Validates and resolves `settings`. When the resolved theme id, density and zoom equal the
     * current ones, nothing happens. Otherwise (loading the new theme first when its id changed;
     * there is no fallback at run time) one new FrostStyle is installed (generation + 1), the
     * palette and font are set, and appearanceChanged() is emitted exactly once. The atlas set
     * is kept, so existing QIcons stay valid. On error the old state is kept.
     * Errors: InvalidArgument (validate, or install() not called), those of loadThemeFile.
     */
    [[nodiscard]] Result<void> apply(const AppearanceSettings& settings);

    /*
     * IDE zoom (decision 12): compute the next or previous kZoomSteps value (or 100), apply()
     * it, then store() it through IAppearanceSettings; the settings listener then sees an
     * unchanged resolved state. If the store fails, the previous settings are re-applied and the
     * store's error returned.
     */
    [[nodiscard]] Result<void> zoomIn();
    [[nodiscard]] Result<void> zoomOut();
    [[nodiscard]] Result<void> resetZoom();

    // Registers JetBrains Mono on first use (m1/editor-view calls it when the first editor
    // opens) and returns its families; later calls return the same. Budget: B19 (never on the
    // start-up path).
    [[nodiscard]] Result<RegisteredFonts> ensureEditorFonts();

    // The current theme; valid until the next appearanceChanged().
    [[nodiscard]] const Theme& theme() const;
    [[nodiscard]] std::shared_ptr<const Theme> themeSnapshot() const { return m_theme; }
    [[nodiscard]] UiScale scale() const noexcept { return m_scale; }
    [[nodiscard]] std::shared_ptr<const IconAtlasSet> icons() const { return m_icons; }
    [[nodiscard]] const std::optional<Error>& lastApplyError() const noexcept { return m_lastApplyError; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }

signals:
    void appearanceChanged();
    void appearanceApplyFailed(QString description);

private:
    [[nodiscard]] std::string themePath(std::string_view id) const;
    [[nodiscard]] OsAppearance osAppearance() const;
    void installStyle();
    [[nodiscard]] Result<void> storeZoom(std::uint16_t zoom);
    void onSettingsChanged(const AppearanceSettings& settings);

    AppearanceEnvironment m_env;
    QApplication* m_app = nullptr;
    std::shared_ptr<const Theme> m_theme;
    std::shared_ptr<const IconAtlasSet> m_icons;
    UiScale m_scale;
    std::string m_resolvedId;
    std::uint64_t m_generation = 0;
    QStringList m_uiFamilies;
    QString m_uiFamily;
    std::optional<RegisteredFonts> m_editorFonts;
    std::optional<Error> m_lastApplyError;
    ListenerToken m_settingsToken;
    ListenerToken m_osToken;
};

} // namespace frost::ui
