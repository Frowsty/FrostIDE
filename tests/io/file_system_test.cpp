// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for the real IFileSystem against the disk.
//
/*
 * Byte-exact round trips, atomic replacement (no temp files left, old content
 * kept on failure, read-only targets, and on POSIX the owner, mode and ACL
 * rules), listings, error-code mapping, links, non-ASCII names, and on Windows
 * paths longer than MAX_PATH, which must work even with LongPathsEnabled = 0.
 */

#include "frost/core/clock.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "internal.hpp" // kAtomicTempMarker.
#include "support/scratch_dir.hpp"
#include <gtest/gtest-spi.h> // EXPECT_NONFATAL_FAILURE, for ScratchDir's own failure reporting.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/xattr.h>

#include <cerrno>

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using namespace std::string_literals;
using frost::ErrorCode;
using frost::io::DirEntry;
using frost::io::FileType;
using frost::test::ScratchDir;

frost::io::IFileSystem& fs() { return frost::io::realFileSystem(); }

// True when no writeFileAtomic temp file is left in `directory`.
bool noTempFiles(const std::string& directory) {
    const auto entries = fs().listDirectory(directory);
    if (!entries) {
        return false;
    }
    for (const DirEntry& entry : *entries) {
        if (entry.name.find(frost::io::detail::kAtomicTempMarker) != std::string::npos) {
            return false;
        }
    }
    return true;
}

#ifndef _WIN32
// Permission checks do not apply to root (e.g. tests run in a container as root).
bool runningAsRoot() { return geteuid() == 0; }
#endif

TEST(RealFileSystem, WriteAndReadAreByteExact) {
    const ScratchDir dir("fs-bytes");
    std::string data = "line one\r\nline two\n\0\xff\xfe binary"s; // The s suffix keeps the NUL.
    // 200 KB: well past small buffers, though each implementation reads and writes it in one call (more
    // only beyond 1 GiB on Windows, after a short write, or when the file grows during the read).
    data += std::string(200'000, 'x');
    const std::string file = dir.join("data.bin");
    ASSERT_TRUE(fs().writeFileAtomic(file, data).has_value());
    EXPECT_EQ(fs().readFile(file).value(), data);
    ASSERT_TRUE(fs().writeFileAtomic(file, "").has_value());
    EXPECT_EQ(fs().readFile(file).value(), "");
}

TEST(RealFileSystem, AtomicWriteReplacesAndLeavesNoTempFiles) {
    const ScratchDir dir("fs-atomic");
    const std::string file = dir.join("settings.json");
    ASSERT_TRUE(fs().writeFileAtomic(file, "version 1, rather long content").has_value());
    ASSERT_TRUE(fs().writeFileAtomic(file, "v2").has_value());
    EXPECT_EQ(fs().readFile(file).value(), "v2");
    EXPECT_EQ(fs().listDirectory(dir.path()).value(), (std::vector<DirEntry>{{"settings.json", FileType::Regular}}));
    EXPECT_TRUE(noTempFiles(dir.path()));
}

TEST(RealFileSystem, AtomicWriteIntoMissingDirectoryFails) {
    const ScratchDir dir("fs-missing");
    const auto result = fs().writeFileAtomic(dir.join("no/such/dir/file"), "x");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_TRUE(fs().listDirectory(dir.path()).value().empty());
}

TEST(RealFileSystem, AtomicWriteOverADirectoryFailsAndLeavesItAlone) {
    const ScratchDir dir("fs-overdir");
    const std::string target = dir.join("sub");
    ASSERT_TRUE(fs().createDirectories(target).has_value());
    ASSERT_TRUE(fs().writeFileAtomic(dir.join("sub/keep"), "k").has_value());
    const auto result = fs().writeFileAtomic(target, "x");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << result.error().describe();
    EXPECT_EQ(fs().readFile(dir.join("sub/keep")).value(), "k");
    EXPECT_TRUE(noTempFiles(dir.path()));
}

#ifdef _WIN32
TEST(RealFileSystem, FailedReplaceKeepsTheOldContent) {
    // Another process holding the target open without FILE_SHARE_DELETE blocks the rename,
    // a real-world failure (virus scanners, editors) injected without any mock.
    const ScratchDir dir("fs-locked");
    const std::string file = dir.join("locked.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old content").has_value());
    const std::wstring wide = frost::io::toWin32Path(file).value();
    const HANDLE holder =
        CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(holder, INVALID_HANDLE_VALUE);
    const auto result = fs().writeFileAtomic(file, "new content");
    CloseHandle(holder);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().code() == ErrorCode::PermissionDenied || result.error().code() == ErrorCode::Busy)
        << result.error().describe();
    EXPECT_EQ(fs().readFile(file).value(), "old content");
    EXPECT_TRUE(noTempFiles(dir.path()));
}

TEST(RealFileSystem, ReadOnlyTargetsAreRefused) {
    // MoveFileExW does not replace a file with FILE_ATTRIBUTE_READONLY (POSIX does; see
    // ReadOnlyTargetsAreReplaced there).
    const ScratchDir dir("fs-readonly-target");
    const std::string file = dir.join("frozen.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old").has_value());
    const std::wstring wide = frost::io::toWin32Path(file).value();
    ASSERT_NE(SetFileAttributesW(wide.c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
    const auto result = fs().writeFileAtomic(file, "new");
    // Cleared before any assertion can return, so ScratchDir can remove the file.
    ASSERT_NE(SetFileAttributesW(wide.c_str(), FILE_ATTRIBUTE_NORMAL), FALSE);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied) << result.error().describe();
    EXPECT_EQ(fs().readFile(file).value(), "old");
    EXPECT_TRUE(noTempFiles(dir.path()));
}

TEST(RealFileSystem, SharingViolationMapsToBusy) {
    const ScratchDir dir("fs-busy");
    const std::string file = dir.join("exclusive.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "x").has_value());
    const std::wstring wide = frost::io::toWin32Path(file).value();
    const HANDLE holder = CreateFileW(wide.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(holder, INVALID_HANDLE_VALUE);
    const auto result = fs().readFile(file);
    CloseHandle(holder);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Busy);
    EXPECT_NE(result.error().message().find("win32 error 32"), std::string::npos) << result.error().message();
}

TEST(RealFileSystem, ForwardSlashesAndDotSegmentsAreNormalised) {
    const ScratchDir dir("fs-slashes");
    std::string mixed = dir.path();
    std::ranges::replace(mixed, '\\', '/');
    ASSERT_TRUE(fs().writeFileAtomic(mixed + "/./x/../file.txt", "slashes").has_value());
    EXPECT_EQ(fs().readFile(dir.join("file.txt")).value(), "slashes");
}
#else
TEST(RealFileSystem, FailedWriteKeepsTheOldContent) {
    if (runningAsRoot()) {
        GTEST_SKIP() << "root ignores directory permissions";
    }
    // A read-only directory refuses the temp file, so the replace never starts.
    const ScratchDir dir("fs-readonly");
    const std::string sub = dir.join("ro");
    ASSERT_TRUE(fs().createDirectories(sub).has_value());
    const std::string file = sub + "/file.txt";
    ASSERT_TRUE(fs().writeFileAtomic(file, "old content").has_value());
    ASSERT_EQ(chmod(sub.c_str(), 0555), 0);
    const auto result = fs().writeFileAtomic(file, "new content");
    chmod(sub.c_str(), 0755);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied) << result.error().describe();
    EXPECT_EQ(fs().readFile(file).value(), "old content");
    EXPECT_TRUE(noTempFiles(sub));
}

TEST(RealFileSystem, AtomicWriteKeepsPermissionBits) {
    const ScratchDir dir("fs-mode");
    const std::string file = dir.join("script.sh");
    ASSERT_TRUE(fs().writeFileAtomic(file, "#!/bin/sh\n").has_value());
    ASSERT_EQ(chmod(file.c_str(), 0751), 0);
    ASSERT_TRUE(fs().writeFileAtomic(file, "#!/bin/sh\necho hi\n").has_value());
    struct stat info{};
    ASSERT_EQ(stat(file.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 07777, 0751u);
}

TEST(RealFileSystem, UnreadableFileMapsToPermissionDenied) {
    if (runningAsRoot()) {
        GTEST_SKIP() << "root ignores file permissions";
    }
    const ScratchDir dir("fs-eacces");
    const std::string file = dir.join("secret");
    ASSERT_TRUE(fs().writeFileAtomic(file, "x").has_value());
    ASSERT_EQ(chmod(file.c_str(), 0), 0);
    const auto result = fs().readFile(file);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    EXPECT_NE(result.error().message().find("errno"), std::string::npos);
}
#endif

TEST(RealFileSystem, StatusReportsTypeSizeAndTime) {
    const ScratchDir dir("fs-status");
    const std::string file = dir.join("f.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "12345").has_value());
    const auto fileStatus = fs().status(file);
    ASSERT_TRUE(fileStatus.has_value());
    EXPECT_EQ(fileStatus->type, FileType::Regular);
    EXPECT_EQ(fileStatus->size, 5u);
    const auto age = std::chrono::system_clock::now() - fileStatus->mtime;
    EXPECT_LT(age, std::chrono::hours(1));
    EXPECT_GT(age, -std::chrono::hours(1));
    const auto dirStatus = fs().status(dir.path());
    ASSERT_TRUE(dirStatus.has_value());
    EXPECT_EQ(dirStatus->type, FileType::Directory);
    EXPECT_EQ(dirStatus->size, 0u);
}

TEST(RealFileSystem, ListingIsSortedAndTyped) {
    const ScratchDir dir("fs-list");
    ASSERT_TRUE(fs().writeFileAtomic(dir.join("b.txt"), "").has_value());
    ASSERT_TRUE(fs().writeFileAtomic(dir.join("a.txt"), "").has_value());
    ASSERT_TRUE(fs().createDirectories(dir.join("c/d")).has_value());
    const std::vector<DirEntry> expected{
        {"a.txt", FileType::Regular}, {"b.txt", FileType::Regular}, {"c", FileType::Directory}};
    EXPECT_EQ(fs().listDirectory(dir.path()).value(), expected);
    EXPECT_TRUE(fs().listDirectory(dir.join("c/d")).value().empty());
}

TEST(RealFileSystem, CreateDirectoriesIsIdempotent) {
    const ScratchDir dir("fs-mkdir");
    ASSERT_TRUE(fs().createDirectories(dir.join("a/b/c")).has_value());
    ASSERT_TRUE(fs().createDirectories(dir.join("a/b/c")).has_value());
    ASSERT_TRUE(fs().createDirectories(dir.path()).has_value());
    EXPECT_EQ(fs().status(dir.join("a/b/c"))->type, FileType::Directory);
}

TEST(RealFileSystem, ErrorCodes) {
    const ScratchDir dir("fs-errors");
    const std::string file = dir.join("file");
    const std::string sub = dir.join("sub");
    ASSERT_TRUE(fs().writeFileAtomic(file, "x").has_value());
    ASSERT_TRUE(fs().createDirectories(sub + "/inner").has_value());
    const std::string missing = dir.join("missing");

    EXPECT_EQ(fs().readFile(missing).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs().readFile(sub).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(fs().status(missing).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs().status(dir.join("file/below")).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs().listDirectory(missing).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs().listDirectory(file).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(fs().createDirectories(file).error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(fs().createDirectories(dir.join("file/below")).error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(fs().removeFile(missing).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs().removeFile(sub).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(fs().removeDirectory(sub).error().code(), ErrorCode::IoError); // Not empty.
    EXPECT_EQ(fs().removeDirectory(file).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(fs().removeDirectory(missing).error().code(), ErrorCode::NotFound);
    EXPECT_FALSE(fs().exists(missing).value());
    EXPECT_TRUE(fs().exists(file).value());
    EXPECT_TRUE(fs().exists(sub).value());

    // The message names the operation and the path, and keeps the OS error number.
    const auto notFound = fs().readFile(missing);
    EXPECT_NE(notFound.error().message().find("readFile '" + missing + "'"), std::string::npos)
        << notFound.error().message();
#ifdef _WIN32
    EXPECT_NE(notFound.error().message().find("win32 error 2"), std::string::npos) << notFound.error().message();
#else
    EXPECT_NE(notFound.error().message().find("errno 2"), std::string::npos) << notFound.error().message();
#endif

    // Relative paths are never resolved against the current directory.
    for (const char* relative : {"file", "./file", "sub/inner"}) {
        EXPECT_EQ(fs().readFile(relative).error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().writeFileAtomic(relative, "x").error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().status(relative).error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().listDirectory(relative).error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().createDirectories(relative).error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().removeFile(relative).error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().removeDirectory(relative).error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().exists(relative).error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(RealFileSystem, NonAsciiNames) {
    const ScratchDir dir("fs-ünïcødé");
    const std::string sub = dir.join("ünïcødé 测试 😀");
    const std::string file = frost::io::joinPath(sub, "файл ✓.txt").value();
    ASSERT_TRUE(fs().createDirectories(sub).has_value());
    ASSERT_TRUE(fs().writeFileAtomic(file, "содержимое").has_value());
    EXPECT_EQ(fs().readFile(file).value(), "содержимое");
    EXPECT_EQ(fs().listDirectory(dir.path()).value(),
              (std::vector<DirEntry>{{"ünïcødé 测试 😀", FileType::Directory}}));
    EXPECT_EQ(fs().listDirectory(sub).value(), (std::vector<DirEntry>{{"файл ✓.txt", FileType::Regular}}));
    ASSERT_TRUE(fs().removeFile(file).has_value());
    ASSERT_TRUE(fs().removeDirectory(sub).has_value());
}

TEST(RealFileSystem, PathsBeyondMaxPath) {
    // Windows' MAX_PATH is 260 UTF-16 units. frost::io uses \\?\ paths internally, so this
    // must pass even where LongPathsEnabled is 0 (as on the reference machine).
    const ScratchDir dir("fs-long");
    std::string deep = dir.path();
    int depth = 0;
    while (deep.size() < 400) {
        auto next = frost::io::joinPath(deep, std::format("segment-{:02}-{}", depth++, std::string(40, 'x')));
        ASSERT_TRUE(next.has_value());
        deep = std::move(*next);
    }
    ASSERT_GT(deep.size(), 260u);
    ASSERT_TRUE(fs().createDirectories(deep).has_value());
    const std::string file = frost::io::joinPath(deep, "long-path file ✓.txt").value();
    ASSERT_GT(file.size(), 300u);
    ASSERT_TRUE(fs().writeFileAtomic(file, "deep content").has_value());
    EXPECT_EQ(fs().readFile(file).value(), "deep content");
    const auto status = fs().status(file);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->size, 12u);
    EXPECT_EQ(fs().listDirectory(deep).value(), (std::vector<DirEntry>{{"long-path file ✓.txt", FileType::Regular}}));
    ASSERT_TRUE(fs().writeFileAtomic(file, "replaced").has_value()); // The atomic replace works there too.
    EXPECT_EQ(fs().readFile(file).value(), "replaced");
    EXPECT_TRUE(noTempFiles(deep));
    ASSERT_TRUE(fs().removeFile(file).has_value());
    EXPECT_FALSE(fs().exists(file).value());
    // ScratchDir removes the deep tree through the same API.
}

#ifdef _WIN32
TEST(RealFileSystem, NamesWin32WouldRewriteAreRefused) {
    const ScratchDir dir("fs-names");
    for (const char* name : {"trail.", "trail ", "CON", "nul.txt", "COM1.log", "Aux .c"}) {
        const std::string path = dir.path() + "\\" + name;
        const auto result = fs().writeFileAtomic(path, "x");
        ASSERT_FALSE(result.has_value()) << name;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << name;
        EXPECT_EQ(fs().createDirectories(path + "\\sub").error().code(), ErrorCode::InvalidArgument) << name;
    }
    EXPECT_TRUE(fs().listDirectory(dir.path()).value().empty());

    // The explicit \\?\ form is the escape hatch: it names exactly the file written.
    ASSERT_TRUE(dir.path().size() > 3 && dir.path()[1] == ':') << dir.path();
    const std::string verbatim = "\\\\?\\" + dir.path() + "\\trail.";
    ASSERT_TRUE(fs().writeFileAtomic(verbatim, "literal").has_value());
    EXPECT_EQ(fs().readFile(verbatim).value(), "literal");
    EXPECT_EQ(fs().listDirectory(dir.path()).value(), (std::vector<DirEntry>{{"trail.", FileType::Regular}}));
    ASSERT_TRUE(fs().removeFile(verbatim).has_value());
}

TEST(RealFileSystem, ReadFileRefusesDevices) {
    // Only reachable through \\?\ now, but a device must never be "read to the end".
    const auto result = fs().readFile("\\\\?\\NUL");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << result.error().describe();
}

// Creates a directory junction with cmd.exe's mklink (junctions, unlike symlinks, need no privilege).
void makeJunction(const std::string& link, const std::string& target) {
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), MAX_PATH);
    ASSERT_GT(length, 0u);
    frost::io::ProcessSpec spec;
    spec.executable = frost::io::fromWide(std::wstring_view(system.data(), length)).value() + "\\cmd.exe";
    spec.arguments = {"/c", "mklink", "/J", link, target};
    spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
    const auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, "", 120s);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << result->stdoutBytes;
}

// A symbolic link, or false where creating one needs a privilege this process lacks
// (no Developer Mode, not elevated).
bool makeSymlink(const std::string& link, const std::string& target, bool directory) {
    const DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | (directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
    if (CreateSymbolicLinkW(frost::io::toWide(link).value().c_str(), frost::io::toWide(target).value().c_str(),
                            flags) == FALSE) {
        const DWORD error = GetLastError();
        EXPECT_EQ(error, static_cast<DWORD>(ERROR_PRIVILEGE_NOT_HELD)) << "CreateSymbolicLinkW failed: " << error;
        return false;
    }
    return true;
}
#else
TEST(RealFileSystem, ReadFileRefusesFifosAndDevicesWithoutBlocking) {
    // A FIFO without a writer would block open() and read() forever; devices never end.
    const ScratchDir dir("fs-fifo");
    const std::string fifo = dir.join("fifo");
    ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
    for (const std::string& path : {fifo, "/dev/zero"s, "/dev/null"s}) {
        const auto result = fs().readFile(path);
        ASSERT_FALSE(result.has_value()) << path;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << path;
        EXPECT_NE(result.error().message().find("not a regular file"), std::string::npos) << path;
    }
}

TEST(RealFileSystem, NewFilesGetTheUmaskMode) {
    const ScratchDir dir("fs-umask");
    const mode_t saved = umask(027);
    const std::string file = dir.join("new.txt");
    const auto written = fs().writeFileAtomic(file, "x");
    umask(saved);
    ASSERT_TRUE(written.has_value()) << written.error().describe();
    struct stat info{};
    ASSERT_EQ(stat(file.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 07777, 0640u); // 0666 & ~027, as open(O_CREAT) would give.
}

// POSIX ACL entry tags (acl(5), linux/posix_acl.h).
constexpr std::uint16_t kUserObj = 0x01;
constexpr std::uint16_t kUser = 0x02;
constexpr std::uint16_t kGroupObj = 0x04;
constexpr std::uint16_t kMask = 0x10;
constexpr std::uint16_t kOther = 0x20;

constexpr const char* kAccessAcl = "system.posix_acl_access";
constexpr const char* kDefaultAcl = "system.posix_acl_default";

struct AclEntry {
    std::uint16_t tag;
    std::uint16_t permissions;
    std::uint32_t id = 0xffffffffu; // ACL_UNDEFINED_ID, for entries that name no user or group.
};

// A POSIX ACL in the kernel's extended-attribute format (linux/posix_acl_xattr.h):
// a little-endian version word 2, then (tag, permissions, id) entries.
std::string aclXattr(std::initializer_list<AclEntry> entries) {
    std::string out;
    const auto put = [&out](std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out += static_cast<char>((value >> (8 * i)) & 0xffu);
        }
    };
    put(2, 4);
    for (const AclEntry& entry : entries) {
        put(entry.tag, 2);
        put(entry.permissions, 2);
        put(entry.id, 4);
    }
    return out;
}

// Sets the ACL attribute `name` of `path`. Returns 0 or the errno value: EOPNOTSUPP (also
// ENOTSUP on Linux) means the file system has no POSIX ACLs.
int setAcl(const std::string& path, const char* name, const std::string& acl) {
    return setxattr(path.c_str(), name, acl.data(), acl.size(), 0) == 0 ? 0 : errno;
}

// The access ACL of `path`, or nullopt when it has none.
std::optional<std::string> accessAcl(const std::string& path) {
    std::string value(4096, '\0');
    const ssize_t got = getxattr(path.c_str(), kAccessAcl, value.data(), value.size());
    if (got < 0) {
        const int error = errno;
        EXPECT_EQ(error, ENODATA) << path;
        return std::nullopt;
    }
    value.resize(static_cast<std::size_t>(got));
    return value;
}

TEST(RealFileSystem, NewFilesFollowTheDirectorysDefaultAcl) {
    // With a default ACL the kernel ignores the umask; a file written here must get the
    // same mode as one created with open(O_CREAT, 0666), not 0666 & ~umask.
    const ScratchDir dir("fs-acl");
    const std::string shared = dir.join("shared");
    ASSERT_TRUE(fs().createDirectories(shared).has_value());
    const std::string acl = aclXattr({{kUserObj, 7}, {kGroupObj, 7}, {kOther, 0}});
    if (const int error = setAcl(shared, kDefaultAcl, acl); error != 0) {
        if (error == EOPNOTSUPP) {
            GTEST_SKIP() << "the file system under " << shared << " has no POSIX ACL support";
        }
        FAIL() << "setxattr(system.posix_acl_default) failed: errno " << error;
    }
    const mode_t saved = umask(022);
    const std::string control = shared + "/control.txt";
    const int fd = open(control.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    const auto written = fs().writeFileAtomic(shared + "/saved.txt", "x");
    umask(saved);
    ASSERT_GE(fd, 0);
    close(fd);
    ASSERT_TRUE(written.has_value()) << written.error().describe();
    struct stat expected{};
    struct stat actual{};
    ASSERT_EQ(stat(control.c_str(), &expected), 0);
    ASSERT_EQ(stat((shared + "/saved.txt").c_str(), &actual), 0);
    EXPECT_EQ(actual.st_mode & 07777, expected.st_mode & 07777);
    EXPECT_EQ(actual.st_mode & 07777, 0660u); // The ACL's rw-rw----, not 0644 from the umask.
}

TEST(RealFileSystem, ReplacingKeepsOwnerGroupAndSetIdBitsWherePermitted) {
    const ScratchDir dir("fs-owner");
    const std::string file = dir.join("deploy.conf");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old").has_value());
    // An owner and group other than this process's where it may set them: root may use
    // any (1 is "daemon" on Linux); others keep their uid and use a supplementary group.
    const bool root = geteuid() == 0;
    const uid_t owner = root ? 1 : geteuid();
    gid_t group = root ? 1 : getegid();
    if (!root) {
        std::vector<gid_t> groups(static_cast<std::size_t>(std::max(getgroups(0, nullptr), 0)));
        const int count = getgroups(static_cast<int>(groups.size()), groups.data());
        for (int i = 0; i < count; ++i) {
            if (groups[static_cast<std::size_t>(i)] != getegid()) {
                group = groups[static_cast<std::size_t>(i)];
                break;
            }
        }
    }
    ASSERT_EQ(chown(file.c_str(), owner, group), 0);
    ASSERT_EQ(chmod(file.c_str(), 04750), 0);
    ASSERT_TRUE(fs().writeFileAtomic(file, "new").has_value());
    struct stat info{};
    ASSERT_EQ(stat(file.c_str(), &info), 0);
    EXPECT_EQ(info.st_uid, owner);
    EXPECT_EQ(info.st_gid, group) << (group == getegid() ? "(no second group to test with)" : "");
    EXPECT_EQ(info.st_mode & 07777, 04750u);
    EXPECT_EQ(fs().readFile(file).value(), "new");
}

TEST(RealFileSystem, ReplacingKeepsTheAccessAcl) {
    // Sharing one file with one person: 0640 plus `setfacl -m u:65534:rw`, so the mask is
    // rw- and st_mode reads 0660. Its group bits are the mask, not the group's permissions:
    // copying only the mode would give the whole group write access and lock out the user.
    const ScratchDir dir("fs-acl-keep");
    const std::string file = dir.join("shared.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old").has_value());
    ASSERT_EQ(chmod(file.c_str(), 0640), 0);
    const std::string acl = aclXattr({{kUserObj, 6}, {kUser, 6, 65534}, {kGroupObj, 4}, {kMask, 6}, {kOther, 0}});
    if (const int error = setAcl(file, kAccessAcl, acl); error != 0) {
        if (error == EOPNOTSUPP) {
            GTEST_SKIP() << "the file system under " << file << " has no POSIX ACL support";
        }
        FAIL() << "setxattr(system.posix_acl_access) failed: errno " << error;
    }
    ASSERT_TRUE(fs().writeFileAtomic(file, "new").has_value());
    EXPECT_EQ(accessAcl(file), acl);
    struct stat info{};
    ASSERT_EQ(stat(file.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 07777, 0660u);
    EXPECT_EQ(fs().readFile(file).value(), "new");
}

TEST(RealFileSystem, ReplacingDropsAnAclInheritedFromTheDirectory) {
    // The temp file inherits the directory's default ACL as its access ACL. The target has
    // none, so the result must have none either: the named user's rwx would otherwise be
    // limited only by the target's group bits, here r--, which the user never had.
    const ScratchDir dir("fs-acl-inherit");
    const std::string shared = dir.join("shared");
    ASSERT_TRUE(fs().createDirectories(shared).has_value());
    const std::string file = shared + "/plain.txt";
    ASSERT_TRUE(fs().writeFileAtomic(file, "old").has_value());
    ASSERT_EQ(chmod(file.c_str(), 0640), 0);
    const std::string inherited = aclXattr({{kUserObj, 7}, {kUser, 7, 65534}, {kGroupObj, 5}, {kMask, 7}, {kOther, 0}});
    if (const int error = setAcl(shared, kDefaultAcl, inherited); error != 0) {
        if (error == EOPNOTSUPP) {
            GTEST_SKIP() << "the file system under " << shared << " has no POSIX ACL support";
        }
        FAIL() << "setxattr(system.posix_acl_default) failed: errno " << error;
    }
    ASSERT_TRUE(fs().writeFileAtomic(file, "new").has_value());
    EXPECT_FALSE(accessAcl(file).has_value());
    struct stat info{};
    ASSERT_EQ(stat(file.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 07777, 0640u);
    EXPECT_EQ(fs().readFile(file).value(), "new");
}

// A supplementary group of this process other than its effective group, if it has one.
std::optional<gid_t> supplementaryGroup() {
    std::vector<gid_t> groups(static_cast<std::size_t>(std::max(getgroups(0, nullptr), 0)));
    const int count = getgroups(static_cast<int>(groups.size()), groups.data());
    for (int i = 0; i < count; ++i) {
        if (groups[static_cast<std::size_t>(i)] != getegid()) {
            return groups[static_cast<std::size_t>(i)];
        }
    }
    return std::nullopt;
}

// Writes `text` to the /proc control file `path`; whether all of it was accepted.
bool writeControlFile(const char* path, std::string_view text) noexcept {
    const int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool written = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    close(fd);
    return written;
}

// The exit status of inUserNamespace's child when it could not map its ids.
constexpr int kNoUserNamespace = 77;

// What inUserNamespace hands its child: the body and the id maps to write.
template<class Body>
struct NamespaceChild {
    Body& body;
    std::string uidMap;
    std::string gidMap;

    // The clone() entry point; its result is the child's exit status. Unprivileged
    // processes must deny setgroups before they may write a gid map.
    static int run(void* self) {
        auto& child = *static_cast<NamespaceChild*>(self);
        if (!writeControlFile("/proc/self/uid_map", child.uidMap) ||
            !writeControlFile("/proc/self/setgroups", "deny") ||
            !writeControlFile("/proc/self/gid_map", child.gidMap)) {
            return kNoUserNamespace;
        }
        return child.body();
    }
};

/*
 * Runs `body` in a child process inside a new user namespace that maps only this
 * process's uid and gid. Every other id, supplementary groups included, then shows as
 * the overflow id, and fchown to it fails with EINVAL: a group this process cannot keep,
 * without needing root to set one up. Returns what `body` returned (0-76), or nullopt
 * where unprivileged user namespaces are unavailable.
 *
 * clone(CLONE_NEWUSER) rather than fork + unshare: unshare(CLONE_NEWUSER) refuses a
 * multithreaded caller, and under ThreadSanitizer a forked child already runs TSan's
 * background thread. Without CLONE_VM the child gets a copy of this address space, as
 * with fork, running on a stack of its own; when `body` returns, glibc ends the child
 * with the exit system call, so no test-framework or exit handlers run twice.
 */
template<class Body>
std::optional<int> inUserNamespace(Body body) {
    NamespaceChild<Body> setup{body, std::format("{} {} 1", geteuid(), geteuid()),
                               std::format("{} {} 1", getegid(), getegid())};
    // mmap, not the heap: AddressSanitizer would otherwise see stack frames inside a heap block.
    constexpr std::size_t kStackBytes = 1024 * 1024;
    void* stack = mmap(nullptr, kStackBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        ADD_FAILURE() << "mmap failed: errno " << errno;
        return -1;
    }
    const pid_t child =
        clone(&NamespaceChild<Body>::run, static_cast<char*>(stack) + kStackBytes, CLONE_NEWUSER | SIGCHLD, &setup);
    const int cloneError = errno;
    int status = 0;
    while (child > 0 && waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    munmap(stack, kStackBytes);
    if (child < 0) {
        // EPERM, ENOSPC, EUSERS: unprivileged user namespaces are disabled or exhausted here.
        if (cloneError == EPERM || cloneError == ENOSPC || cloneError == EUSERS) {
            return std::nullopt;
        }
        ADD_FAILURE() << "clone(CLONE_NEWUSER) failed: errno " << cloneError;
        return -1;
    }
    if (!WIFEXITED(status)) {
        ADD_FAILURE() << "the namespace child did not exit normally: status " << status;
        return -1;
    }
    if (WEXITSTATUS(status) == kNoUserNamespace) {
        return std::nullopt;
    }
    return WEXITSTATUS(status);
}

TEST(RealFileSystem, ReplacingNarrowsTheModeWhereTheGroupCannotBeKept) {
    // In the namespace this user's supplementary group is unmapped: the replacement file
    // gets the process's own group, so the mode must not grant anyone more than before.
    const std::optional<gid_t> group = supplementaryGroup();
    if (!group) {
        GTEST_SKIP() << "this user has no supplementary group to lose";
    }
    const ScratchDir dir("fs-lost-group");
    const std::string plain = dir.join("plain.txt");
    const std::string denied = dir.join("denied.txt");
    for (const std::string& file : {plain, denied}) {
        ASSERT_TRUE(fs().writeFileAtomic(file, "old").has_value());
        ASSERT_EQ(chown(file.c_str(), static_cast<uid_t>(-1), *group), 0);
    }
    ASSERT_EQ(chmod(plain.c_str(), 02664), 0);
    ASSERT_EQ(chmod(denied.c_str(), 0664), 0);
    // user:65534:--- shuts out one user whom the group and other classes would let read.
    const int aclError = setAcl(denied, kAccessAcl,
                                aclXattr({{kUserObj, 6}, {kUser, 0, 65534}, {kGroupObj, 4}, {kMask, 6}, {kOther, 4}}));
    ASSERT_TRUE(aclError == 0 || aclError == EOPNOTSUPP) << "setxattr failed: errno " << aclError;
    const bool withAcl = aclError == 0;

    const std::optional<int> outcome = inUserNamespace([&] {
        if (!fs().writeFileAtomic(plain, "new").has_value()) {
            return 1;
        }
        if (withAcl && !fs().writeFileAtomic(denied, "new").has_value()) {
            return 2;
        }
        return 0;
    });
    if (!outcome) {
        GTEST_SKIP() << "unprivileged user namespaces are unavailable";
    }
    ASSERT_EQ(*outcome, 0) << "writeFileAtomic failed in the namespace";

    // rw-rw-r-- with S_ISGID: old members may now be others, and others may be members of
    // the new group, so both classes keep only r--, and S_ISGID goes.
    struct stat info{};
    ASSERT_EQ(stat(plain.c_str(), &info), 0);
    EXPECT_NE(info.st_gid, *group);
    EXPECT_EQ(info.st_mode & 07777, 0644u);
    EXPECT_EQ(fs().readFile(plain).value(), "new");
    if (withAcl) {
        // The ACL is not copied to a file with another group; without it, the denied user
        // would fall into the group or other class, so both get only what the user had.
        ASSERT_EQ(stat(denied.c_str(), &info), 0);
        EXPECT_EQ(info.st_mode & 07777, 0600u);
        EXPECT_FALSE(accessAcl(denied).has_value());
        EXPECT_EQ(fs().readFile(denied).value(), "new");
    }
}

TEST(RealFileSystem, ReadOnlyTargetsAreReplaced) {
    // rename needs write access to the directory only, so POSIX replaces a 0444 file (Windows
    // refuses; see ReadOnlyTargetsAreRefused), and the new file keeps the read-only mode.
    const ScratchDir dir("fs-readonly-target");
    const std::string file = dir.join("frozen.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old").has_value());
    ASSERT_EQ(chmod(file.c_str(), 0444), 0);
    ASSERT_TRUE(fs().writeFileAtomic(file, "new").has_value());
    EXPECT_EQ(fs().readFile(file).value(), "new");
    struct stat info{};
    ASSERT_EQ(stat(file.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 07777, 0444u);
}

TEST(RealFileSystem, ReadFileRefusesSockets) {
    // Sockets refuse open() with ENXIO, which must not read as "no such file".
    const ScratchDir dir("fs-sock");
    const std::string path = dir.join("sock");
    const int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(sock, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_LT(path.size(), sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    ASSERT_EQ(bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    const auto result = fs().readFile(path);
    close(sock);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << result.error().describe();
    EXPECT_NE(result.error().message().find("not a regular file"), std::string::npos);
    EXPECT_TRUE(fs().exists(path).value());
}

TEST(RealFileSystem, ReadFileWaitsForALeaseBreak) {
    // Another process holds a write lease (as Samba oplocks or NFS delegations do). The
    // non-blocking open fails at once, but readFile must still return the content once
    // the holder releases the lease, as a blocking open would.
    const ScratchDir dir("fs-lease");
    const std::string file = dir.join("leased.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "content").has_value());
    frost::io::ProcessSpec spec;
    spec.executable = frost::io::normalizePath(FROST_IO_TEST_CHILD).value();
    spec.arguments = {"lease", file};
    auto holder = frost::io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(holder.has_value()) << holder.error().describe();
    std::string first;
    std::array<char, 256> buffer{};
    while (first.find('\n') == std::string::npos) {
        const auto got = (*holder)->readStdout(buffer);
        if (!got || *got == 0) {
            break;
        }
        first.append(buffer.data(), *got);
    }
    if (first.starts_with("no lease")) {
        static_cast<void>((*holder)->communicate({}, 30s));
        GTEST_SKIP() << "this file system grants no write leases: " << first;
    }
    ASSERT_EQ(first, "leased\n");
    const auto start = std::chrono::steady_clock::now();
    const auto content = fs().readFile(file);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(content.has_value()) << content.error().describe();
    EXPECT_EQ(*content, "content");
    EXPECT_LT(elapsed, 20s); // The holder released it; the kernel's revocation would take 45 s.
    const auto rest = (*holder)->communicate({}, 60s);
    ASSERT_TRUE(rest.has_value()) << rest.error().describe();
    EXPECT_EQ(rest->stdoutBytes, "released\n");
}
#endif

/*
 * POSIX ".." semantics that the real file system and MemFs must share (MemFs
 * promises to behave like the real one). `root` is an existing empty directory.
 */
void checkDotDotSemantics(frost::io::IFileSystem& files, const std::string& root) {
    const std::string w = root + "/w";
    ASSERT_TRUE(files.createDirectories(w).has_value());
    ASSERT_TRUE(files.writeFileAtomic(w + "/file", "f").has_value());
    // createDirectories works like mkdir -p: the directory before ".." is created first.
    ASSERT_TRUE(files.createDirectories(w + "/new/../out").has_value());
    EXPECT_EQ(files.status(w + "/new").value().type, FileType::Directory);
    EXPECT_EQ(files.status(w + "/out").value().type, FileType::Directory);
    EXPECT_EQ(files.createDirectories(w + "/file/../x").error().code(), ErrorCode::AlreadyExists);
    // Everything else resolves ".." only through existing directories.
    EXPECT_EQ(files.readFile(w + "/new/../file").value(), "f");
    EXPECT_EQ(files.readFile(w + "/missing/../file").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(files.readFile(w + "/file/../file").error().code(), ErrorCode::NotFound);
    ASSERT_TRUE(files.writeFileAtomic(w + "/new/../g", "g").has_value());
    EXPECT_EQ(files.readFile(w + "/g").value(), "g");
    // A final ".." is never an entry to write or remove.
    for (const std::string& path : {w + "/new/..", w + "/.."}) {
        EXPECT_EQ(files.writeFileAtomic(path, "x").error().code(), ErrorCode::InvalidArgument) << path;
        EXPECT_EQ(files.removeFile(path).error().code(), ErrorCode::InvalidArgument) << path;
        EXPECT_EQ(files.removeDirectory(path).error().code(), ErrorCode::InvalidArgument) << path;
    }
    ASSERT_TRUE(files.removeDirectory(w + "/out/../new").has_value());
    EXPECT_FALSE(files.exists(w + "/new").value());
    EXPECT_TRUE(files.exists(w + "/out").value());
}

#ifndef _WIN32
TEST(RealFileSystem, DotDotSemantics) {
    const ScratchDir dir("fs-dotdot");
    checkDotDotSemantics(fs(), dir.path());
}
#endif

TEST(MemFsAgainstRealSemantics, DotDotSemantics) {
    frost::ManualClock clock;
    frost::io::MemFs memory(clock, frost::io::PathStyle::Posix);
    ASSERT_TRUE(memory.createDirectories("/scratch").has_value());
    checkDotDotSemantics(memory, "/scratch");
}

TEST(RealFileSystem, LinksAreRemovedWithoutFollowingThem) {
    const ScratchDir dir("fs-links");
    const std::string target = dir.join("target");
    ASSERT_TRUE(fs().createDirectories(target).has_value());
    const std::string kept = frost::io::joinPath(target, "keep.txt").value();
    ASSERT_TRUE(fs().writeFileAtomic(kept, "kept").has_value());
    const std::string gone = dir.join("gone");
    ASSERT_TRUE(fs().createDirectories(gone).has_value());

    std::vector<std::string> directoryLinks;
    std::vector<std::string> fileLinks;
    std::vector<std::string> danglingLinks;
#ifdef _WIN32
    ASSERT_NO_FATAL_FAILURE(makeJunction(dir.join("junction"), target));
    directoryLinks.push_back(dir.join("junction"));
    ASSERT_NO_FATAL_FAILURE(makeJunction(dir.join("dangling-junction"), gone));
    danglingLinks.push_back(dir.join("dangling-junction"));
    if (makeSymlink(dir.join("dirlink"), target, true)) {
        directoryLinks.push_back(dir.join("dirlink"));
        ASSERT_TRUE(makeSymlink(dir.join("filelink"), kept, false));
        fileLinks.push_back(dir.join("filelink"));
    }
#else
    ASSERT_EQ(symlink(target.c_str(), dir.join("dirlink").c_str()), 0);
    directoryLinks.push_back(dir.join("dirlink"));
    ASSERT_EQ(symlink(kept.c_str(), dir.join("filelink").c_str()), 0);
    fileLinks.push_back(dir.join("filelink"));
    ASSERT_EQ(symlink(gone.c_str(), dir.join("dangling").c_str()), 0);
    danglingLinks.push_back(dir.join("dangling"));
#endif
    ASSERT_TRUE(fs().removeDirectory(gone).has_value());

    // listDirectory reports the links themselves; status follows them.
    for (const DirEntry& entry : fs().listDirectory(dir.path()).value()) {
        EXPECT_EQ(entry.type, entry.name == "target" ? FileType::Directory : FileType::Symlink) << entry.name;
    }
    for (const std::string& link : directoryLinks) {
        EXPECT_EQ(fs().status(link).value().type, FileType::Directory) << link;
        EXPECT_EQ(fs().listDirectory(link).value(), (std::vector<DirEntry>{{"keep.txt", FileType::Regular}}));
        // Not a directory to removeDirectory (as rmdir on a POSIX symlink), never emptied...
        EXPECT_EQ(fs().removeDirectory(link).error().code(), ErrorCode::InvalidArgument) << link;
        // ...and removeFile removes the link, not the target or its contents.
        ASSERT_TRUE(fs().removeFile(link).has_value()) << link;
        EXPECT_FALSE(fs().exists(link).value()) << link;
    }
    for (const std::string& link : fileLinks) {
        EXPECT_EQ(fs().readFile(link).value(), "kept");
        ASSERT_TRUE(fs().removeFile(link).has_value()) << link;
    }
    for (const std::string& link : danglingLinks) {
        EXPECT_FALSE(fs().exists(link).value()) << link; // status follows the link: NotFound.
        ASSERT_TRUE(fs().removeFile(link).has_value()) << link;
    }
    EXPECT_EQ(fs().readFile(kept).value(), "kept");
    EXPECT_EQ(fs().listDirectory(dir.path()).value(), (std::vector<DirEntry>{{"target", FileType::Directory}}));
}

// A link to a directory (a junction on Windows, which needs no privilege; a symlink on POSIX).
void makeDirectoryLink(const std::string& link, const std::string& target) {
#ifdef _WIN32
    ASSERT_NO_FATAL_FAILURE(makeJunction(link, target));
#else
    ASSERT_EQ(symlink(target.c_str(), link.c_str()), 0) << link;
#endif
}

TEST(RealFileSystem, CreateDirectoriesStopsAtADanglingLink) {
    // A link whose target is gone is no directory to create children in: the call fails at the
    // link with AlreadyExists, as mkdir -p does, on every platform.
    const ScratchDir dir("fs-mkdir-dangling");
    const std::string gone = dir.join("gone");
    ASSERT_TRUE(fs().createDirectories(gone).has_value());
    const std::string link = dir.join("link");
    ASSERT_NO_FATAL_FAILURE(makeDirectoryLink(link, gone));
    ASSERT_TRUE(fs().removeDirectory(gone).has_value());
    for (const std::string& path : {frost::io::joinPath(link, "sub").value(), link}) {
        const auto result = fs().createDirectories(path);
        ASSERT_FALSE(result.has_value()) << path;
        EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists) << result.error().describe();
        EXPECT_NE(result.error().message().find("createDirectories '" + path + "': "), std::string::npos)
            << result.error().message();
    }
    EXPECT_FALSE(fs().exists(gone).value()); // Nothing was created through the link.
    ASSERT_TRUE(fs().removeFile(link).has_value());
}

TEST(ScratchDirSupport, RemoveTreeRemovesALinkWithoutFollowingIt) {
    // ScratchDir clears leftovers with removeTree, so a link planted at its name must not lead
    // it into the directory the link points to.
    const ScratchDir dir("fs-removetree-link");
    const std::string target = dir.join("target");
    ASSERT_TRUE(fs().createDirectories(target).has_value());
    const std::string kept = frost::io::joinPath(target, "keep.txt").value();
    ASSERT_TRUE(fs().writeFileAtomic(kept, "kept").has_value());
    const std::string link = dir.join("link");
    ASSERT_NO_FATAL_FAILURE(makeDirectoryLink(link, target));
    EXPECT_TRUE(frost::test::removeTree(fs(), link));
    EXPECT_EQ(fs().readFile(kept).value(), "kept");
    EXPECT_EQ(fs().listDirectory(dir.path()).value(), (std::vector<DirEntry>{{"target", FileType::Directory}}));
    EXPECT_TRUE(frost::test::removeTree(fs(), link)); // Missing paths are fine.
}

TEST(ScratchDirSupport, JoinRecordsAFailureForARejectedName) {
    // A quiet "" would let a check on the joined path pass vacuously, so join reports the rejection.
    const ScratchDir dir("fs-join-rejected");
    std::string joined = "unset";
    EXPECT_NONFATAL_FAILURE(joined = dir.join(""), "InvalidArgument: joinPath needs a non-empty relative name");
    EXPECT_EQ(joined, "");
}

} // namespace
