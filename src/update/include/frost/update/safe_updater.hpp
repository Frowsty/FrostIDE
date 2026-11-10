// SPDX-License-Identifier: Apache-2.0
// Transactional update installer with health check and automatic rollback (FND-11).
#pragma once

#include <frost/core/clock.hpp>
#include <frost/core/error.hpp>
#include <frost/io/file_system.hpp>
#include <frost/io/process.hpp>
#include <frost/update/manifest.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::update {

inline constexpr std::string_view kDefaultExecutableName =
#ifdef _WIN32
    "frost.exe";
#else
    "frost";
#endif

struct SafeUpdateSpec {
    // Target installation directory (absolute UTF-8 path).
    std::string installDir{};
    // Backup directory where the previous installation is preserved for rollback (absolute UTF-8 path).
    std::string backupDir{};
    // Candidate update payload: a .zip archive, a staged directory, or a single executable file (absolute UTF-8 path).
    std::string payloadPath{};
    // Optional authenticated release metadata. When set, regular-file payloads are verified against
    // expectedRelease->size and expectedRelease->digest via verifyPayload() before any file is moved.
    std::optional<Release> expectedRelease{};
    // Relative path of the main binary inside installDir.
    std::string executableRelativePath{kDefaultExecutableName};
    // Arguments passed to the newly installed binary for its health check.
    std::vector<std::string> healthCheckArgs{"--selftest"};
    // Arguments passed to the restored previous binary to verify it starts after a rollback.
    // When empty, healthCheckArgs is used.
    std::vector<std::string> rollbackVerifyArgs{};
    // Environment overrides applied to health-check and rollback-verify launches.
    // QT_QPA_PLATFORM=offscreen is always injected unless already specified.
    std::vector<std::pair<std::string, std::optional<std::string>>> healthCheckEnvironment{};
    IClock::Duration healthCheckTimeout = std::chrono::seconds(30);
    IClock::Duration extractTimeout = std::chrono::seconds(120);
    // When true and a rollback occurs, launches the restored previous version via IProcessLauncher
    // and verifies that it exits with code 0.
    bool verifyPreviousVersionStartsOnRollback = true;
    // When true, starts the active executable detached (killOnClose = false) after commit.
    bool launchDetachedAfterInstall = false;
    // Arguments for the detached launch when launchDetachedAfterInstall is true.
    std::vector<std::string> detachedLaunchArgs{};
    // Keep backupDir on disk after a successful update commit.
    bool keepBackupOnSuccess = false;
};

struct UpdateOutcome {
    bool applied = false;
    bool rolledBack = false;
    bool healthCheckPassed = false;
    bool previousVersionStarted = false;
    int healthCheckExitCode = -1;
    int rollbackStartExitCode = -1;
    std::string installedVersion{};
    std::uint64_t installedBuild = 0;
    std::string activeExecutablePath{};
    std::string failureReason{};
    std::string processStdout{};
    std::string processStderr{};
};

class SafeUpdater final {
public:
    SafeUpdater(io::IFileSystem& fs, io::IProcessLauncher& launcher) noexcept : m_fs(fs), m_launcher(launcher) {}

    // Verifies a regular payload file against `release.size` and `release.digest`.
    [[nodiscard]] Result<void> verifyPayloadFile(std::string_view payloadPath, const Release& release) const;

    // Applies the update described by `spec`, running the health check on the new executable and
    // rolling back to the previous installation (and starting it) if the health check fails.
    [[nodiscard]] Result<UpdateOutcome> applyUpdate(const SafeUpdateSpec& spec) const;

private:
    [[nodiscard]] Result<void> stagePayload(const SafeUpdateSpec& spec, std::string_view stageDir) const;
    [[nodiscard]] Result<void> extractZipArchive(std::string_view zipPath, std::string_view destDir,
                                                 IClock::Duration timeout) const;
    [[nodiscard]] Result<void> copyDirectoryTree(std::string_view srcDir, std::string_view dstDir) const;
    [[nodiscard]] Result<void> moveOrCopyTree(std::string_view srcDir, std::string_view dstDir) const;

    io::IFileSystem& m_fs;
    io::IProcessLauncher& m_launcher;
};

} // namespace frost::update
