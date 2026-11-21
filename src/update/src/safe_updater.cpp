// SPDX-License-Identifier: Apache-2.0
// Transactional update installation, health-check probing, and automatic rollback (FND-11).
#include <frost/io/os_error.hpp>
#include <frost/io/path.hpp>
#include <frost/io/utf.hpp>
#include <frost/update/safe_updater.hpp>
#include <frost/update/verification.hpp>

#include <algorithm>
#include <array>
#include <span>

#ifdef _WIN32
#include "install_ownership_win.hpp"

#include <windows.h>

#include <winioctl.h>
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
Result<bool> directoryCompression(std::string_view path) {
    auto wide = io::toWin32Path(path);
    if (!wide)
        return std::unexpected(wide.error());
    const DWORD attributes = GetFileAttributesW(wide->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return std::unexpected(io::win32Error(GetLastError(), "read directory compression", path));
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != FILE_ATTRIBUTE_DIRECTORY)
        return fail(ErrorCode::InvalidArgument, "Update compression requires an ordinary directory");
    return (attributes & FILE_ATTRIBUTE_COMPRESSED) != 0;
}

Result<void> setDirectoryCompression(std::string_view path, bool compressed) {
    auto current = directoryCompression(path);
    if (!current)
        return std::unexpected(current.error());
    if (*current == compressed)
        return {};
    auto wide = io::toWin32Path(path);
    if (!wide)
        return std::unexpected(wide.error());
    HANDLE directory =
        CreateFileW(wide->c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE)
        return std::unexpected(io::win32Error(GetLastError(), "open update directory for compression", path));
    FILE_ATTRIBUTE_TAG_INFO info{};
    DWORD error = ERROR_SUCCESS;
    if (!GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &info, sizeof(info))) {
        error = GetLastError();
    } else if ((info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
               FILE_ATTRIBUTE_DIRECTORY) {
        error = ERROR_DIRECTORY;
    } else {
        USHORT format = compressed ? COMPRESSION_FORMAT_DEFAULT : COMPRESSION_FORMAT_NONE;
        DWORD returned = 0;
        if (!DeviceIoControl(directory, FSCTL_SET_COMPRESSION, &format, sizeof(format), nullptr, 0, &returned, nullptr))
            error = GetLastError();
    }
    CloseHandle(directory);
    if (error != ERROR_SUCCESS)
        return std::unexpected(io::win32Error(error, "set update directory compression", path));
    return {};
}

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

Result<void> createUpdateDirectory(io::IFileSystem& fs, std::string_view path, std::optional<bool> compressed) {
    if (auto created = fs.createDirectories(path); !created)
        return created;
#ifdef _WIN32
    if (compressed)
        return setDirectoryCompression(path, *compressed);
#else
    (void)compressed;
#endif
    return {};
}

#ifdef _WIN32
Result<void> createExclusiveUpdateDirectory(std::string_view path, std::optional<bool> compressed) {
    auto wide = io::toWin32Path(path);
    if (!wide)
        return std::unexpected(wide.error());
    if (!CreateDirectoryW(wide->c_str(), nullptr)) {
        const auto error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
            return fail(ErrorCode::Busy, "The update destination already exists: " + std::string(path));
        return std::unexpected(io::win32Error(error, "create private update directory", path));
    }
    if (compressed) {
        auto changed = setDirectoryCompression(path, *compressed);
        if (!changed) {
            (void)RemoveDirectoryW(wide->c_str());
            return changed;
        }
    }
    return {};
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

Result<void> SafeUpdater::moveOrCopyTree(std::string_view srcDir, std::string_view dstDir,
                                        bool replaceDestination, const detail::InstallOwnership* ownership) const {
    if (replaceDestination) {
        if (auto removed = m_fs.removeAll(dstDir); !removed && removed.error().code() != ErrorCode::NotFound)
            return std::unexpected(removed.error());
    }
    if (auto renamed = m_fs.rename(srcDir, dstDir); renamed)
        return {};
#ifdef _WIN32
    if (!replaceDestination) {
        auto occupied = m_fs.exists(dstDir);
        if (!occupied)
            return std::unexpected(occupied.error());
        if (*occupied)
            return fail(ErrorCode::Busy, "The update destination already exists: " + std::string(dstDir));
    }
    // Across volumes a new directory does not inherit the source root's policy.
    auto compressed = directoryCompression(srcDir);
    if (!compressed)
        return std::unexpected(compressed.error());
    auto created = replaceDestination ? createUpdateDirectory(m_fs, dstDir, *compressed)
                                      : createExclusiveUpdateDirectory(dstDir, *compressed);
    if (!created)
        return created;
#endif
    if (auto copied = copyDirectoryTree(srcDir, dstDir); !copied)
        return std::unexpected(copied.error());
#ifdef _WIN32
    if (ownership) {
        // A cross-volume move is copy-and-remove. Preserve both trees if either stopped matching
        // its receipt during the copy instead of deleting unowned source bytes.
        if (auto copied = detail::validateInstallOwnership(m_fs, dstDir, *ownership); !copied)
            return copied;
        if (auto source = detail::validateInstallOwnership(m_fs, srcDir, *ownership); !source)
            return source;
    }
#else
    (void)ownership;
#endif
    if (auto cleaned = m_fs.removeAll(srcDir); !cleaned)
        return std::unexpected(cleaned.error());
    return {};
}

Result<void> SafeUpdater::extractZipArchive(std::string_view zipPath, std::string_view destDir,
                                            IClock::Duration timeout) const {
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

Result<void> SafeUpdater::stagePayload(const SafeUpdateSpec& spec, std::string_view stageDir,
                                       std::optional<bool> compressed, bool requireVerifiedArchive) const {
    if (!requireVerifiedArchive) {
        if (auto removed = m_fs.removeAll(stageDir); !removed && removed.error().code() != ErrorCode::NotFound)
            return std::unexpected(removed.error());
        if (auto created = createUpdateDirectory(m_fs, stageDir, compressed); !created)
            return std::unexpected(created.error());
    }

    auto st = m_fs.status(spec.payloadPath);
    if (!st)
        return std::unexpected(st.error());
    if (requireVerifiedArchive && (!spec.expectedRelease || st->type != io::FileType::Regular))
        return fail(ErrorCode::Unsupported, "A per-user install requires a verified update ZIP");

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
        std::string archivePath = spec.payloadPath;
        if (requireVerifiedArchive) {
            // Extract the verified bytes, even if the original download is subsequently replaced.
            auto snapshot = io::joinPath(stageDir, ".verified-update-input.zip");
            if (!snapshot)
                return std::unexpected(snapshot.error());
            if (auto written = m_fs.writeFileAtomic(*snapshot, *rawBytes); !written)
                return written;
            archivePath = std::move(*snapshot);
        }
        const std::string rawExtractDir = std::string(stageDir) + ".extract";
        if (!requireVerifiedArchive) {
            if (auto removed = m_fs.removeAll(rawExtractDir); !removed && removed.error().code() != ErrorCode::NotFound)
                return std::unexpected(removed.error());
            if (auto created = createUpdateDirectory(m_fs, rawExtractDir, compressed); !created) {
                (void)m_fs.removeAll(rawExtractDir);
                return created;
            }
        }
#ifdef _WIN32
        else if (auto created = createExclusiveUpdateDirectory(rawExtractDir, compressed); !created) {
            return created;
        }
#endif
        auto extracted = extractZipArchive(archivePath, rawExtractDir, spec.extractTimeout);
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

    if (requireVerifiedArchive)
        return fail(ErrorCode::Unsupported, "A per-user install requires a verified update ZIP");

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

    std::optional<bool> compressed;
    bool requireVerifiedArchive = false;
    const detail::InstallOwnership* originalOwnership = nullptr;
    const detail::InstallOwnership* candidateOwnership = nullptr;
#ifdef _WIN32
    std::optional<detail::InstallOwnership> ownership;
    std::optional<detail::InstallOwnership> stagedOwnership;
    auto receiptPath = io::joinPath(*normInstall, detail::kInstallReceipt);
    if (!receiptPath)
        return std::unexpected(receiptPath.error());
    auto originalExists = m_fs.exists(*normInstall);
    if (!originalExists)
        return std::unexpected(originalExists.error());
    if (*originalExists) {
        auto previous = directoryCompression(*normInstall);
        if (!previous)
            return std::unexpected(previous.error());
        compressed = *previous;
        auto hasReceipt = m_fs.exists(*receiptPath);
        if (!hasReceipt)
            return std::unexpected(hasReceipt.error());
        if (*hasReceipt) {
            auto receipt = m_fs.readFileSnapshot(*receiptPath, detail::kInstallReceiptLimit);
            if (!receipt)
                return std::unexpected(receipt.error());
            auto parsed = detail::parseInstallOwnership(std::move(*receipt));
            if (!parsed)
                return std::unexpected(parsed.error());
            ownership = std::move(*parsed);
            originalOwnership = &*ownership;
            requireVerifiedArchive = true;
        }
    }
#endif
    // Apply the existing directory's policy before creating any payload entries.
    const std::string stageDir = *normBackup + ".stage";
    if (requireVerifiedArchive) {
        for (const auto& path : {*normBackup, stageDir, stageDir + ".extract", stageDir + ".failed"}) {
            auto occupied = m_fs.exists(path);
            if (!occupied)
                return std::unexpected(occupied.error());
            if (*occupied)
                return fail(ErrorCode::Busy, "An update workspace path already exists: " + path);
        }
#ifdef _WIN32
        if (auto created = createExclusiveUpdateDirectory(stageDir, compressed); !created)
            return std::unexpected(created.error());
#endif
    }
    if (auto staged = stagePayload(spec, stageDir, compressed, requireVerifiedArchive); !staged) {
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
#ifdef _WIN32
    if (ownership) {
        const ReleaseFloor floor{
            .version = spec.expectedRelease->version,
            .build = spec.expectedRelease->build,
            .digest = hexEncodeSha256(spec.expectedRelease->digest),
        };
        auto encoded = encodeReleaseFloor(floor);
        if (!encoded) {
            (void)m_fs.removeAll(stageDir);
            return std::unexpected(encoded.error());
        }
        auto recorded = detail::writeInstallOwnership(m_fs, stageDir, *ownership, *encoded);
        if (!recorded) {
            (void)m_fs.removeAll(stageDir);
            return std::unexpected(recorded.error());
        }
        stagedOwnership = std::move(*recorded);
        candidateOwnership = &*stagedOwnership;
    }
    if (hadPreviousInstall != *originalExists) {
        (void)m_fs.removeAll(stageDir);
        return fail(ErrorCode::Busy, "Update installation appeared or disappeared while staging");
    }
    if (hadPreviousInstall) {
        auto current = directoryCompression(*normInstall);
        if (!current || *current != *compressed) {
            (void)m_fs.removeAll(stageDir);
            if (!current)
                return std::unexpected(current.error());
            return fail(ErrorCode::Busy, "Update installation compression changed while staging");
        }
        auto hasReceipt = m_fs.exists(*receiptPath);
        if (!hasReceipt || *hasReceipt != ownership.has_value()) {
            (void)m_fs.removeAll(stageDir);
            if (!hasReceipt)
                return std::unexpected(hasReceipt.error());
            return fail(ErrorCode::Busy, "The per-user install receipt appeared or disappeared during the update");
        }
        if (ownership) {
            if (auto validated = detail::validateInstallOwnership(m_fs, *normInstall, *ownership); !validated) {
                (void)m_fs.removeAll(stageDir);
                return std::unexpected(validated.error());
            }
        }
    }
#endif
    if (hadPreviousInstall) {
        if (auto backedUp = moveOrCopyTree(*normInstall, *normBackup, !requireVerifiedArchive, originalOwnership);
            !backedUp) {
            (void)m_fs.removeAll(stageDir);
            return std::unexpected(backedUp.error());
        }
    }
#ifdef _WIN32
    if (ownership) {
        if (auto verified = detail::validateInstallOwnership(m_fs, *normBackup, *ownership); !verified) {
            (void)m_fs.removeAll(stageDir);
            auto restored = moveOrCopyTree(*normBackup, *normInstall, false);
            if (!restored)
                return fail(ErrorCode::Busy, "The installation changed while moving to backup; the backup was retained: " +
                                                 restored.error().message());
            return std::unexpected(verified.error());
        }
    }
#endif

    // Move staged update into installDir.
    if (auto installed = moveOrCopyTree(stageDir, *normInstall, !requireVerifiedArchive, candidateOwnership);
        !installed) {
        bool removeStage = !requireVerifiedArchive;
#ifdef _WIN32
        if (stagedOwnership)
            removeStage = detail::validateInstallOwnership(m_fs, stageDir, *stagedOwnership).has_value();
#endif
        if (removeStage)
            (void)m_fs.removeAll(stageDir);
#ifdef _WIN32
        if (ownership) {
            auto safeBackup = detail::validateInstallOwnership(m_fs, *normBackup, *ownership);
            if (!safeBackup)
                return fail(ErrorCode::Busy, "Update publication failed; the changed backup was preserved: " +
                                                 safeBackup.error().message());
        }
#endif
        if (hadPreviousInstall) {
            auto restored = moveOrCopyTree(*normBackup, *normInstall, !requireVerifiedArchive, originalOwnership);
            if (!restored && requireVerifiedArchive)
                return fail(ErrorCode::IoError, "Update publication failed; the backup was retained: " +
                                                    restored.error().message());
        }
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
            if (!requireVerifiedArchive) {
                if (auto encoded = encodeReleaseFloor(floor)) {
                    if (auto markerPath = io::joinPath(*normInstall, "installed-release.json"))
                        (void)m_fs.writeFileAtomic(*markerPath, *encoded);
                }
            }
        }

        bool removeBackup = !spec.keepBackupOnSuccess && hadPreviousInstall;
#ifdef _WIN32
        if (removeBackup && ownership)
            removeBackup = detail::validateInstallOwnership(m_fs, *normBackup, *ownership).has_value();
#endif
        if (removeBackup)
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

#ifdef _WIN32
    std::string failedInstall;
    if (stagedOwnership) {
        auto previous = detail::validateInstallOwnership(m_fs, *normBackup, *ownership);
        if (!previous)
            return fail(ErrorCode::Busy, outcome.failureReason + "; rollback retained the changed backup: " +
                                             previous.error().message());
        auto current = detail::validateInstallOwnership(m_fs, *normInstall, *stagedOwnership);
        if (!current)
            return fail(ErrorCode::Busy, outcome.failureReason + "; rollback preserved both directories: " +
                                             current.error().message());
        failedInstall = stageDir + ".failed";
        if (auto moved = moveOrCopyTree(*normInstall, failedInstall, false, candidateOwnership); !moved)
            return fail(ErrorCode::IoError, outcome.failureReason + "; could not preserve the failed installation: " +
                                                moved.error().message());
    }
#endif
    if (auto restored = moveOrCopyTree(*normBackup, *normInstall, !requireVerifiedArchive, originalOwnership);
        !restored) {
        return fail(ErrorCode::IoError,
                    outcome.failureReason + "; rollback restore failed: " + restored.error().message());
    }
#ifdef _WIN32
    if (stagedOwnership && detail::validateInstallOwnership(m_fs, failedInstall, *stagedOwnership))
        (void)m_fs.removeAll(failedInstall);
#endif
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
