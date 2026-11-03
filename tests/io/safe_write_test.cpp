// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for io::IFileSystem::safeWrite on the real file system (ADR 0028): new and
// replaced files, read-only targets refused unless overwriteReadOnly, hard links kept by an
// in-place rewrite, symlinks kept by writing their target, and never a temp file left behind;
// on Windows a file another program holds open is refused with Busy and keeps its attributes and
// creation time across a save, on POSIX the file mode survives.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include "internal.hpp"
#include "save_alias_journal_win.hpp"
#include "save_metadata_win.hpp"

#include <windows.h>

#include <aclapi.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#endif

namespace {

using frost::ErrorCode;
using frost::io::SafeWriteMethod;
using frost::io::SafeWriteOptions;
using frost::test::ScratchDir;

frost::io::IFileSystem& fs() { return frost::io::realFileSystem(); }

std::string read(const std::string& file) {
    auto bytes = fs().readFile(file);
    return bytes ? std::move(*bytes) : std::string("<unreadable>");
}

std::filesystem::path native(const std::string& file) { return frost::io::toStdPath(file).value(); }

// Whether a writeFileAtomic or safeWrite temp file is left in `directory`.
bool tempLeft(const std::string& directory) {
    auto entries = fs().listDirectory(directory);
    EXPECT_TRUE(entries);
    for (const auto& entry : entries.value_or(std::vector<frost::io::DirEntry>{})) {
        if (entry.name.find(".frost-tmp-") != std::string::npos) {
            return true;
        }
    }
    return false;
}

void setReadOnly(const std::string& file, bool readOnly) {
    std::error_code error;
    std::filesystem::permissions(native(file),
                                 std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                                     std::filesystem::perms::others_write,
                                 readOnly ? std::filesystem::perm_options::remove : std::filesystem::perm_options::add,
                                 error);
    ASSERT_FALSE(error) << error.message();
}

TEST(SafeWrite, CreatesAndReplaces) {
    const ScratchDir dir("safe-write-basic");
    const std::string file = dir.join("a.txt");
    auto created = fs().safeWrite(file, "one\r\n", SafeWriteOptions{});
    ASSERT_TRUE(created) << created.error().describe();
    EXPECT_EQ(created->method, SafeWriteMethod::Created);
    auto replaced = fs().safeWrite(file, "two\n", SafeWriteOptions{});
    ASSERT_TRUE(replaced) << replaced.error().describe();
    EXPECT_EQ(replaced->method, SafeWriteMethod::Replaced);
    EXPECT_EQ(replaced->target, frost::io::normalizePath(file).value());
    EXPECT_EQ(read(file), "two\n");
    EXPECT_FALSE(tempLeft(dir.path()));

    // A directory is refused; a missing parent fails without creating anything.
    EXPECT_FALSE(fs().safeWrite(dir.path(), "x", SafeWriteOptions{}));
    auto orphan = fs().safeWrite(dir.join("missing/b.txt"), "x", SafeWriteOptions{});
    ASSERT_FALSE(orphan);
    EXPECT_EQ(orphan.error().code(), ErrorCode::NotFound) << orphan.error().describe();
}

TEST(SafeWrite, ReadOnlyTargetsNeedOverwrite) {
    const ScratchDir dir("safe-write-readonly");
    const std::string file = dir.join("ro.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "original"));
    setReadOnly(file, true);
    auto refused = fs().safeWrite(file, "new", SafeWriteOptions{});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied) << refused.error().describe();
    EXPECT_EQ(read(file), "original");
    EXPECT_FALSE(tempLeft(dir.path()));

    auto forced = fs().safeWrite(file, "new", SafeWriteOptions{.overwriteReadOnly = true});
    ASSERT_TRUE(forced) << forced.error().describe();
    EXPECT_EQ(read(file), "new");
    const auto perms = std::filesystem::status(native(file)).permissions();
    EXPECT_NE(perms & std::filesystem::perms::owner_write, std::filesystem::perms::none);
    setReadOnly(file, false);
}

TEST(SafeWrite, HardLinksStayLinked) {
    const ScratchDir dir("safe-write-hardlink");
    const std::string file = dir.join("a.txt");
    const std::string other = dir.join("b.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "shared content that is long enough\n"));
    std::error_code error;
    std::filesystem::create_hard_link(native(file), native(other), error);
    ASSERT_FALSE(error) << error.message();
    for (const std::string_view content : {std::string_view("short\n"), std::string_view("a longer second save\n")}) {
        auto written = fs().safeWrite(file, content, SafeWriteOptions{});
        ASSERT_TRUE(written) << written.error().describe();
        EXPECT_EQ(written->method, SafeWriteMethod::InPlace);
        EXPECT_EQ(read(file), content);
        EXPECT_EQ(read(other), content); // Still the same file.
    }
    EXPECT_EQ(std::filesystem::hard_link_count(native(file)), 2U);
    EXPECT_FALSE(tempLeft(dir.path()));
}

TEST(SafeWrite, SymlinksStayLinks) {
    const ScratchDir dir("safe-write-symlink");
    ASSERT_TRUE(fs().createDirectories(dir.join("real")));
    const std::string target = dir.join("real/target.txt");
    const std::string link = dir.join("link.txt");
    ASSERT_TRUE(fs().writeFileAtomic(target, "old"));
    std::error_code error;
    std::filesystem::create_symlink(native(target), native(link), error);
    if (error) {
#ifdef _WIN32
        GTEST_SKIP() << "creating a symlink needs Developer Mode or elevation here: " << error.message();
#else
        FAIL() << error.message();
#endif
    }
    auto written = fs().safeWrite(link, "new", SafeWriteOptions{});
    ASSERT_TRUE(written) << written.error().describe();
    EXPECT_EQ(written->target, frost::io::normalizePath(target).value());
    EXPECT_TRUE(std::filesystem::is_symlink(native(link)));
    EXPECT_EQ(read(target), "new");
    EXPECT_FALSE(tempLeft(dir.path()));
    EXPECT_FALSE(tempLeft(dir.join("real")));
}

#ifdef _WIN32
TEST(SafeWrite, LockedFilesAreBusyAndUnchanged) {
    const ScratchDir dir("safe-write-locked");
    const std::string file = dir.join("locked.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "original"));
    const std::wstring wide = frost::io::toWide(file).value();
    // Another program holds the file open without sharing (Visual Studio's build, an antivirus scan).
    HANDLE held = CreateFileW(wide.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(held, INVALID_HANDLE_VALUE) << GetLastError();
    auto refused = fs().safeWrite(file, "new", SafeWriteOptions{});
    CloseHandle(held);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::Busy) << refused.error().describe();
    EXPECT_EQ(read(file), "original");
    EXPECT_FALSE(tempLeft(dir.path()));
    auto later = fs().safeWrite(file, "new", SafeWriteOptions{});
    ASSERT_TRUE(later) << later.error().describe();
    EXPECT_EQ(read(file), "new");
}

TEST(SafeWrite, KeepsAttributesAndCreationTime) {
    const ScratchDir dir("safe-write-attributes");
    const std::string file = dir.join("hidden.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "original"));
    const std::wstring wide = frost::io::toWide(file).value();
    ASSERT_NE(SetFileAttributesW(wide.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE), FALSE);
    FILETIME created{};
    {
        // A creation time well in the past (2001-01-01), so a replacement's own time would differ.
        SYSTEMTIME old{2001, 1, 1, 1, 0, 0, 0, 0};
        ASSERT_NE(SystemTimeToFileTime(&old, &created), FALSE);
        HANDLE handle = CreateFileW(wide.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(handle, INVALID_HANDLE_VALUE);
        EXPECT_NE(SetFileTime(handle, &created, nullptr, nullptr), FALSE);
        CloseHandle(handle);
    }
    auto written = fs().safeWrite(file, "replaced", SafeWriteOptions{});
    ASSERT_TRUE(written) << written.error().describe();
    EXPECT_EQ(written->method, SafeWriteMethod::Replaced);
    EXPECT_EQ(read(file), "replaced");
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    EXPECT_NE(attributes & FILE_ATTRIBUTE_HIDDEN, 0U);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    ASSERT_NE(GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data), FALSE);
    EXPECT_EQ(data.ftCreationTime.dwLowDateTime, created.dwLowDateTime);
    EXPECT_EQ(data.ftCreationTime.dwHighDateTime, created.dwHighDateTime);
    ASSERT_NE(SetFileAttributesW(wide.c_str(), FILE_ATTRIBUTE_NORMAL), FALSE);
}
TEST(SafeWrite, CopyCandidateSerializesConcurrentSingleLinkSaves) {
    const ScratchDir dir("safe-write-copy-concurrent");
    const std::string file = dir.join("v.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old"));
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::optional<frost::Result<frost::io::SafeWriteResult>> first, second;
    const std::string a(65536, 'a'), b(49157, 'b');
    std::thread left([&] {
        ++ready;
        while (!start.load())
            std::this_thread::yield();
        first = fs().safeWrite(file, a, {});
    });
    std::thread right([&] {
        ++ready;
        while (!start.load())
            std::this_thread::yield();
        second = fs().safeWrite(file, b, {});
    });
    while (ready.load() != 2)
        std::this_thread::yield();
    start.store(true);
    left.join();
    right.join();
    ASSERT_TRUE(first && *first);
    ASSERT_TRUE(second && *second);
    EXPECT_EQ((*first)->method, SafeWriteMethod::Replaced);
    EXPECT_EQ((*second)->method, SafeWriteMethod::Replaced);
    const auto contents = read(file);
    EXPECT_TRUE(contents == a || contents == b);
    EXPECT_FALSE(tempLeft(dir.path()));
}
TEST(SafeWrite, CopyCandidatePreservesAlternateStreamsAndSecurity) {
    const ScratchDir dir("safe-write-copy-metadata");
    const std::string file = dir.join("v.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old"));
    const std::wstring wide = frost::io::toWide(file).value();
    const std::wstring stream = wide + L":frost";
    frost::io::detail::UniqueHandle out(
        CreateFileW(stream.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(out.valid()) << GetLastError();
    DWORD count = 0;
    ASSERT_TRUE(WriteFile(out.get(), "stream", 6, &count, nullptr));
    ASSERT_EQ(count, 6u);
    out.reset();
    constexpr SECURITY_INFORMATION bits =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    const auto security = [&]() {
        DWORD size = 0;
        GetFileSecurityW(wide.c_str(), bits, nullptr, 0, &size);
        std::vector<unsigned char> result(size);
        if (!size || !GetFileSecurityW(wide.c_str(), bits, result.data(), size, &size))
            result.clear();
        return result;
    };
    const auto before = security();
    ASSERT_FALSE(before.empty());
    auto saved = fs().safeWrite(file, "new", {});
    ASSERT_TRUE(saved) << saved.error().describe();
    EXPECT_EQ(security(), before);
    EXPECT_EQ(read(file), "new");
    frost::io::detail::UniqueHandle in(CreateFileW(stream.c_str(), GENERIC_READ,
                                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                   OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(in.valid()) << GetLastError();
    char bytes[6]{};
    DWORD got = 0;
    ASSERT_TRUE(ReadFile(in.get(), bytes, sizeof(bytes), &got, nullptr));
    in.reset();
    EXPECT_EQ(std::string_view(bytes, got), "stream");
    EXPECT_FALSE(tempLeft(dir.path()));
}

TEST(SafeWrite, CopyCandidatePreservesProtectedDacl) {
    const ScratchDir dir("safe-write-protected-dacl");
    const std::string file = dir.join("v.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old"));
    const std::wstring wide = frost::io::toWide(file).value();
    frost::io::detail::UniqueHandle h(CreateFileW(wide.c_str(), READ_CONTROL | WRITE_DAC,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                  OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(h.valid());
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD readError = GetSecurityInfo(h.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                            nullptr, &descriptor);
    ASSERT_EQ(readError, ERROR_SUCCESS);
    const DWORD writeError =
        SetSecurityInfo(h.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, dacl, nullptr);
    LocalFree(descriptor);
    ASSERT_EQ(writeError, ERROR_SUCCESS);
    h.reset();
    const auto security = [&]() {
        DWORD size = 0;
        GetFileSecurityW(wide.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &size);
        std::vector<unsigned char> result(size);
        if (!size || !GetFileSecurityW(wide.c_str(), DACL_SECURITY_INFORMATION, result.data(), size, &size))
            result.clear();
        return result;
    };
    const auto before = security();
    ASSERT_FALSE(before.empty());
    auto saved = fs().safeWrite(file, "new", {});
    ASSERT_TRUE(saved) << saved.error().describe();
    EXPECT_EQ(security(), before);
    EXPECT_EQ(read(file), "new");
}
TEST(SafeWrite, CopyCandidateRefusesObjectIdBeforeMutation) {
    const ScratchDir dir("safe-write-object-id");
    const std::string file = dir.join("v.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "original"));
    const std::wstring wide = frost::io::toWide(file).value();
    frost::io::detail::UniqueHandle h(CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                  OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(h.valid()) << GetLastError();
    FILE_OBJECTID_BUFFER id{};
    DWORD count = 0;
    ASSERT_TRUE(DeviceIoControl(h.get(), FSCTL_CREATE_OR_GET_OBJECT_ID, nullptr, 0, &id, sizeof(id), &count, nullptr));
    h.reset();
    auto saved = fs().safeWrite(file, "new", {});
    ASSERT_FALSE(saved);
    EXPECT_NE(saved.error().message().find("object ID"), std::string::npos);
    EXPECT_EQ(read(file), "original");
    EXPECT_FALSE(tempLeft(dir.path()));
}

TEST(SafeWrite, CopyCandidatePreservesShortName) {
    const ScratchDir dir("safe-write-short-name");
    const std::string file = dir.join("long-target-name.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "original"));
    const std::wstring wide = frost::io::toWide(file).value();
    frost::io::detail::UniqueHandle h(CreateFileW(wide.c_str(), DELETE,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    ASSERT_TRUE(h.valid()) << GetLastError();
    ASSERT_TRUE(SetFileShortNameW(h.get(), L"FRSV~1.TXT"));
    h.reset();
    auto saved = fs().safeWrite(file, "new", {});
    ASSERT_TRUE(saved) << saved.error().describe();
    EXPECT_EQ(read(file), "new");
    EXPECT_FALSE(tempLeft(dir.path()));
}

TEST(SafeWrite, CopyCandidatePreservesNtfsCompression) {
    const ScratchDir dir("safe-write-compression");
    const std::string file = dir.join("v.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old"));
    const std::wstring wide = frost::io::toWide(file).value();
    frost::io::detail::UniqueHandle h(CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                  OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(h.valid()) << GetLastError();
    USHORT format = COMPRESSION_FORMAT_DEFAULT;
    DWORD count = 0;
    const BOOL compressed =
        DeviceIoControl(h.get(), FSCTL_SET_COMPRESSION, &format, sizeof(format), nullptr, 0, &count, nullptr);
    const DWORD error = GetLastError();
    h.reset();
    if (!compressed && (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED))
        GTEST_SKIP() << "compression unavailable";
    ASSERT_TRUE(compressed) << error;
    auto saved = fs().safeWrite(file, "new", {});
    ASSERT_TRUE(saved) << saved.error().describe();
    EXPECT_NE(GetFileAttributesW(wide.c_str()) & FILE_ATTRIBUTE_COMPRESSED, 0u);
    EXPECT_EQ(read(file), "new");
}
// Beta policy refuses object-ID-bearing files without changing the existing file.
TEST(SafeWrite, ObjectIdRefusedWithoutChangingFile) {
    const ScratchDir dir("safe-write-object-id-contract");
    const std::string file = dir.join("v.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "original"));
    const std::wstring wide = frost::io::toWide(file).value();
    frost::io::detail::UniqueHandle h(CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                  OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(h.valid()) << GetLastError();
    FILE_OBJECTID_BUFFER id{};
    DWORD count = 0;
    ASSERT_TRUE(DeviceIoControl(h.get(), FSCTL_CREATE_OR_GET_OBJECT_ID, nullptr, 0, &id, sizeof(id), &count, nullptr));
    frost::io::save_metadata::Metadata before;
    ASSERT_EQ(frost::io::save_metadata::queryMetadata(h.get(), wide, before), ERROR_SUCCESS);
    h.reset();
    ASSERT_TRUE(before.hasObjectId);
    auto saved = fs().safeWrite(file, "new", {});
    ASSERT_FALSE(saved);
    EXPECT_NE(saved.error().message().find("Copy your edits to a new, unencrypted file"), std::string::npos);
    h.reset(CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(h.valid());
    frost::io::save_metadata::Metadata after;
    ASSERT_EQ(frost::io::save_metadata::queryMetadata(h.get(), wide, after), ERROR_SUCCESS);
    h.reset();
    EXPECT_TRUE(after.hasObjectId);
    EXPECT_EQ(after.objectId, before.objectId);
    EXPECT_EQ(read(file), "original");
    EXPECT_FALSE(tempLeft(dir.path()));
}

TEST(SafeWrite, ObjectIdHardlinkRefusedWithoutChangingFile) {
    const ScratchDir dir("safe-write-object-id-hardlink");
    const std::string file = dir.join("v.txt"), alias = dir.join("alias.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "original"));
    const auto wide = frost::io::toWide(file).value();
    const auto wideAlias = frost::io::toWide(alias).value();
    frost::io::detail::UniqueHandle h(CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                  OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(h.valid());
    FILE_OBJECTID_BUFFER id{};
    DWORD count = 0;
    ASSERT_TRUE(DeviceIoControl(h.get(), FSCTL_CREATE_OR_GET_OBJECT_ID, nullptr, 0, &id, sizeof(id), &count, nullptr));
    h.reset();
    ASSERT_TRUE(CreateHardLinkW(wideAlias.c_str(), wide.c_str(), nullptr));
    const auto saved = fs().safeWrite(file, "new", {});
    ASSERT_FALSE(saved);
    EXPECT_NE(saved.error().message().find("Copy your edits to a new, unencrypted file"), std::string::npos);
    EXPECT_EQ(read(file), "original");
    EXPECT_EQ(read(alias), "original");
}

TEST(SafeWrite, ShortNamePreservedOnSuccessfulSave) {
    const ScratchDir dir("safe-write-short-name-contract");
    const std::string file = dir.join("long-target-name.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "original"));
    const std::wstring wide = frost::io::toWide(file).value();
    frost::io::detail::UniqueHandle h(CreateFileW(wide.c_str(), DELETE,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    ASSERT_TRUE(h.valid()) << GetLastError();
    ASSERT_TRUE(SetFileShortNameW(h.get(), L"FRSV~1.TXT"));
    h.reset();
    auto saved = fs().safeWrite(file, "new", {});
    ASSERT_TRUE(saved) << saved.error().describe();
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(wide.c_str(), &data);
    ASSERT_NE(find, INVALID_HANDLE_VALUE) << GetLastError();
    FindClose(find);
    EXPECT_STREQ(data.cAlternateFileName, L"FRSV~1.TXT");
    EXPECT_EQ(read(file), "new");
}
TEST(SafeWrite, AliasJournalRecoversBeforeAndAfterPublication) {
    for (int published = 0; published < 2; ++published) {
        SCOPED_TRACE(published);
        const ScratchDir dir(published ? "alias-after" : "alias-before");
        const std::string file = dir.join("long-target-name.txt");
        ASSERT_TRUE(fs().writeFileAtomic(file, "old"));
        const std::wstring wide = frost::io::toWin32Path(file).value();
        frost::io::detail::UniqueHandle original(CreateFileW(
            wide.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        ASSERT_TRUE(original.valid());
        ASSERT_TRUE(SetFileShortNameW(original.get(), L"FRSV~1.TXT"));
        frost::io::detail::SaveFileIdentity oldId{};
        ASSERT_TRUE(frost::io::detail::saveFileIdentity(original.get(), oldId));
        const std::wstring temp = wide.substr(0, wide.find_last_of(L"\\/") + 1) + L".alias.frost-tmp-42-1";
        frost::io::detail::UniqueHandle replacement(CreateFileW(temp.c_str(),
                                                                GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
                                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        ASSERT_TRUE(replacement.valid());
        DWORD written = 0;
        ASSERT_TRUE(WriteFile(replacement.get(), "new", 3, &written, nullptr));
        ASSERT_EQ(written, 3u);
        ASSERT_TRUE(FlushFileBuffers(replacement.get()));
        frost::io::detail::SaveFileIdentity newId{};
        ASSERT_TRUE(frost::io::detail::saveFileIdentity(replacement.get(), newId));
        frost::io::detail::AliasRecord record;
        record.target = wide;
        record.temp = temp;
        record.alias = L"FRSV~1.TXT";
        record.header.original = oldId;
        record.header.prepared = newId;
        const std::wstring marker = frost::io::detail::aliasJournalPath(wide);
        ASSERT_EQ(frost::io::detail::writeAliasRecord(marker, record), ERROR_SUCCESS);
        if (published)
            ASSERT_EQ(frost::io::detail::publishCopy(replacement.get(), wide), ERROR_SUCCESS);
        replacement.reset();
        original.reset();
        ASSERT_EQ(frost::io::detail::recoverAliasRecord(wide, published ? newId : oldId), ERROR_SUCCESS);
        EXPECT_EQ(frost::io::detail::recoverAliasRecord(wide, published ? newId : oldId), ERROR_SUCCESS);
        EXPECT_EQ(GetFileAttributesW(marker.c_str()), INVALID_FILE_ATTRIBUTES);
        EXPECT_EQ(GetFileAttributesW(temp.c_str()), INVALID_FILE_ATTRIBUTES);
        WIN32_FIND_DATAW info{};
        HANDLE find = FindFirstFileW(wide.c_str(), &info);
        ASSERT_NE(find, INVALID_HANDLE_VALUE);
        FindClose(find);
        EXPECT_STREQ(info.cAlternateFileName, L"FRSV~1.TXT");
        EXPECT_EQ(read(file), published ? "new" : "old");
    }
}

TEST(SafeWrite, AliasJournalRetainsRecordOnAliasCollision) {
    const ScratchDir dir("alias-collision");
    const std::string file = dir.join("long-target-name.txt");
    const std::string other = dir.join("long-other-name.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old"));
    ASSERT_TRUE(fs().writeFileAtomic(other, "other"));
    const std::wstring wide = frost::io::toWin32Path(file).value();
    const std::wstring wideOther = frost::io::toWin32Path(other).value();
    frost::io::detail::UniqueHandle original(CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
                                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                         nullptr, OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(original.valid());
    frost::io::detail::SaveFileIdentity identity{};
    ASSERT_TRUE(frost::io::detail::saveFileIdentity(original.get(), identity));
    frost::io::detail::UniqueHandle otherHandle(
        CreateFileW(wideOther.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    ASSERT_TRUE(otherHandle.valid());
    ASSERT_TRUE(SetFileShortNameW(otherHandle.get(), L"FRSV~1.TXT"));
    otherHandle.reset();
    frost::io::detail::AliasRecord record;
    record.target = wide;
    record.temp = wide.substr(0, wide.find_last_of(L"\\/") + 1) + L".alias.frost-tmp-42-1";
    record.alias = L"FRSV~1.TXT";
    record.header.prepared = identity;
    record.header.original = identity;
    ++record.header.original.indexLow;
    const std::wstring marker = frost::io::detail::aliasJournalPath(wide);
    ASSERT_EQ(frost::io::detail::writeAliasRecord(marker, record), ERROR_SUCCESS);
    EXPECT_NE(frost::io::detail::recoverAliasRecord(wide, identity), ERROR_SUCCESS);
    EXPECT_NE(GetFileAttributesW(marker.c_str()), INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(read(file), "old");
    EXPECT_EQ(read(other), "other");
}
TEST(SafeWrite, MissingShortAliasNeverCreatesSplitFileAfterInterruptedPublish) {
    const ScratchDir dir("alias-missing-recovery");
    const std::string file = dir.join("long-target-name.txt");
    const std::string oldAlias = dir.join("FRSV~1.TXT");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old"));
    const std::wstring wide = frost::io::toWin32Path(file).value();
    frost::io::detail::UniqueHandle original(CreateFileW(wide.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    ASSERT_TRUE(original.valid());
    ASSERT_TRUE(SetFileShortNameW(original.get(), L"FRSV~1.TXT"));
    frost::io::detail::SaveFileIdentity oldId{};
    ASSERT_TRUE(frost::io::detail::saveFileIdentity(original.get(), oldId));
    const std::wstring temp = wide.substr(0, wide.find_last_of(L"\\/") + 1) + L".alias.frost-tmp-42-1";
    frost::io::detail::UniqueHandle replacement(CreateFileW(temp.c_str(), GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
                                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(replacement.valid());
    DWORD count = 0;
    ASSERT_TRUE(WriteFile(replacement.get(), "new", 3, &count, nullptr));
    ASSERT_EQ(count, 3u);
    ASSERT_TRUE(FlushFileBuffers(replacement.get()));
    frost::io::detail::SaveFileIdentity newId{};
    ASSERT_TRUE(frost::io::detail::saveFileIdentity(replacement.get(), newId));
    frost::io::detail::AliasRecord record;
    record.target = wide;
    record.temp = temp;
    record.alias = L"FRSV~1.TXT";
    record.header.original = oldId;
    record.header.prepared = newId;
    const auto marker = frost::io::detail::aliasJournalPath(wide);
    ASSERT_EQ(frost::io::detail::writeAliasRecord(marker, record), ERROR_SUCCESS);
    ASSERT_EQ(frost::io::detail::publishCopy(replacement.get(), wide), ERROR_SUCCESS);
    replacement.reset();
    original.reset();
    // Some NTFS volumes retain the alias on the new inode during rename.
    // Force the missing-name crash state only after verifying the published ID.
    frost::io::detail::UniqueHandle published(CreateFileW(wide.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    ASSERT_TRUE(published.valid());
    frost::io::detail::SaveFileIdentity publishedId{};
    ASSERT_TRUE(frost::io::detail::saveFileIdentity(published.get(), publishedId));
    ASSERT_EQ(publishedId, newId);
    ASSERT_TRUE(SetFileShortNameW(published.get(), L""));
    published.reset();

    ASSERT_FALSE(fs().exists(oldAlias).value());
    auto shortSave = fs().safeWrite(oldAlias, "split", {});
    ASSERT_FALSE(shortSave);
    EXPECT_NE(shortSave.error().message().find("alias recovery"), std::string::npos);
    EXPECT_FALSE(fs().exists(oldAlias).value());
    EXPECT_EQ(read(file), "new");
    EXPECT_NE(GetFileAttributesW(marker.c_str()), INVALID_FILE_ATTRIBUTES);
    auto longSave = fs().safeWrite(file, "final", {});
    ASSERT_TRUE(longSave) << longSave.error().describe();
    EXPECT_EQ(read(file), "final");
    EXPECT_EQ(read(oldAlias), "final");
}
TEST(SafeWrite, MissingFileAllowsUnrelatedValidAliasJournal) {
    const ScratchDir dir("alias-unrelated");
    const std::string other = dir.join("other-long-name.txt");
    ASSERT_TRUE(fs().writeFileAtomic(other, "old"));
    const std::wstring wide = frost::io::toWin32Path(other).value();
    frost::io::detail::UniqueHandle h(CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                  OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(h.valid());
    frost::io::detail::SaveFileIdentity identity{};
    ASSERT_TRUE(frost::io::detail::saveFileIdentity(h.get(), identity));
    frost::io::detail::AliasRecord record;
    record.target = wide;
    record.temp = wide.substr(0, wide.find_last_of(L"\\/") + 1) + L".alias.frost-tmp-42-1";
    record.alias = L"OTHER~1.TXT";
    record.header.original = identity;
    record.header.prepared = identity;
    ++record.header.prepared.indexLow;
    const auto marker = frost::io::detail::aliasJournalPath(wide);
    ASSERT_EQ(frost::io::detail::writeAliasRecord(marker, record), ERROR_SUCCESS);
    const auto created = fs().safeWrite(dir.join("new.txt"), "new", {});
    ASSERT_TRUE(created) << created.error().describe();
    EXPECT_EQ(read(dir.join("new.txt")), "new");
    EXPECT_NE(GetFileAttributesW(marker.c_str()), INVALID_FILE_ATTRIBUTES);
}

TEST(SafeWrite, MissingFileRefusesMalformedOwnedAliasJournal) {
    const ScratchDir dir("alias-malformed");
    const std::string other = dir.join("other-long-name.txt");
    ASSERT_TRUE(fs().writeFileAtomic(other, "old"));
    const std::wstring wide = frost::io::toWin32Path(other).value();
    const auto marker = frost::io::detail::aliasJournalPath(wide);
    frost::io::detail::UniqueHandle f(
        CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr));
    ASSERT_TRUE(f.valid());
    DWORD count = 0;
    ASSERT_TRUE(WriteFile(f.get(), "x", 1, &count, nullptr));
    f.reset();
    const std::string missing = dir.join("new.txt");
    const auto created = fs().safeWrite(missing, "new", {});
    ASSERT_FALSE(created);
    EXPECT_FALSE(fs().exists(missing).value());
    EXPECT_NE(GetFileAttributesW(marker.c_str()), INVALID_FILE_ATTRIBUTES);
}

TEST(SafeWrite, AliasJournalRejectsEqualOriginalAndPreparedIdentity) {
    const ScratchDir dir("alias-equal-id");
    const std::string file = dir.join("long-target-name.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old"));
    const std::wstring wide = frost::io::toWin32Path(file).value();
    frost::io::detail::UniqueHandle h(CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                  OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(h.valid());
    frost::io::detail::SaveFileIdentity identity{};
    ASSERT_TRUE(frost::io::detail::saveFileIdentity(h.get(), identity));
    frost::io::detail::AliasRecord record;
    record.target = wide;
    record.temp = wide.substr(0, wide.find_last_of(L"\\/") + 1) + L".alias.frost-tmp-42-1";
    record.alias = L"FRSV~1.TXT";
    record.header.original = identity;
    record.header.prepared = identity;
    const auto marker = frost::io::detail::aliasJournalPath(wide);
    ASSERT_EQ(frost::io::detail::writeAliasRecord(marker, record), ERROR_SUCCESS);
    EXPECT_EQ(frost::io::detail::recoverAliasRecord(wide, identity), ERROR_INVALID_DATA);
    EXPECT_NE(GetFileAttributesW(marker.c_str()), INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(read(file), "old");
}
TEST(SafeWrite, MissingShortAliasThroughJunctionDoesNotSplitFile) {
    const ScratchDir dir("alias-junction");
    const std::string real = dir.join("real"), link = dir.join("link");
    ASSERT_TRUE(fs().createDirectories(real));
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), MAX_PATH);
    ASSERT_GT(length, 0u);
    frost::io::ProcessSpec spec;
    spec.executable = frost::io::fromWide(std::wstring_view(system.data(), length)).value() + "\\cmd.exe";
    spec.arguments = {"/c", "mklink", "/J", link, real};
    spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
    const auto junction =
        frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, "", std::chrono::seconds{30});
    ASSERT_TRUE(junction);
    ASSERT_EQ(junction->exitCode, 0) << junction->stdoutBytes;
    const std::string file = dir.join("real/long-target-name.txt"), oldAlias = dir.join("link/FRSV~1.TXT");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old"));
    const std::wstring wide = frost::io::toWin32Path(file).value();
    frost::io::detail::UniqueHandle original(CreateFileW(wide.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    ASSERT_TRUE(original.valid());
    ASSERT_TRUE(SetFileShortNameW(original.get(), L"FRSV~1.TXT"));
    frost::io::detail::SaveFileIdentity oldId{};
    ASSERT_TRUE(frost::io::detail::saveFileIdentity(original.get(), oldId));
    const std::wstring temp = wide.substr(0, wide.find_last_of(L"\\/") + 1) + L".alias.frost-tmp-42-1";
    frost::io::detail::UniqueHandle replacement(CreateFileW(temp.c_str(), GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
                                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(replacement.valid());
    DWORD count = 0;
    ASSERT_TRUE(WriteFile(replacement.get(), "new", 3, &count, nullptr));
    ASSERT_EQ(count, 3u);
    ASSERT_TRUE(FlushFileBuffers(replacement.get()));
    frost::io::detail::SaveFileIdentity newId{};
    ASSERT_TRUE(frost::io::detail::saveFileIdentity(replacement.get(), newId));
    frost::io::detail::AliasRecord record;
    record.target = wide;
    record.temp = temp;
    record.alias = L"FRSV~1.TXT";
    record.header.original = oldId;
    record.header.prepared = newId;
    const auto marker = frost::io::detail::aliasJournalPath(wide);
    ASSERT_EQ(frost::io::detail::writeAliasRecord(marker, record), ERROR_SUCCESS);
    ASSERT_EQ(frost::io::detail::publishCopy(replacement.get(), wide), ERROR_SUCCESS);
    replacement.reset();
    original.reset();
    // Some NTFS volumes retain the alias on the new inode during rename.
    // Force the missing-name crash state only after verifying the published ID.
    frost::io::detail::UniqueHandle published(CreateFileW(wide.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    ASSERT_TRUE(published.valid());
    frost::io::detail::SaveFileIdentity publishedId{};
    ASSERT_TRUE(frost::io::detail::saveFileIdentity(published.get(), publishedId));
    ASSERT_EQ(publishedId, newId);
    ASSERT_TRUE(SetFileShortNameW(published.get(), L""));
    published.reset();

    const auto shortSave = fs().safeWrite(oldAlias, "split", {});
    ASSERT_FALSE(shortSave);
    EXPECT_NE(shortSave.error().message().find("alias recovery"), std::string::npos);
    EXPECT_FALSE(fs().exists(oldAlias).value());
    EXPECT_EQ(read(file), "new");
    const auto longSave = fs().safeWrite(dir.join("link/long-target-name.txt"), "final", {});
    ASSERT_TRUE(longSave) << longSave.error().describe();
    EXPECT_EQ(read(oldAlias), "final");
    ASSERT_TRUE(fs().removeFile(link));
}
#else
TEST(SafeWrite, KeepsTheFileMode) {
    const ScratchDir dir("safe-write-mode");
    const std::string file = dir.join("script.sh");
    ASSERT_TRUE(fs().writeFileAtomic(file, "#!/bin/sh\n"));
    ASSERT_EQ(chmod(file.c_str(), 0750), 0);
    auto written = fs().safeWrite(file, "#!/bin/sh\necho hi\n", SafeWriteOptions{});
    ASSERT_TRUE(written) << written.error().describe();
    struct stat info{};
    ASSERT_EQ(stat(file.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 07777, 0750U);
    EXPECT_EQ(read(file), "#!/bin/sh\necho hi\n");
}
#endif

} // namespace
