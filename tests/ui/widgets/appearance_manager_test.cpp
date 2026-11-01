// SPDX-License-Identifier: Apache-2.0
//
// AppearanceManager tests (offscreen): install, apply, the settings and OS listeners, zoom,
// snapshots, and the start-up fallback and fatal paths under fault injection (MemFs and, on
// Windows, a real file held open without sharing).

#include "frost/io/path.hpp"
#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/frost_style.hpp"

#include "support/appearance_fixture.hpp"
#include "support/scratch_dir.hpp"
#include <QApplication>
#include <QFontDatabase>
#include <QScreen>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::ui {
namespace {

AppearanceSettings withTheme(std::string_view id) {
    AppearanceSettings settings;
    settings.theme = std::string(id);
    return settings;
}

QApplication& app() { return *qobject_cast<QApplication*>(QCoreApplication::instance()); }

// Counts a manager's counter.
struct SignalCounter {
    int changed = 0;
    int failed = 0;
    QString lastFailure;
    explicit SignalCounter(AppearanceManager& manager) {
        QObject::connect(&manager, &AppearanceManager::appearanceChanged, [this] { ++changed; });
        QObject::connect(&manager, &AppearanceManager::appearanceApplyFailed, [this](const QString& text) {
            ++failed;
            lastFailure = text;
        });
    }
};

TEST(AppearanceManager, InstallsStyleFontAndIcons) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    const FrostStyle* style = FrostStyle::current();
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->theme().id(), "glacier-dark");
    EXPECT_EQ(style->generation(), 1U);
    EXPECT_EQ(installed->report.requestedThemeId, "glacier-dark");
    EXPECT_EQ(installed->report.themeId, "glacier-dark");
    EXPECT_TRUE(installed->report.warnings.empty());
    ASSERT_FALSE(installed->report.uiFamilies.isEmpty());
    EXPECT_TRUE(QApplication::font().families().contains(installed->report.uiFamilies.front()));
    EXPECT_NEAR(QApplication::font().pointSizeF() * QGuiApplication::primaryScreen()->logicalDotsPerInch() / 72.0, 13.0,
                0.01);
    ASSERT_TRUE(installed->manager->icons());
    EXPECT_TRUE(installed->manager->icons()->contains("actions.search"));
    EXPECT_EQ(QApplication::palette().color(QPalette::Window),
              QColor::fromRgba(style->theme().color(ColorKey::PanelBackground).argb));
}

TEST(AppearanceManager, InstallTwiceFails) {
    const auto installed = test::install({});
    ASSERT_TRUE(installed);
    const auto second = installed->manager->install(app());
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code(), ErrorCode::InvalidArgument);
}

TEST(AppearanceManager, ApplySwitchesThemeOnceAndEmitsOnce) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    SignalCounter counter(*installed->manager);
    ASSERT_TRUE(installed->manager->apply(withTheme("glacier-light")));
    EXPECT_EQ(counter.changed, 1);
    ASSERT_NE(FrostStyle::current(), nullptr);
    EXPECT_EQ(FrostStyle::current()->theme().id(), "glacier-light");
    EXPECT_EQ(FrostStyle::current()->generation(), 2U);
    EXPECT_EQ(installed->manager->theme().id(), "glacier-light");
    EXPECT_EQ(QApplication::palette().color(QPalette::Window),
              QColor::fromRgba(installed->manager->theme().color(ColorKey::PanelBackground).argb));
}

TEST(AppearanceManager, EqualResolvedStateNoop) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    SignalCounter counter(*installed->manager);
    const QStyle* before = QApplication::style();
    // A different settings value that resolves to the same state.
    AppearanceSettings sync = withTheme(std::string(kSyncWithOs));
    installed->os.set({ColorScheme::Dark, false});
    ASSERT_TRUE(installed->manager->apply(sync));
    ASSERT_TRUE(installed->manager->apply(withTheme("glacier-dark")));
    EXPECT_EQ(counter.changed, 0);
    EXPECT_EQ(QApplication::style(), before);
    // Invalid settings are refused.
    AppearanceSettings bad = withTheme("glacier-dark");
    bad.zoomPercent = 20;
    EXPECT_EQ(installed->manager->apply(bad).error().code(), ErrorCode::InvalidArgument);
}

TEST(AppearanceManager, FailedLoadKeepsOldTheme) {
    auto mem = test::mirrorResources();
    ASSERT_TRUE(mem);
    const auto installed = test::install(withTheme("glacier-dark"), &mem->fs, mem->root);
    ASSERT_TRUE(installed);
    SignalCounter counter(*installed->manager);
    const QStyle* before = QApplication::style();
    mem->fs.injectFault(io::FsOp::Read, mem->path("themes/glacier-light.ftheme"), ErrorCode::IoError);
    const auto applied = installed->manager->apply(withTheme("glacier-light"));
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code(), ErrorCode::IoError);
    EXPECT_EQ(QApplication::style(), before);
    EXPECT_EQ(installed->manager->theme().id(), "glacier-dark");
    EXPECT_EQ(counter.changed, 0);
    // The next attempt succeeds (no fallback, no sticky failure).
    ASSERT_TRUE(installed->manager->apply(withTheme("glacier-light")));
    EXPECT_EQ(counter.changed, 1);
}

TEST(AppearanceManager, SyncWithOsFollowsDouble) {
    const auto installed =
        test::install(withTheme(std::string(kSyncWithOs)), nullptr, {}, OsAppearance{ColorScheme::Light, false});
    ASSERT_TRUE(installed);
    EXPECT_EQ(installed->manager->theme().id(), "glacier-light");
    SignalCounter counter(*installed->manager);
    installed->os.set({ColorScheme::Dark, false});
    EXPECT_EQ(installed->manager->theme().id(), "glacier-dark");
    installed->os.set({ColorScheme::Dark, true});
    EXPECT_EQ(installed->manager->theme().id(), "glacier-high-contrast");
    installed->os.set({ColorScheme::Light, false});
    EXPECT_EQ(installed->manager->theme().id(), "glacier-light");
    // Unknown resolves to the sync dark theme.
    installed->os.set({ColorScheme::Unknown, false});
    EXPECT_EQ(installed->manager->theme().id(), "glacier-dark");
    EXPECT_EQ(counter.changed, 4);
    EXPECT_EQ(FrostStyle::current()->theme().id(), "glacier-dark");
}

TEST(AppearanceManager, OsChangesIgnoredWhenNotSync) {
    const auto installed =
        test::install(withTheme("glacier-light"), nullptr, {}, OsAppearance{ColorScheme::Light, false});
    ASSERT_TRUE(installed);
    SignalCounter counter(*installed->manager);
    installed->os.set({ColorScheme::Dark, true});
    EXPECT_EQ(installed->manager->theme().id(), "glacier-light");
    EXPECT_EQ(counter.changed, 0);
}

TEST(AppearanceManager, ZoomStepsPersistThroughStore) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    SignalCounter counter(*installed->manager);
    ASSERT_TRUE(installed->manager->zoomIn());
    EXPECT_EQ(installed->manager->scale().zoomPercent, 110);
    EXPECT_EQ(installed->settings.load().zoomPercent, 110);
    EXPECT_EQ(FrostStyle::current()->scale().zoomPercent, 110);
    EXPECT_EQ(counter.changed, 1) << "the store's own notification is a no-op";
    ASSERT_TRUE(installed->manager->zoomIn());
    EXPECT_EQ(installed->settings.load().zoomPercent, 125);
    EXPECT_EQ(FrostStyle::current()->pixelMetric(QStyle::PM_ButtonMargin), 10) << "8 px at 125%";
    ASSERT_TRUE(installed->manager->zoomOut());
    ASSERT_TRUE(installed->manager->zoomOut());
    ASSERT_TRUE(installed->manager->zoomOut());
    EXPECT_EQ(installed->settings.load().zoomPercent, 90);
    ASSERT_TRUE(installed->manager->resetZoom());
    EXPECT_EQ(installed->settings.load().zoomPercent, 100);
    EXPECT_EQ(counter.changed, 6);
    // A settings change from elsewhere (m1/settings) is followed too.
    AppearanceSettings compact = installed->settings.load();
    compact.density = Density::Compact;
    ASSERT_TRUE(installed->settings.store(compact));
    EXPECT_EQ(installed->manager->scale().density, Density::Compact);
    EXPECT_EQ(counter.changed, 7);
}

// Settings whose store always fails, as a broken backing store would.
class FailingStoreSettings final : public IAppearanceSettings {
public:
    explicit FailingStoreSettings(AppearanceSettings value) : m_value(std::move(value)) {}
    [[nodiscard]] AppearanceSettings load() const override { return m_value; }
    [[nodiscard]] Result<void> store(const AppearanceSettings&) override {
        return fail(ErrorCode::IoError, "the settings file is read-only");
    }
    [[nodiscard]] ListenerToken subscribe(std::function<void(const AppearanceSettings&)> listener) override {
        return m_listeners.add(std::move(listener));
    }

private:
    AppearanceSettings m_value;
    ListenerList<const AppearanceSettings&> m_listeners;
};

TEST(AppearanceManager, ZoomStoreFailureReverts) {
    FailingStoreSettings settings(withTheme("glacier-dark"));
    AppearanceManager manager(AppearanceEnvironment{io::realFileSystem(), test::resourceDir(), settings, nullptr});
    ASSERT_TRUE(manager.install(app()));
    const auto zoomed = manager.zoomIn();
    ASSERT_FALSE(zoomed);
    EXPECT_EQ(zoomed.error().code(), ErrorCode::IoError);
    EXPECT_EQ(manager.scale().zoomPercent, 100);
    EXPECT_EQ(FrostStyle::current()->scale().zoomPercent, 100);
}

TEST(AppearanceManager, ListenerApplyFailureEmitsSignal) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    SignalCounter counter(*installed->manager);
    ASSERT_TRUE(installed->settings.store(withTheme("no-such-theme")));
    EXPECT_EQ(counter.failed, 1);
    EXPECT_NE(counter.lastFailure.indexOf(QStringLiteral("no-such-theme")), -1) << counter.lastFailure.toStdString();
    ASSERT_TRUE(installed->manager->lastApplyError());
    EXPECT_EQ(installed->manager->lastApplyError()->code(), ErrorCode::NotFound);
    EXPECT_EQ(installed->manager->theme().id(), "glacier-dark");
    EXPECT_EQ(FrostStyle::current()->theme().id(), "glacier-dark");
    // A later good change clears the error.
    ASSERT_TRUE(installed->settings.store(withTheme("glacier-light")));
    EXPECT_FALSE(installed->manager->lastApplyError());
    EXPECT_EQ(counter.changed, 1);
}

TEST(AppearanceManager, ThemeSnapshotOutlivesSwitch) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    const std::shared_ptr<const Theme> snapshot = installed->manager->themeSnapshot();
    const Rgba island = snapshot->color(ColorKey::IslandBackground);
    ASSERT_TRUE(installed->manager->apply(withTheme("glacier-light")));
    ASSERT_TRUE(installed->manager->apply(withTheme("glacier-classic")));
    EXPECT_EQ(snapshot->id(), "glacier-dark");
    EXPECT_EQ(snapshot->color(ColorKey::IslandBackground), island);
}

// The observable global state a failed install must leave alone.
struct GlobalState {
    const QStyle* style = QApplication::style();
    QPalette palette = QApplication::palette();
    QFont font = QApplication::font();
    QStringList families = QFontDatabase::families();
    void expectUnchanged() const {
        EXPECT_EQ(QApplication::style(), style);
        EXPECT_EQ(QApplication::palette(), palette);
        EXPECT_EQ(QApplication::font(), font);
        EXPECT_EQ(QFontDatabase::families(), families);
    }
};

Result<InstallReport> tryInstall(io::IFileSystem& fs, const std::string& root, const AppearanceSettings& settings,
                                 InMemoryAppearanceSettings& store, std::unique_ptr<AppearanceManager>& manager) {
    EXPECT_TRUE(store.store(settings));
    manager = std::make_unique<AppearanceManager>(AppearanceEnvironment{fs, root, store, nullptr});
    return manager->install(app());
}

TEST(AppearanceManager, InstallFallbackAndFatal) {
    // Make sure a FrostStyle is current, so "unchanged" is meaningful.
    const auto baseline = test::install(withTheme("glacier-classic"));
    ASSERT_TRUE(baseline);

    // A missing, truncated or busy chosen theme falls back to the default with a warning.
    for (const int scenario : {0, 1, 2}) {
        SCOPED_TRACE(scenario);
        auto mem =
            test::mirrorResources(scenario == 0 ? std::initializer_list<std::string_view>{"themes/glacier-light.ftheme"}
                                                : std::initializer_list<std::string_view>{});
        ASSERT_TRUE(mem);
        const std::string light = mem->path("themes/glacier-light.ftheme");
        if (scenario == 1) {
            const std::string bytes = *mem->fs.readFile(light);
            ASSERT_TRUE(mem->fs.writeFileAtomic(light, bytes.substr(0, bytes.size() / 2)));
        } else if (scenario == 2) {
            mem->fs.injectFault(io::FsOp::Read, light, ErrorCode::Busy);
        }
        InMemoryAppearanceSettings store;
        std::unique_ptr<AppearanceManager> manager;
        const auto report = tryInstall(mem->fs, mem->root, withTheme("glacier-light"), store, manager);
        ASSERT_TRUE(report) << report.error().describe();
        EXPECT_EQ(report->requestedThemeId, "glacier-light");
        EXPECT_EQ(report->themeId, "glacier-dark");
        ASSERT_EQ(report->warnings.size(), 1U);
        EXPECT_NE(report->warnings[0].find("glacier-light"), std::string::npos) << report->warnings[0];
    }
    const auto restored = test::install(withTheme("glacier-classic"));
    ASSERT_TRUE(restored);

    // Fatal: a missing or truncated default theme, a missing primary atlas.
    struct Fatal {
        const char* name;
        std::string_view skip;
        bool truncateDefault;
        ErrorCode code;
    };
    const Fatal cases[] = {
        {"missing default", "themes/glacier-dark.ftheme", false, ErrorCode::NotFound},
        {"truncated default", "", true, ErrorCode::Corrupt},
        {"missing primary atlas", "icons/icons-100.fia", false, ErrorCode::NotFound},
    };
    for (const Fatal& c : cases) {
        SCOPED_TRACE(c.name);
        auto mem = c.skip.empty() ? test::mirrorResources() : test::mirrorResources({c.skip});
        ASSERT_TRUE(mem);
        if (c.truncateDefault) {
            const std::string dark = mem->path("themes/glacier-dark.ftheme");
            ASSERT_TRUE(mem->fs.writeFileAtomic(dark, mem->fs.readFile(dark)->substr(0, 40)));
        }
        const GlobalState before;
        InMemoryAppearanceSettings store;
        std::unique_ptr<AppearanceManager> manager;
        const auto report = tryInstall(mem->fs, mem->root, {}, store, manager);
        ASSERT_FALSE(report);
        EXPECT_EQ(report.error().code(), c.code) << report.error().describe();
        before.expectUnchanged();
    }

#ifdef _WIN32
    // A default theme held open without sharing is Busy, and fatal.
    frost::test::ScratchDir scratch("appearance");
    const std::string resources = test::resourceDir();
    std::filesystem::copy(std::filesystem::path(std::u8string(resources.begin(), resources.end())),
                          std::filesystem::path(scratch.path()), std::filesystem::copy_options::recursive);
    const std::filesystem::path dark = std::filesystem::path(scratch.path()) / "themes" / "glacier-dark.ftheme";
    const HANDLE held =
        CreateFileW(dark.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(held, INVALID_HANDLE_VALUE);
    {
        const GlobalState before;
        InMemoryAppearanceSettings store;
        std::unique_ptr<AppearanceManager> manager;
        const auto report = tryInstall(io::realFileSystem(), scratch.path(), {}, store, manager);
        ASSERT_FALSE(report);
        EXPECT_EQ(report.error().code(), ErrorCode::Busy) << report.error().describe();
        before.expectUnchanged();
    }
    CloseHandle(held);
#endif
}

} // namespace
} // namespace frost::ui
