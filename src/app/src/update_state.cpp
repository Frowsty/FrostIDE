// SPDX-License-Identifier: Apache-2.0
// Serialized anti-replay state; failed writes never authorize a download.
#include "update_state.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>

#include <array>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace frost::app {
namespace {
Result<QString> stateDirectory(const QString& directory) {
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory) || !QDir().mkpath(directory))
        return fail(ErrorCode::IoError, "Cannot create update state directory");
    auto canonical = QFileInfo(directory).canonicalFilePath();
    if (canonical.isEmpty())
        return fail(ErrorCode::IoError, "Cannot resolve update state directory");
#ifdef _WIN32
    if (canonical.size() < 3 || !canonical[0].isLetter() || canonical[1] != QLatin1Char(':') ||
        canonical[2] != QLatin1Char('/'))
        return fail(ErrorCode::InvalidArgument, "Update state requires a local drive path");
    const auto drive = canonical.left(3);
    const auto driveType = GetDriveTypeW(reinterpret_cast<LPCWSTR>(drive.utf16()));
    if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE && driveType != DRIVE_RAMDISK)
        return fail(ErrorCode::InvalidArgument, "Update state requires local storage");
#endif
    return canonical;
}
Result<update::ReleaseFloor> readUnlocked(const QString& path) {
    std::array<char, 1025> bytes{};
    std::size_t used = 0;
#ifdef _WIN32
    struct File {
        HANDLE handle;
        ~File() {
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
        }
    } input{CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (input.handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
            return update::ReleaseFloor{};
        return fail(ErrorCode::IoError, "Cannot open saved update state");
    }
    FILE_STANDARD_INFO standard{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileType(input.handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(input.handle, FileStandardInfo, &standard, sizeof(standard)) ||
        !GetFileInformationByHandleEx(input.handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        standard.Directory || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        standard.EndOfFile.QuadPart < 0 || standard.EndOfFile.QuadPart > 1024)
        return fail(ErrorCode::Corrupt, "Invalid saved update state file");
    while (used < bytes.size()) {
        DWORD count = 0;
        if (!ReadFile(input.handle, bytes.data() + used, static_cast<DWORD>(bytes.size() - used), &count, nullptr))
            return fail(ErrorCode::IoError, "Cannot read saved update state");
        if (count == 0)
            break;
        used += count;
    }
#else
    struct File {
        int descriptor;
        ~File() {
            if (descriptor >= 0)
                close(descriptor);
        }
    } input{open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW)};
    if (input.descriptor < 0) {
        if (errno == ENOENT)
            return update::ReleaseFloor{};
        return fail(ErrorCode::IoError, "Cannot open saved update state");
    }
    struct stat info{};
    if (fstat(input.descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > 1024)
        return fail(ErrorCode::Corrupt, "Invalid saved update state file");
    while (used < bytes.size()) {
        const auto count = read(input.descriptor, bytes.data() + used, bytes.size() - used);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return fail(ErrorCode::IoError, "Cannot read saved update state");
        }
        if (count == 0)
            break;
        used += static_cast<std::size_t>(count);
    }
#endif
    return update::parseReleaseFloor(std::string_view(bytes.data(), used));
}
QString statePath(const QString& directory) { return directory + QStringLiteral("/accepted-release.json"); }
} // namespace
Result<update::ReleaseFloor> readUpdateFloor(const QString& directory) {
    auto resolved = stateDirectory(directory);
    if (!resolved)
        return std::unexpected(resolved.error());
    QLockFile lock(*resolved + QStringLiteral("/accepted-release.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return fail(ErrorCode::Busy, "Another instance is updating release state");
    return readUnlocked(statePath(*resolved));
}
Result<update::Release> acceptUpdate(const QString& directory, const update::AuthenticatedRelease& envelope,
                                     const update::PublicKey& key, update::ReleasePolicy policy) {
    auto resolved = stateDirectory(directory);
    if (!resolved)
        return std::unexpected(resolved.error());
    QLockFile lock(*resolved + QStringLiteral("/accepted-release.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return fail(ErrorCode::Busy, "Another instance is updating release state");
    const auto path = statePath(*resolved);
    auto floor = readUnlocked(path);
    if (!floor)
        return std::unexpected(floor.error());
    policy.acceptedBuild = floor->build;
    policy.acceptedVersion = floor->version;
    policy.acceptedDigest = floor->digest;
    auto verified = update::verifyRelease(envelope.manifest, envelope.signature, key, policy);
    if (!verified)
        return std::unexpected(verified.error());
    if (verified->build == floor->build)
        return verified;
    constexpr char digits[] = "0123456789abcdef";
    std::string digest;
    for (auto byte : verified->digest) {
        digest.push_back(digits[byte >> 4]);
        digest.push_back(digits[byte & 15]);
    }
    auto encoded = update::encodeReleaseFloor({verified->version, verified->build, std::move(digest)});
    if (!encoded)
        return std::unexpected(encoded.error());
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) ||
        !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        output.write(encoded->data(), static_cast<qint64>(encoded->size())) != static_cast<qint64>(encoded->size()) ||
        !output.commit())
        return fail(ErrorCode::IoError, "Cannot save update security state; download refused");
    return verified;
}
} // namespace frost::app
