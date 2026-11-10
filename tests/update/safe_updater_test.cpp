// SPDX-License-Identifier: Apache-2.0
// Real-process integration tests for SafeUpdater with health check and automatic rollback (FND-11).

#include <frost/io/file_system.hpp>
#include <frost/io/path.hpp>
#include <frost/io/process.hpp>
#include <frost/update/safe_updater.hpp>
#include <frost/update/verification.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <sodium.h>

namespace frost::update {
namespace {

std::string mustJoin(std::string_view base, std::string_view name) {
    auto res = io::joinPath(base, name);
    EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
    return res ? *res : std::string{};
}

class SafeUpdaterTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_GE(sodium_init(), 0);
        const auto* testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string testName = testInfo ? testInfo->name() : "unknown";
        m_root = std::filesystem::temp_directory_path() / ("frost_safe_updater_" + testName);
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directories(m_root);

        const auto helperBytes = m_fs.readFile(FROST_UPDATE_TEST_HELPER_PATH);
        ASSERT_TRUE(helperBytes.has_value()) << "Could not read helper binary at " << FROST_UPDATE_TEST_HELPER_PATH
                                             << ": " << helperBytes.error().message();
        m_baseHelperImage = std::move(*helperBytes);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    // Creates a runnable executable image by appending a metadata trailer to the helper binary.
    std::string makeHelperBinary(std::string_view version, int selftestExit, int runExit = 0) const {
        std::string image = m_baseHelperImage;
        const std::string trailer = "\nFROST_HELPER_META:version=" + std::string(version) +
                                    ";selftest_exit=" + std::to_string(selftestExit) +
                                    ";run_exit=" + std::to_string(runExit) + "\n";
        image.append(trailer);
        return image;
    }

    static Release makeReleaseForBytes(std::string_view bytes, std::string version, std::uint64_t build) {
        Release rel{};
        rel.version = std::move(version);
        rel.build = build;
        rel.channel = "stable";
        rel.url = "https://github.com/Frowsty/FrostIDE/releases/download/v" + rel.version + "/payload.bin";
        rel.size = bytes.size();
        crypto_hash_sha256(rel.digest.data(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
        return rel;
    }

    void writeFileWithParents(const std::string& path, std::string_view bytes) {
        const auto norm = io::normalizePath(path);
        ASSERT_TRUE(norm.has_value()) << norm.error().message();
        const auto parent = io::parentPath(*norm);
        if (!parent.empty()) {
            ASSERT_TRUE(m_fs.createDirectories(parent).has_value());
        }
        ASSERT_TRUE(m_fs.writeFileAtomic(*norm, bytes).has_value());
    }

    void writeExecutable(const std::string& path, std::string_view bytes) {
        writeFileWithParents(path, bytes);
#ifndef _WIN32
        std::error_code ec;
        std::filesystem::permissions(path,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                         std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
                                         std::filesystem::perms::group_exec,
                                     std::filesystem::perm_options::replace, ec);
#endif
    }

    Result<io::ProcessResult> runInstalledBinary(const std::string& installDir, std::vector<std::string> args = {}) {
        io::ProcessSpec spec;
        spec.executable = mustJoin(installDir, kDefaultExecutableName);
        spec.arguments = std::move(args);
        spec.workingDirectory = installDir;
        return io::runToCompletion(m_launcher, spec, {}, std::chrono::seconds{10});
    }

    io::IFileSystem& m_fs = io::realFileSystem();
    io::IProcessLauncher& m_launcher = io::realProcessLauncher();
    std::filesystem::path m_root;
    std::string m_baseHelperImage;
};

TEST_F(SafeUpdaterTest, HealthyUpdateInstallsPassesSelftestAndStartsNewVersion) {
    const std::string installDir = *io::fromStdPath(m_root / "install");
    const std::string backupDir = *io::fromStdPath(m_root / "backup");
    const std::string payloadPath = *io::fromStdPath(m_root / "v2_payload.bin");

    // 1. Seed existing v1.0.0 installation.
    const auto v1Image = makeHelperBinary("1.0.0", /*selftestExit=*/0, /*runExit=*/0);
    const std::string installedExe = mustJoin(installDir, kDefaultExecutableName);
    writeExecutable(installedExe, v1Image);

    const auto v1Run = runInstalledBinary(installDir);
    ASSERT_TRUE(v1Run.has_value()) << v1Run.error().message();
    EXPECT_EQ(v1Run->exitCode, 0);
    EXPECT_NE(v1Run->stdoutBytes.find("FrostIDE 1.0.0 running"), std::string::npos);

    // 2. Stage verified v2.0.0 binary payload.
    const auto v2Image = makeHelperBinary("2.0.0", /*selftestExit=*/0, /*runExit=*/0);
    writeExecutable(payloadPath, v2Image);
    const Release v2Release = makeReleaseForBytes(v2Image, "2.0.0", 200);

    SafeUpdateSpec spec;
    spec.installDir = installDir;
    spec.backupDir = backupDir;
    spec.payloadPath = payloadPath;
    spec.expectedRelease = v2Release;
    spec.executableRelativePath = std::string(kDefaultExecutableName);
    spec.healthCheckArgs = {"--selftest"};
    spec.healthCheckTimeout = std::chrono::seconds{10};
    spec.launchDetachedAfterInstall = false;

    SafeUpdater updater(m_fs, m_launcher);
    const auto outcome = updater.applyUpdate(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message();
    EXPECT_TRUE(outcome->applied);
    EXPECT_FALSE(outcome->rolledBack);
    EXPECT_TRUE(outcome->healthCheckPassed);
    EXPECT_EQ(outcome->healthCheckExitCode, 0);
    EXPECT_EQ(outcome->installedVersion, "2.0.0");
    EXPECT_EQ(outcome->installedBuild, 200u);
    EXPECT_TRUE(outcome->failureReason.empty());

    // 3. Verify installed-release.json marker was written and new version runs cleanly.
    const std::string markerPath = mustJoin(installDir, "installed-release.json");
    const auto markerText = m_fs.readFile(markerPath);
    ASSERT_TRUE(markerText.has_value()) << markerText.error().message();
    EXPECT_NE(markerText->find("\"version\":\"2.0.0\""), std::string::npos);
    EXPECT_NE(markerText->find("\"build\":200"), std::string::npos);

    const auto v2Run = runInstalledBinary(installDir);
    ASSERT_TRUE(v2Run.has_value()) << v2Run.error().message();
    EXPECT_EQ(v2Run->exitCode, 0);
    EXPECT_NE(v2Run->stdoutBytes.find("FrostIDE 2.0.0 running"), std::string::npos);
}

TEST_F(SafeUpdaterTest, FailedHealthCheckRollsBackAndPreviousVersionStarts) {
    const std::string installDir = *io::fromStdPath(m_root / "install");
    const std::string backupDir = *io::fromStdPath(m_root / "backup");
    const std::string payloadPath = *io::fromStdPath(m_root / "v2_broken_payload.bin");

    // 1. Seed a healthy v1.0.0 installation with an extra configuration asset.
    const auto v1Image = makeHelperBinary("1.0.0", /*selftestExit=*/0, /*runExit=*/0);
    const std::string installedExe = mustJoin(installDir, kDefaultExecutableName);
    writeExecutable(installedExe, v1Image);
    const std::string configPath = mustJoin(mustJoin(installDir, "assets"), "version.txt");
    writeFileWithParents(configPath, "installed=1.0.0\n");

    // 2. Stage a broken v2.0.0-broken binary whose --selftest exits with code 42.
    const auto brokenV2Image = makeHelperBinary("2.0.0-broken", /*selftestExit=*/42, /*runExit=*/42);
    writeExecutable(payloadPath, brokenV2Image);
    const Release brokenRelease = makeReleaseForBytes(brokenV2Image, "2.0.0-broken", 200);

    SafeUpdateSpec spec;
    spec.installDir = installDir;
    spec.backupDir = backupDir;
    spec.payloadPath = payloadPath;
    spec.expectedRelease = brokenRelease;
    spec.executableRelativePath = std::string(kDefaultExecutableName);
    spec.healthCheckArgs = {"--selftest"};
    spec.healthCheckTimeout = std::chrono::seconds{10};

    SafeUpdater updater(m_fs, m_launcher);
    const auto outcome = updater.applyUpdate(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message();

    // 3. Verify rollback outcome fields.
    EXPECT_FALSE(outcome->applied);
    EXPECT_TRUE(outcome->rolledBack);
    EXPECT_FALSE(outcome->healthCheckPassed);
    EXPECT_EQ(outcome->healthCheckExitCode, 42);
    EXPECT_TRUE(outcome->previousVersionStarted);
    EXPECT_EQ(outcome->rollbackStartExitCode, 0);
    EXPECT_NE(outcome->failureReason.find("42"), std::string::npos);

    // 4. Verify the installed binary and companion asset were restored byte-for-byte to v1.0.0.
    const auto restoredBytes = m_fs.readFile(installedExe);
    ASSERT_TRUE(restoredBytes.has_value()) << restoredBytes.error().message();
    EXPECT_EQ(*restoredBytes, v1Image);

    const auto restoredConfig = m_fs.readFile(configPath);
    ASSERT_TRUE(restoredConfig.has_value()) << restoredConfig.error().message();
    EXPECT_EQ(*restoredConfig, "installed=1.0.0\n");

    // 5. Launch the rolled-back binary and confirm the previous version starts and exits 0.
    const auto v1AfterRollback = runInstalledBinary(installDir);
    ASSERT_TRUE(v1AfterRollback.has_value()) << v1AfterRollback.error().message();
    EXPECT_EQ(v1AfterRollback->exitCode, 0);
    EXPECT_NE(v1AfterRollback->stdoutBytes.find("FrostIDE 1.0.0 running"), std::string::npos);
}

TEST_F(SafeUpdaterTest, StagedDirectoryUpdateRollsBackAddedFilesOnFailureAndCommitsOnSuccess) {
    const std::string installDir = *io::fromStdPath(m_root / "install");
    const std::string backupDir = *io::fromStdPath(m_root / "backup");
    const std::string brokenStagedDir = *io::fromStdPath(m_root / "staged_broken");
    const std::string healthyStagedDir = *io::fromStdPath(m_root / "staged_healthy");

    // 1. Seed v1.0.0 installation.
    const auto v1Image = makeHelperBinary("1.0.0", /*selftestExit=*/0, /*runExit=*/0);
    const std::string installedExe = mustJoin(installDir, kDefaultExecutableName);
    writeExecutable(installedExe, v1Image);
    writeFileWithParents(mustJoin(installDir, "readme.txt"), "v1 readme");

    // 2. Build a broken staged directory that adds a new file and overwrites readme.txt.
    const auto brokenV2Image = makeHelperBinary("2.0.0-bad-dir", /*selftestExit=*/9, /*runExit=*/9);
    writeExecutable(mustJoin(brokenStagedDir, kDefaultExecutableName), brokenV2Image);
    writeFileWithParents(mustJoin(brokenStagedDir, "readme.txt"), "v2 broken readme");
    writeFileWithParents(mustJoin(mustJoin(brokenStagedDir, "plugins"), "bad.dll"), "bad-plugin");

    SafeUpdateSpec brokenSpec;
    brokenSpec.installDir = installDir;
    brokenSpec.backupDir = backupDir;
    brokenSpec.payloadPath = brokenStagedDir;
    brokenSpec.expectedRelease = makeReleaseForBytes(brokenV2Image, "2.0.0-bad-dir", 201);
    brokenSpec.executableRelativePath = std::string(kDefaultExecutableName);

    SafeUpdater updater(m_fs, m_launcher);
    const auto brokenOutcome = updater.applyUpdate(brokenSpec);
    ASSERT_TRUE(brokenOutcome.has_value()) << brokenOutcome.error().message();
    EXPECT_TRUE(brokenOutcome->rolledBack);
    EXPECT_TRUE(brokenOutcome->previousVersionStarted);
    const auto badPluginExists = m_fs.exists(mustJoin(mustJoin(installDir, "plugins"), "bad.dll"));
    ASSERT_TRUE(badPluginExists.has_value());
    EXPECT_FALSE(*badPluginExists);
    const auto readmeAfterRollback = m_fs.readFile(mustJoin(installDir, "readme.txt"));
    ASSERT_TRUE(readmeAfterRollback.has_value());
    EXPECT_EQ(*readmeAfterRollback, "v1 readme");

    // 3. Now apply a healthy staged directory update (v2.1.0).
    const auto healthyV2Image = makeHelperBinary("2.1.0", /*selftestExit=*/0, /*runExit=*/0);
    writeExecutable(mustJoin(healthyStagedDir, kDefaultExecutableName), healthyV2Image);
    writeFileWithParents(mustJoin(healthyStagedDir, "readme.txt"), "v2.1 readme");
    writeFileWithParents(mustJoin(mustJoin(healthyStagedDir, "plugins"), "good.dll"), "good-plugin");

    SafeUpdateSpec healthySpec;
    healthySpec.installDir = installDir;
    healthySpec.backupDir = backupDir;
    healthySpec.payloadPath = healthyStagedDir;
    healthySpec.expectedRelease = makeReleaseForBytes(healthyV2Image, "2.1.0", 210);
    healthySpec.executableRelativePath = std::string(kDefaultExecutableName);

    const auto healthyOutcome = updater.applyUpdate(healthySpec);
    ASSERT_TRUE(healthyOutcome.has_value()) << healthyOutcome.error().message();
    EXPECT_TRUE(healthyOutcome->applied);
    EXPECT_FALSE(healthyOutcome->rolledBack);
    EXPECT_TRUE(healthyOutcome->healthCheckPassed);
    const auto goodPluginExists = m_fs.exists(mustJoin(mustJoin(installDir, "plugins"), "good.dll"));
    ASSERT_TRUE(goodPluginExists.has_value());
    EXPECT_TRUE(*goodPluginExists);

    const auto runV21 = runInstalledBinary(installDir);
    ASSERT_TRUE(runV21.has_value()) << runV21.error().message();
    EXPECT_EQ(runV21->exitCode, 0);
    EXPECT_NE(runV21->stdoutBytes.find("FrostIDE 2.1.0 running"), std::string::npos);
}

TEST_F(SafeUpdaterTest, TamperedPayloadDigestIsRejectedBeforeTouchingInstallDir) {
    const std::string installDir = *io::fromStdPath(m_root / "install");
    const std::string backupDir = *io::fromStdPath(m_root / "backup");
    const std::string payloadPath = *io::fromStdPath(m_root / "tampered_payload.bin");

    const auto v1Image = makeHelperBinary("1.0.0", /*selftestExit=*/0, /*runExit=*/0);
    const std::string installedExe = mustJoin(installDir, kDefaultExecutableName);
    writeExecutable(installedExe, v1Image);

    auto v2Image = makeHelperBinary("2.0.0", /*selftestExit=*/0, /*runExit=*/0);
    const Release v2Release = makeReleaseForBytes(v2Image, "2.0.0", 200);

    // Flip one byte in the staged payload file after computing the signed release digest.
    v2Image.back() = static_cast<char>(v2Image.back() ^ 0xFF);
    writeExecutable(payloadPath, v2Image);

    SafeUpdateSpec spec;
    spec.installDir = installDir;
    spec.backupDir = backupDir;
    spec.payloadPath = payloadPath;
    spec.expectedRelease = v2Release;
    spec.executableRelativePath = std::string(kDefaultExecutableName);

    SafeUpdater updater(m_fs, m_launcher);
    const auto outcome = updater.applyUpdate(spec);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code(), ErrorCode::Corrupt);

    // Verify v1 installation was never modified.
    const auto currentBytes = m_fs.readFile(installedExe);
    ASSERT_TRUE(currentBytes.has_value());
    EXPECT_EQ(*currentBytes, v1Image);
}

TEST_F(SafeUpdaterTest, MissingExecutableInStagedDirectoryRollsBackAndStartsPreviousVersion) {
    const std::string installDir = *io::fromStdPath(m_root / "install");
    const std::string backupDir = *io::fromStdPath(m_root / "backup");
    const std::string incompleteStagedDir = *io::fromStdPath(m_root / "staged_incomplete");

    // Seed v1.0.0 installation.
    const auto v1Image = makeHelperBinary("1.0.0", /*selftestExit=*/0, /*runExit=*/0);
    const std::string installedExe = mustJoin(installDir, kDefaultExecutableName);
    writeExecutable(installedExe, v1Image);

    // Stage a directory that has a readme file but is missing the main executable.
    writeFileWithParents(mustJoin(incompleteStagedDir, "readme.txt"), "incomplete update");

    SafeUpdateSpec spec;
    spec.installDir = installDir;
    spec.backupDir = backupDir;
    spec.payloadPath = incompleteStagedDir;
    spec.executableRelativePath = std::string(kDefaultExecutableName);
    spec.healthCheckArgs = {"--selftest"};
    spec.healthCheckTimeout = std::chrono::seconds{5};

    SafeUpdater updater(m_fs, m_launcher);
    const auto outcome = updater.applyUpdate(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message();
    EXPECT_FALSE(outcome->applied);
    EXPECT_TRUE(outcome->rolledBack);
    EXPECT_FALSE(outcome->healthCheckPassed);
    EXPECT_TRUE(outcome->previousVersionStarted);

    const auto restoredBytes = m_fs.readFile(installedExe);
    ASSERT_TRUE(restoredBytes.has_value());
    EXPECT_EQ(*restoredBytes, v1Image);
}

} // namespace
} // namespace frost::update
