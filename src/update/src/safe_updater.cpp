// SPDX-License-Identifier: Apache-2.0
// Transactional update installation, health-check probing, and automatic rollback (FND-11).
#include <frost/io/path.hpp>
#include <frost/io/utf.hpp>
#include <frost/update/safe_updater.hpp>
#include <frost/update/verification.hpp>

#include <algorithm>
#include <array>
#include <span>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace frost::update {
namespace {

bool hasZipMagicOrSuffix(std::string_view path, std::string_view content) noexcept {
    if (content.size() >= 4 && static_cast<unsigned char>(content[0]) == 0x50U &&
        static_cast<unsigned char>(content[1]) == 0x4BU && static_cast<unsigned char>(content[2]) == 0x03U &&
        static_cast<unsigned char>(content[3]) == 0x04U) {
        return true;
    }
    if (path.size() < 4)
        return false;
    const auto suffix = path.substr(path.size() - 4);
    return (suffix[0] == '.') && (suffix[1] == 'z' || suffix[1] == 'Z') && (suffix[2] == 'i' || suffix[2] == 'I') &&
           (suffix[3] == 'p' || suffix[3] == 'P');
}

std::string hexEncodeSha256(const Sha256& digest) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        hex.push_back(kDigits[byte >> 4]);
        hex.push_back(kDigits[byte & 0x0FU]);
    }
    return hex;
}

void ensureExecutablePermission(std::string_view executablePath) {
#ifndef _WIN32
    const std::string pathStr(executablePath);
    (void)::chmod(pathStr.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
#else
    (void)executablePath;
#endif
}

std::vector<std::pair<std::string, std::optional<std::string>>>
withOffscreenPlatform(std::vector<std::pair<std::string, std::optional<std::string>>> env) {
    const bool hasQpa = std::ranges::any_of(env, [](const auto& pair) { return pair.first == "QT_QPA_PLATFORM"; });
    if (!hasQpa)
        env.emplace_back("QT_QPA_PLATFORM", std::string("offscreen"));
    return env;
}

#ifdef _WIN32
Result<std::string> locateWindowsPowerShell(io::IFileSystem& fs) {
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return fail(ErrorCode::IoError, "GetSystemDirectoryW failed while locating PowerShell");
    auto sysDir = io::fromWide(std::wstring_view(buffer.data(), length));
    if (!sysDir)
        return std::unexpected(sysDir.error());
    auto psPath = io::joinPath(*sysDir, "WindowsPowerShell\\v1.0\\powershell.exe");
    if (!psPath)
        return std::unexpected(psPath.error());
    auto exists = fs.exists(*psPath);
    if (!exists || !*exists)
        return fail(ErrorCode::NotFound, "Windows PowerShell not found at " + *psPath);
    return *psPath;
}
#endif

} // namespace

Result<void> SafeUpdater::verifyPayloadFile(std::string_view payloadPath, const Release& release) const {
    auto bytes = m_fs.readFile(payloadPath);
    if (!bytes)
        return std::unexpected(bytes.error());
    const std::span<const std::byte> span(reinterpret_cast<const std::byte*>(bytes->data()), bytes->size());
    return verifyPayload(span, static_cast<std::size_t>(release.size), release.digest);
}

Result<void> SafeUpdater::copyDirectoryTree(std::string_view srcDir, std::string_view dstDir) const {
    if (auto created = m_fs.createDirectories(dstDir); !created)
        return std::unexpected(created.error());
    auto entries = m_fs.listDirectory(srcDir);
    if (!entries)
        return std::unexpected(entries.error());
    for (const auto& entry : *entries) {
        auto srcChild = io::joinPath(srcDir, entry.name);
        if (!srcChild)
            return std::unexpected(srcChild.error());
        auto dstChild = io::joinPath(dstDir, entry.name);
        if (!dstChild)
            return std::unexpected(dstChild.error());
        if (entry.type == io::FileType::Directory) {
            if (auto sub = copyDirectoryTree(*srcChild, *dstChild); !sub)
                return std::unexpected(sub.error());
        } else if (entry.type == io::FileType::Regular) {
            auto data = m_fs.readFile(*srcChild);
            if (!data)
                return std::unexpected(data.error());
            if (auto written = m_fs.writeFileAtomic(*dstChild, *data); !written)
                return std::unexpected(written.error());
        }
    }
    return {};
}

Result<void> SafeUpdater::moveOrCopyTree(std::string_view srcDir, std::string_view dstDir) const {
    if (auto removed = m_fs.removeAll(dstDir); !removed && removed.error().code() != ErrorCode::NotFound)
        return std::unexpected(removed.error());
    if (auto renamed = m_fs.rename(srcDir, dstDir); renamed)
        return {};
    if (auto copied = copyDirectoryTree(srcDir, dstDir); !copied)
        return std::unexpected(copied.error());
    if (auto cleaned = m_fs.removeAll(srcDir); !cleaned)
        return std::unexpected(cleaned.error());
    return {};
}

Result<void> SafeUpdater::extractZipArchive(std::string_view zipPath, std::string_view destDir,
                                            IClock::Duration timeout) const {
    if (auto created = m_fs.createDirectories(destDir); !created)
        return std::unexpected(created.error());
#ifdef _WIN32
    auto powershell = locateWindowsPowerShell(m_fs);
    if (!powershell)
        return std::unexpected(powershell.error());
    auto escapeSingleQuotes = [](std::string_view s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            if (c == '\'')
                out += "''";
            else
                out.push_back(c);
        }
        return out;
    };
    const std::string script = "$ErrorActionPreference = 'Stop'\n"
                               "Expand-Archive -LiteralPath '" +
                               escapeSingleQuotes(zipPath) + "' -DestinationPath '" + escapeSingleQuotes(destDir) +
                               "' -Force\n";
    io::ProcessSpec spec;
    spec.executable = *powershell;
    spec.arguments = {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script};
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    auto extracted = io::runToCompletion(m_launcher, spec, {}, timeout);
    if (!extracted)
        return std::unexpected(extracted.error());
    if (extracted->exitCode != 0) {
        return fail(ErrorCode::IoError, "Archive extraction failed with exit code " +
                                            std::to_string(extracted->exitCode) + ": " + extracted->stderrBytes);
    }
    return {};
#else
    io::ProcessSpec spec;
    spec.executable = "/usr/bin/unzip";
    spec.arguments = {"-oq", std::string(zipPath), "-d", std::string(destDir)};
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    auto extracted = io::runToCompletion(m_launcher, spec, {}, timeout);
    if (!extracted || extracted->exitCode != 0)
        return fail(ErrorCode::IoError, "Archive extraction failed on POSIX");
    return {};
#endif
}

Result<void> SafeUpdater::stagePayload(const SafeUpdateSpec& spec, std::string_view stageDir) const {
    if (auto removed = m_fs.removeAll(stageDir); !removed && removed.error().code() != ErrorCode::NotFound)
        return std::unexpected(removed.error());
    if (auto created = m_fs.createDirectories(stageDir); !created)
        return std::unexpected(created.error());

    auto st = m_fs.status(spec.payloadPath);
    if (!st)
        return std::unexpected(st.error());

    if (st->type == io::FileType::Directory) {
        if (spec.expectedRelease) {
            auto candidateExe = io::joinPath(spec.payloadPath, spec.executableRelativePath);
            if (!candidateExe)
                return std::unexpected(candidateExe.error());
            if (auto verified = verifyPayloadFile(*candidateExe, *spec.expectedRelease); !verified)
                return std::unexpected(verified.error());
        }
        return copyDirectoryTree(spec.payloadPath, stageDir);
    }

    if (st->type != io::FileType::Regular)
        return fail(ErrorCode::InvalidArgument, "Update payload must be a regular file or staged directory");

    auto rawBytes = m_fs.readFile(spec.payloadPath);
    if (!rawBytes)
        return std::unexpected(rawBytes.error());

    if (spec.expectedRelease) {
        const std::span<const std::byte> span(reinterpret_cast<const std::byte*>(rawBytes->data()), rawBytes->size());
        if (auto verified =
                verifyPayload(span, static_cast<std::size_t>(spec.expectedRelease->size), spec.expectedRelease->digest);
            !verified) {
            return std::unexpected(verified.error());
        }
    }

    if (hasZipMagicOrSuffix(spec.payloadPath, *rawBytes)) {
        const std::string rawExtractDir = std::string(stageDir) + ".extract";
        if (auto removed = m_fs.removeAll(rawExtractDir); !removed && removed.error().code() != ErrorCode::NotFound)
            return std::unexpected(removed.error());
        auto extracted = extractZipArchive(spec.payloadPath, rawExtractDir, spec.extractTimeout);
        if (!extracted) {
            (void)m_fs.removeAll(rawExtractDir);
            return std::unexpected(extracted.error());
        }
        // Unwrap a single top-level directory inside the zip if the executable sits inside it.
        std::string sourceRoot = rawExtractDir;
        auto rootExe = io::joinPath(rawExtractDir, spec.executableRelativePath);
        if (!rootExe) {
            (void)m_fs.removeAll(rawExtractDir);
            return std::unexpected(rootExe.error());
        }
        auto rootExeExists = m_fs.exists(*rootExe);
        if (!rootExeExists || !*rootExeExists) {
            if (auto entries = m_fs.listDirectory(rawExtractDir);
                entries && entries->size() == 1 && (*entries)[0].type == io::FileType::Directory) {
                auto nestedRoot = io::joinPath(rawExtractDir, (*entries)[0].name);
                if (nestedRoot) {
                    auto nestedExe = io::joinPath(*nestedRoot, spec.executableRelativePath);
                    if (nestedExe) {
                        if (auto nestedExists = m_fs.exists(*nestedExe); nestedExists && *nestedExists)
                            sourceRoot = *nestedRoot;
                    }
                }
            }
        }
        auto moved = moveOrCopyTree(sourceRoot, stageDir);
        (void)m_fs.removeAll(rawExtractDir);
        return moved;
    }

    // Single-binary payload: place at <stageDir>/<executableRelativePath>.
    auto stagedExe = io::joinPath(stageDir, spec.executableRelativePath);
    if (!stagedExe)
        return std::unexpected(stagedExe.error());
    if (const auto parent = io::parentPath(*stagedExe); !parent.empty()) {
        if (auto created = m_fs.createDirectories(parent); !created)
            return std::unexpected(created.error());
    }
    if (auto written = m_fs.writeFileAtomic(*stagedExe, *rawBytes); !written)
        return std::unexpected(written.error());
    ensureExecutablePermission(*stagedExe);
    return {};
}

Result<UpdateOutcome> SafeUpdater::applyUpdate(const SafeUpdateSpec& spec) const {
    if (spec.installDir.empty() || spec.backupDir.empty() || spec.payloadPath.empty() ||
        spec.executableRelativePath.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "SafeUpdateSpec requires non-empty installDir, backupDir, and payloadPath");
    }

    auto normInstall = io::normalizePath(spec.installDir);
    auto normBackup = io::normalizePath(spec.backupDir);
    auto normPayload = io::normalizePath(spec.payloadPath);
    if (!normInstall || !normBackup || !normPayload)
        return fail(ErrorCode::InvalidArgument, "SafeUpdateSpec paths must be valid absolute paths");
    if (*normInstall == *normBackup)
        return fail(ErrorCode::InvalidArgument, "installDir and backupDir must be distinct directories");

    const std::string stageDir = *normBackup + ".stage";
    if (auto staged = stagePayload(spec, stageDir); !staged) {
        (void)m_fs.removeAll(stageDir);
        return std::unexpected(staged.error());
    }

    // Back up any existing installation at installDir before replacing it.
    auto hadPreviousInstallResult = m_fs.exists(*normInstall);
    if (!hadPreviousInstallResult) {
        (void)m_fs.removeAll(stageDir);
        return std::unexpected(hadPreviousInstallResult.error());
    }
    const bool hadPreviousInstall = *hadPreviousInstallResult;
    if (hadPreviousInstall) {
        if (auto backedUp = moveOrCopyTree(*normInstall, *normBackup); !backedUp) {
            (void)m_fs.removeAll(stageDir);
            return std::unexpected(backedUp.error());
        }
    }

    // Move staged update into installDir.
    if (auto installed = moveOrCopyTree(stageDir, *normInstall); !installed) {
        (void)m_fs.removeAll(stageDir);
        if (hadPreviousInstall)
            (void)moveOrCopyTree(*normBackup, *normInstall);
        return std::unexpected(installed.error());
    }

    auto installedExeRes = io::joinPath(*normInstall, spec.executableRelativePath);
    if (!installedExeRes)
        return std::unexpected(installedExeRes.error());
    const std::string installedExe = *installedExeRes;
    ensureExecutablePermission(installedExe);

    // Run bounded health check on the newly installed executable via IProcessLauncher.
    io::ProcessSpec healthSpec;
    healthSpec.executable = installedExe;
    healthSpec.arguments = spec.healthCheckArgs;
    healthSpec.workingDirectory = *normInstall;
    healthSpec.environment = withOffscreenPlatform(spec.healthCheckEnvironment);
    healthSpec.stdinMode = io::Stdio::Null;
    healthSpec.stdoutMode = io::Stdio::Pipe;
    healthSpec.stderrMode = io::Stdio::Pipe;
    healthSpec.killOnClose = true;

    auto healthResult = io::runToCompletion(m_launcher, healthSpec, {}, spec.healthCheckTimeout);
    const bool healthOk = healthResult.has_value() && healthResult->exitCode == 0 && healthResult->signal == 0;

    UpdateOutcome outcome;
    outcome.activeExecutablePath = installedExe;

    if (healthOk) {
        outcome.applied = true;
        outcome.rolledBack = false;
        outcome.healthCheckPassed = true;
        outcome.healthCheckExitCode = 0;
        outcome.processStdout = std::move(healthResult->stdoutBytes);
        outcome.processStderr = std::move(healthResult->stderrBytes);

        if (spec.expectedRelease) {
            outcome.installedVersion = spec.expectedRelease->version;
            outcome.installedBuild = spec.expectedRelease->build;
            ReleaseFloor floor{
                .version = spec.expectedRelease->version,
                .build = spec.expectedRelease->build,
                .digest = hexEncodeSha256(spec.expectedRelease->digest),
            };
            if (auto encoded = encodeReleaseFloor(floor)) {
                if (auto markerPath = io::joinPath(*normInstall, "installed-release.json"))
                    (void)m_fs.writeFileAtomic(*markerPath, *encoded);
            }
        }

        if (!spec.keepBackupOnSuccess && hadPreviousInstall)
            (void)m_fs.removeAll(*normBackup);

        if (spec.launchDetachedAfterInstall) {
            io::ProcessSpec detached;
            detached.executable = installedExe;
            detached.arguments = spec.detachedLaunchArgs;
            detached.workingDirectory = *normInstall;
            detached.stdinMode = io::Stdio::Null;
            detached.stdoutMode = io::Stdio::Null;
            detached.stderrMode = io::Stdio::Null;
            detached.killOnClose = false;
            if (auto started = m_launcher.launch(detached); !started)
                return std::unexpected(started.error());
        }

        return outcome;
    }

    // Health check failed: record diagnostic details and automatically roll back to backupDir.
    outcome.applied = false;
    outcome.healthCheckPassed = false;
    if (healthResult) {
        outcome.healthCheckExitCode = healthResult->exitCode;
        outcome.processStderr = std::move(healthResult->stderrBytes);
        outcome.failureReason = "New version health check exited with code " + std::to_string(healthResult->exitCode);
    } else {
        outcome.healthCheckExitCode = -1;
        outcome.failureReason = "New version health check failed to run: " + healthResult.error().message();
    }

    if (!hadPreviousInstall) {
        (void)m_fs.removeAll(*normInstall);
        return fail(ErrorCode::Corrupt, outcome.failureReason + " (no previous installation to roll back to)");
    }

    if (auto restored = moveOrCopyTree(*normBackup, *normInstall); !restored) {
        return fail(ErrorCode::IoError,
                    outcome.failureReason + "; rollback restore failed: " + restored.error().message());
    }
    ensureExecutablePermission(installedExe);
    outcome.rolledBack = true;

    if (spec.verifyPreviousVersionStartsOnRollback) {
        io::ProcessSpec rollbackSpec;
        rollbackSpec.executable = installedExe;
        rollbackSpec.arguments = spec.rollbackVerifyArgs.empty() ? spec.healthCheckArgs : spec.rollbackVerifyArgs;
        rollbackSpec.workingDirectory = *normInstall;
        rollbackSpec.environment = withOffscreenPlatform(spec.healthCheckEnvironment);
        rollbackSpec.stdinMode = io::Stdio::Null;
        rollbackSpec.stdoutMode = io::Stdio::Pipe;
        rollbackSpec.stderrMode = io::Stdio::Pipe;
        rollbackSpec.killOnClose = true;

        auto rollbackRun = io::runToCompletion(m_launcher, rollbackSpec, {}, spec.healthCheckTimeout);
        if (!rollbackRun || rollbackRun->exitCode != 0 || rollbackRun->signal != 0) {
            outcome.previousVersionStarted = false;
            outcome.rollbackStartExitCode = rollbackRun ? rollbackRun->exitCode : -1;
            return fail(ErrorCode::IoError, outcome.failureReason + "; rolled-back previous version failed to start");
        }
        outcome.previousVersionStarted = true;
        outcome.rollbackStartExitCode = 0;
        outcome.processStdout = std::move(rollbackRun->stdoutBytes);
    }

    return outcome;
}

} // namespace frost::update
