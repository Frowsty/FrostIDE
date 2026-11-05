// SPDX-License-Identifier: Apache-2.0
//
// AppearanceManager (appearance_manager.hpp).

#include "frost/ui/appearance_manager.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/glacial_logo.hpp"
#include "frost/ui/model/theme_blob.hpp"

#include <QApplication>
#include <QByteArray>
#include <QFont>
#include <QFontDatabase>
#include <QScreen>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace frost::ui {

namespace {

std::string joined(std::string_view base, std::string_view a, std::string_view b = {}) {
    auto first = io::joinPath(base, a);
    if (!first) {
        return std::string(base) + "/" + std::string(a) + (b.empty() ? "" : "/" + std::string(b));
    }
    if (b.empty()) {
        return *first;
    }
    auto second = io::joinPath(*first, b);
    return second ? *second : *first + "/" + std::string(b);
}

// The primary screen's logical DPI, for the point size of a pixel-sized font.
qreal logicalDpi() {
    const QScreen* screen = QGuiApplication::primaryScreen();
    const qreal dpi = screen != nullptr ? screen->logicalDotsPerInch() : 96.0;
    return dpi > 0.0 ? dpi : 96.0;
}

} // namespace

AppearanceManager::AppearanceManager(AppearanceEnvironment environment, QObject* parent)
    : QObject(parent), m_env(std::move(environment)) {}

AppearanceManager::~AppearanceManager() = default;

std::string AppearanceManager::themePath(std::string_view id) const {
    return joined(m_env.resourceDir, "themes", std::string(id) + ".ftheme");
}

OsAppearance AppearanceManager::osAppearance() const {
    return m_env.os != nullptr ? m_env.os->current() : OsAppearance{};
}

const Theme& AppearanceManager::theme() const {
    // Precondition: install() succeeded.
    FROST_PRE(m_theme != nullptr);
    return *m_theme;
}

Result<InstallReport> AppearanceManager::install(QApplication& app) {
    // Budget: B1.
    if (m_app != nullptr) {
        return fail(ErrorCode::InvalidArgument, "AppearanceManager::install called twice");
    }
    InstallReport report;
    // 1-2. Settings and the theme id.
    const AppearanceSettings settings = m_env.settings.load();
    if (auto valid = validate(settings); !valid) {
        return std::unexpected(std::move(valid).error().withContext("while installing the appearance"));
    }
    report.requestedThemeId = resolveThemeId(settings, osAppearance());
    const UiScale scale{settings.density, settings.zoomPercent};

    // 3. The theme, falling back to the default.
    auto theme = loadThemeFile(m_env.fileSystem, themePath(report.requestedThemeId), report.requestedThemeId);
    if (!theme && report.requestedThemeId != kDefaultThemeId) {
        report.warnings.push_back("theme " + report.requestedThemeId + " could not be loaded (" +
                                  theme.error().describe() + "); using " + std::string(kDefaultThemeId));
        theme = loadThemeFile(m_env.fileSystem, themePath(kDefaultThemeId), kDefaultThemeId);
    }
    if (!theme) {
        return std::unexpected(std::move(theme).error());
    }
    report.themeId = std::string(theme->id());

    // 4. The icon atlases, preloading the scale the primary screen's Small icons will use first.
    const QScreen* screen = QGuiApplication::primaryScreen();
    const auto iconDesign =
        static_cast<std::uint8_t>(std::clamp(std::lround(theme->metric(MetricKey::IconSize, scale.density)), 1L, 255L));
    const std::array<std::uint16_t, 1> preload{
        preloadScaleFor(screen != nullptr ? screen->devicePixelRatio() : 1.0, scale.zoomPercent, iconDesign)};
    auto icons = IconAtlasSet::open(m_env.fileSystem, joined(m_env.resourceDir, "icons"), preload);
    if (!icons) {
        return std::unexpected(std::move(icons).error());
    }

    // 5. The UI fonts; Qt falls back to the system font if they cannot be registered.
    auto fonts = registerBundledFonts(m_env.fileSystem, joined(m_env.resourceDir, "fonts", "inter"), FontSet::Ui);
    if (fonts) {
        m_uiFamilies = fonts->families;
        m_uiFamily = fonts->primaryFamily;
        report.uiFamilies = fonts->families;
    } else {
        report.warnings.push_back("the UI fonts could not be registered (" + fonts.error().describe() +
                                  "); using the system font");
    }

    // 6-7. Commit: from here on the application changes.
    m_app = &app;
    app.setWindowIcon(glacialLogoIcon());
    m_theme = std::make_shared<const Theme>(std::move(*theme));
    m_icons = std::move(*icons);
    m_scale = scale;
    m_resolvedId = report.requestedThemeId;
    installStyle();
    report.scale = m_scale;

    // 8. Follow settings and, in Sync with OS mode, the OS.
    m_settingsToken = m_env.settings.subscribe([this](const AppearanceSettings& s) { onSettingsChanged(s); });
    if (m_env.os != nullptr) {
        m_osToken = m_env.os->subscribe([this](OsAppearance) {
            const AppearanceSettings current = m_env.settings.load();
            if (current.theme == kSyncWithOs) {
                onSettingsChanged(current);
            }
        });
    }
    return report;
}

void AppearanceManager::installStyle() {
    auto* style = new FrostStyle(m_theme, m_scale, m_icons, ++m_generation);
    QApplication::setStyle(style); // QApplication owns it and deletes the previous style
    QApplication::setPalette(style->standardPalette());
    QFont font = QApplication::font();
    if (!m_uiFamily.isEmpty()) {
        font.setFamilies({m_uiFamily});
    }
    const double px = m_scale.fontPx(*m_theme, MetricKey::FontUiSize);
    font.setPointSizeF(px * 72.0 / logicalDpi());
    QApplication::setFont(font);
}

Result<void> AppearanceManager::apply(const AppearanceSettings& settings) {
    if (m_app == nullptr) {
        return fail(ErrorCode::InvalidArgument, "AppearanceManager::apply before install");
    }
    if (auto valid = validate(settings); !valid) {
        return valid;
    }
    const std::string resolved = resolveThemeId(settings, osAppearance());
    const UiScale scale{settings.density, settings.zoomPercent};
    if (resolved == m_resolvedId && scale == m_scale) {
        return {};
    }
    std::shared_ptr<const Theme> theme = m_theme;
    if (resolved != m_resolvedId) {
        auto loaded = loadThemeFile(m_env.fileSystem, themePath(resolved), resolved);
        if (!loaded) {
            return std::unexpected(std::move(loaded).error());
        }
        theme = std::make_shared<const Theme>(std::move(*loaded));
    }
    m_theme = std::move(theme);
    m_scale = scale;
    m_resolvedId = resolved;
    installStyle();
    emit appearanceChanged();
    return {};
}

void AppearanceManager::onSettingsChanged(const AppearanceSettings& settings) {
    auto applied = apply(settings);
    if (applied) {
        m_lastApplyError.reset();
        return;
    }
    m_lastApplyError = applied.error();
    emit appearanceApplyFailed(QString::fromStdString(applied.error().describe()));
}

Result<void> AppearanceManager::storeZoom(std::uint16_t zoom) {
    const AppearanceSettings previous = m_env.settings.load();
    AppearanceSettings next = previous;
    next.zoomPercent = zoom;
    if (auto applied = apply(next); !applied) {
        return applied;
    }
    if (auto stored = m_env.settings.store(next); !stored) {
        // Best effort: the previous settings were valid and their theme is already loaded.
        static_cast<void>(apply(previous));
        return stored;
    }
    return {};
}

Result<void> AppearanceManager::zoomIn() { return storeZoom(nextZoomStep(m_scale.zoomPercent)); }

Result<void> AppearanceManager::zoomOut() { return storeZoom(previousZoomStep(m_scale.zoomPercent)); }

Result<void> AppearanceManager::resetZoom() { return storeZoom(100); }

Result<RegisteredFonts> AppearanceManager::ensureEditorFonts() {
    // Budget: B19. Registered once, on the first editor open.
    if (m_editorFonts) {
        return *m_editorFonts;
    }
    auto fonts =
        registerBundledFonts(m_env.fileSystem, joined(m_env.resourceDir, "fonts", "jetbrains-mono"), FontSet::Editor);
    if (!fonts) {
        return fonts;
    }
    struct BundledFontFile {
        std::string_view subdir;
        std::string_view file;
    };
    constexpr std::array<BundledFontFile, 36> kExtraEditorFontFiles{{
        {"fira-code", "FiraCode-Regular.ttf"},
        {"fira-code", "FiraCode-Medium.ttf"},
        {"fira-code", "FiraCode-SemiBold.ttf"},
        {"fira-code", "FiraCode-Bold.ttf"},
        {"cascadia-code", "CascadiaCode-Regular.ttf"},
        {"cascadia-code", "CascadiaCode-Italic.ttf"},
        {"cascadia-code", "CascadiaCode-Bold.ttf"},
        {"cascadia-code", "CascadiaCode-BoldItalic.ttf"},
        {"cascadia-code", "CascadiaMono-Regular.ttf"},
        {"cascadia-code", "CascadiaMono-Italic.ttf"},
        {"cascadia-code", "CascadiaMono-Bold.ttf"},
        {"cascadia-code", "CascadiaMono-BoldItalic.ttf"},
        {"source-code-pro", "SourceCodePro-Regular.ttf"},
        {"source-code-pro", "SourceCodePro-It.ttf"},
        {"source-code-pro", "SourceCodePro-Bold.ttf"},
        {"source-code-pro", "SourceCodePro-BoldIt.ttf"},
        {"ibm-plex-mono", "IBMPlexMono-Regular.ttf"},
        {"ibm-plex-mono", "IBMPlexMono-Italic.ttf"},
        {"ibm-plex-mono", "IBMPlexMono-Bold.ttf"},
        {"ibm-plex-mono", "IBMPlexMono-BoldItalic.ttf"},
        {"inconsolata", "Inconsolata-Regular.ttf"},
        {"inconsolata", "Inconsolata-Medium.ttf"},
        {"inconsolata", "Inconsolata-SemiBold.ttf"},
        {"inconsolata", "Inconsolata-Bold.ttf"},
        {"intel-one-mono", "IntelOneMono-Regular.ttf"},
        {"intel-one-mono", "IntelOneMono-Italic.ttf"},
        {"intel-one-mono", "IntelOneMono-Bold.ttf"},
        {"intel-one-mono", "IntelOneMono-BoldItalic.ttf"},
        {"commit-mono", "CommitMono-400-Regular.ttf"},
        {"commit-mono", "CommitMono-400-Italic.ttf"},
        {"commit-mono", "CommitMono-700-Regular.ttf"},
        {"commit-mono", "CommitMono-700-Italic.ttf"},
        {"fantasque-sans-mono", "FantasqueSansMono-Regular.ttf"},
        {"fantasque-sans-mono", "FantasqueSansMono-Italic.ttf"},
        {"fantasque-sans-mono", "FantasqueSansMono-Bold.ttf"},
        {"fantasque-sans-mono", "FantasqueSansMono-BoldItalic.ttf"},
    }};
    for (const auto& entry : kExtraEditorFontFiles) {
        const std::string dir = joined(m_env.resourceDir, "fonts", entry.subdir);
        if (auto path = io::joinPath(dir, entry.file)) {
            if (auto bytes = m_env.fileSystem.readFile(*path)) {
                const int id = QFontDatabase::addApplicationFontFromData(
                    QByteArray(bytes->data(), static_cast<qsizetype>(bytes->size())));
                if (id >= 0) {
                    for (const QString& family : QFontDatabase::applicationFontFamilies(id)) {
                        if (!fonts->families.contains(family)) {
                            fonts->families.push_back(family);
                        }
                    }
                }
            }
        }
    }
    m_editorFonts = *fonts;
    return fonts;
}

} // namespace frost::ui
