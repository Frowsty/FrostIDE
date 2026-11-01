// SPDX-License-Identifier: Apache-2.0
//
// Shared set-up for the Qt UI tests: the built resource directory (FROST_RESOURCE_DIR), an
// installed AppearanceManager with in-memory settings and a fake OS source, and the test-only
// font settings the screenshot goldens rely on (ADR 0015 decision 11).
#pragma once

#include "frost/core/clock.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/model/appearance_settings.hpp"

#include "support/fake_os_appearance.hpp"

#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

namespace frost::ui::test {

// <frost's output directory>/resources for this build configuration (FROST_RESOURCE_DIR).
[[nodiscard]] std::string resourceDir();
// The source tree root (FROST_SOURCE_DIR).
[[nodiscard]] std::string sourceDir();

/*
 * One installed appearance. The members are declared in dependency order (the manager refers
 * to the settings and the OS source), so destruction is safe. Creating a second Installed
 * replaces the application's style; the previous manager keeps working on its own state.
 */
struct Installed {
    InMemoryAppearanceSettings settings;
    FakeOsAppearance os;
    std::unique_ptr<AppearanceManager> manager;
    InstallReport report;
};

/*
 * Creates an AppearanceManager over `fs` (realFileSystem() by default) and `resources`
 * (resourceDir() by default), stores `settings`, and installs it on qApp. Fails the current
 * test (ADD_FAILURE) and returns nullptr when install() fails.
 */
[[nodiscard]] std::unique_ptr<Installed> install(const AppearanceSettings& settings, io::IFileSystem* fs = nullptr,
                                                 std::string resources = {}, OsAppearance os = {});

/*
 * An in-memory copy of the built resource directory, for fault injection (MemFs, native path
 * style). `root` is C:\res on Windows and /res elsewhere. An installed FrostStyle keeps its
 * IconAtlasSet, which refers to this file system, so the destructor replaces a current
 * FrostStyle with Fusion: no style outlives the MemFs it reads. Declare it before the
 * AppearanceManager or IconAtlasSet that uses it.
 */
struct MemResources {
    MemResources() = default;
    MemResources(const MemResources&) = delete ("the MemFs is referred to by address");
    MemResources& operator=(const MemResources&) = delete ("the MemFs is referred to by address");
    ~MemResources();

    ManualClock clock;
    io::MemFs fs{clock};
    std::string root;
    // root joined with a relative path using '/' separators ("themes/glacier-dark.ftheme").
    [[nodiscard]] std::string path(std::string_view relative) const;
};
// Copies every file of resourceDir() into a new MemResources, skipping relative paths that
// `skip` names. Fails the current test on an error.
[[nodiscard]] std::unique_ptr<MemResources> mirrorResources(std::initializer_list<std::string_view> skip = {});

// Sets QFont::PreferNoHinting and QFont::NoSubpixelAntialias on the application font, so the
// goldens test style, layout and colour rather than the OS text rasteriser. Re-apply after
// every appearance change (apply() sets a new application font).
void applyTestFontSettings();

} // namespace frost::ui::test
