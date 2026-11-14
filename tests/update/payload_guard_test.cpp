// SPDX-License-Identifier: Apache-2.0
// Real local payload sharing, writable-map refusal and retained path tests; no MSI launch.
#include <frost/update/payload_guard.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <msiquery.h>
#include <sodium.h>
#include <winioctl.h>
#endif

namespace {
TEST(UpdatePayload, InvalidSizeNeverAcquiresAuthority) {
    EXPECT_FALSE(frost::update::PayloadGuard::acquire({}, {}));
    frost::update::Release release;
    release.size = 2ULL * 1024 * 1024 * 1024 + 1;
    EXPECT_FALSE(frost::update::PayloadGuard::acquire({}, release));
#ifndef _WIN32
    release.size = 1;
    auto unsupported = frost::update::PayloadGuard::acquire("/tmp/not-an-update", release);
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code(), frost::ErrorCode::Unsupported);
#endif
}
#ifdef _WIN32
struct NativeHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~NativeHandle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
class UpdatePayloadFile : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_GE(sodium_init(), 0);
        std::uint64_t random{};
        randombytes_buf(&random, sizeof(random));
        root = std::filesystem::temp_directory_path() / ("frost-payload-guard-" + std::to_string(random));
        ASSERT_TRUE(std::filesystem::create_directory(root));
        created = true;
        stage = root / "stage";
        ASSERT_TRUE(std::filesystem::create_directory(stage));
        file = stage / L"candidate å.msi";
        bytes = std::string(100000, 'f');
        {
            std::ofstream out(file, std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            ASSERT_TRUE(out.good());
        }
        release.size = bytes.size();
        crypto_hash_sha256(release.digest.data(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    }
    void TearDown() override {
        if (created) {
            std::error_code error;
            std::filesystem::remove_all(root, error);
            EXPECT_FALSE(error) << error.message();
        }
    }
    std::filesystem::path root, stage, file;
    std::string bytes;
    frost::update::Release release;
    bool created = false;
};
TEST_F(UpdatePayloadFile, AllowsReadersAndDeniesWritesDeletionAndAncestorRename) {
    auto held = frost::update::PayloadGuard::acquire(file, release);
    ASSERT_TRUE(held) << held.error().message();
    EXPECT_TRUE(held->validate());
    EXPECT_NE(held->installerPath().find("\xc3\xa5"), std::string_view::npos);
    NativeHandle reader{CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
    ASSERT_NE(reader.value, INVALID_HANDLE_VALUE);
    std::array<char, 8> contents{};
    DWORD count = 0;
    ASSERT_TRUE(ReadFile(reader.value, contents.data(), static_cast<DWORD>(contents.size()), &count, nullptr));
    EXPECT_EQ(std::string_view(contents.data(), count), std::string_view(bytes).substr(0, contents.size()));
    NativeHandle writer{CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, 0, nullptr)};
    EXPECT_EQ(writer.value, INVALID_HANDLE_VALUE);
    EXPECT_FALSE(DeleteFileW(file.c_str()));
    EXPECT_FALSE(MoveFileExW(stage.c_str(), (root / "renamed").c_str(), 0));
    NativeHandle writableMap{CreateFileMappingW(reader.value, nullptr, PAGE_READWRITE, 0, 0, nullptr)};
    EXPECT_EQ(writableMap.value, nullptr);
    EXPECT_TRUE(held->validate());
}
TEST_F(UpdatePayloadFile, RejectsWritableMappingAfterItsOriginalFileHandleClosed) {
    NativeHandle writer{CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                                    nullptr)};
    ASSERT_NE(writer.value, INVALID_HANDLE_VALUE);
    NativeHandle section{CreateFileMappingW(writer.value, nullptr, PAGE_READWRITE, 0, 0, nullptr)};
    ASSERT_NE(section.value, nullptr);
    struct View {
        void* data;
        ~View() {
            if (data)
                UnmapViewOfFile(data);
        }
    } view{MapViewOfFile(section.value, FILE_MAP_WRITE, 0, 0, 0)};
    ASSERT_NE(view.data, nullptr);
    ASSERT_TRUE(CloseHandle(writer.value));
    writer.value = INVALID_HANDLE_VALUE;
    EXPECT_FALSE(frost::update::PayloadGuard::acquire(file, release));
    auto* mapped = static_cast<char*>(view.data);
    mapped[0] = 'x'; // The mapping is still writable despite the closed original file handle.
    EXPECT_EQ(mapped[0], 'x');
    mapped[0] = bytes[0];
    ASSERT_TRUE(UnmapViewOfFile(view.data));
    view.data = nullptr;
    ASSERT_TRUE(CloseHandle(section.value));
    section.value = INVALID_HANDLE_VALUE;
    auto held = frost::update::PayloadGuard::acquire(file, release);
    ASSERT_TRUE(held) << held.error().message();
    EXPECT_TRUE(held->validate());
}
TEST_F(UpdatePayloadFile, RejectsWrongDigestHardLinksAndAmbiguousPaths) {
    auto wrong = release;
    wrong.digest[0] ^= 1;
    EXPECT_FALSE(frost::update::PayloadGuard::acquire(file, wrong));
    const auto alias = root / "alias.msi";
    ASSERT_TRUE(CreateHardLinkW(alias.c_str(), file.c_str(), nullptr));
    EXPECT_FALSE(frost::update::PayloadGuard::acquire(file, release));
    ASSERT_TRUE(DeleteFileW(alias.c_str()));
    EXPECT_FALSE(frost::update::PayloadGuard::acquire(file.native() + L":stream", release));
    EXPECT_FALSE(frost::update::PayloadGuard::acquire("relative.msi", release));
    EXPECT_FALSE(frost::update::PayloadGuard::acquire(L"\\\\server\\share\\payload.msi", release));
    auto held = frost::update::PayloadGuard::acquire(file, release);
    ASSERT_TRUE(held) << held.error().message();
}
TEST_F(UpdatePayloadFile, MoveAndCancellationReleaseOnlyTheOwnedGuard) {
    const auto started = std::chrono::steady_clock::now();
    {
        auto held = frost::update::PayloadGuard::acquire(file, release);
        ASSERT_TRUE(held) << held.error().message();
        auto moved = std::move(*held);
        EXPECT_FALSE(held->validate());
        EXPECT_TRUE(held->installerPath().empty());
        EXPECT_TRUE(moved.validate());
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    NativeHandle writer{CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, 0, nullptr)};
    EXPECT_NE(writer.value, INVALID_HANDLE_VALUE);
}
TEST_F(UpdatePayloadFile, OccupiedAncestorRejectsAttributeOnlyJunctionConversion) {
    const auto destination = root / L"junction-target";
    const auto empty = root / L"empty-control";
    ASSERT_TRUE(std::filesystem::create_directory(destination));
    ASSERT_TRUE(std::filesystem::create_directory(empty));
    const auto substitute = L"\\??\\" + destination.native();
    const auto& display = destination.native();
    struct Header {
        DWORD tag;
        WORD length, reserved, substituteOffset, substituteLength, printOffset, printLength;
    };
    static_assert(sizeof(Header) == 16);
    const auto stringBytes = (substitute.size() + display.size() + 2) * sizeof(wchar_t);
    ASSERT_LT(stringBytes + 8, 65536U);
    Header header{IO_REPARSE_TAG_MOUNT_POINT,
                  static_cast<WORD>(8 + stringBytes),
                  0,
                  0,
                  static_cast<WORD>(substitute.size() * sizeof(wchar_t)),
                  static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t)),
                  static_cast<WORD>(display.size() * sizeof(wchar_t))};
    std::vector<std::byte> request(sizeof(header) + stringBytes);
    std::memcpy(request.data(), &header, sizeof(header));
    std::memcpy(request.data() + sizeof(header), substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(request.data() + sizeof(header) + header.printOffset, display.c_str(),
                (display.size() + 1) * sizeof(wchar_t));
    const auto attempt = [&](const std::filesystem::path& path) -> DWORD {
        NativeHandle directoryHandle{
            CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (directoryHandle.value == INVALID_HANDLE_VALUE)
            return GetLastError();
        DWORD returned = 0;
        if (!DeviceIoControl(directoryHandle.value, FSCTL_SET_REPARSE_POINT, request.data(),
                             static_cast<DWORD>(request.size()), nullptr, 0, &returned, nullptr))
            return GetLastError();
        // Remove any successfully created reparse before owned fixture cleanup.
        Header remove{IO_REPARSE_TAG_MOUNT_POINT, 0, 0, 0, 0, 0, 0};
        EXPECT_TRUE(DeviceIoControl(directoryHandle.value, FSCTL_DELETE_REPARSE_POINT, &remove, 8, nullptr, 0,
                                    &returned, nullptr));
        return ERROR_SUCCESS;
    };
    // The empty-directory control proves this access mask and request can create
    // a junction; a permission-only failure would not prove the occupied invariant.
    ASSERT_EQ(attempt(empty), ERROR_SUCCESS);
    auto held = frost::update::PayloadGuard::acquire(file, release);
    ASSERT_TRUE(held) << held.error().message();
    EXPECT_EQ(attempt(stage), static_cast<DWORD>(ERROR_DIR_NOT_EMPTY));
    EXPECT_TRUE(held->validate());
}
TEST_F(UpdatePayloadFile, WindowsInstallerCanReadTheContinuouslyGuardedDatabase) {
    ASSERT_TRUE(DeleteFileW(file.c_str()));
    struct Database {
        MSIHANDLE handle = 0;
        ~Database() {
            if (handle)
                MsiCloseHandle(handle);
        }
    };
    {
        Database createdDatabase;
        ASSERT_EQ(MsiOpenDatabaseW(file.c_str(), MSIDBOPEN_CREATE, &createdDatabase.handle), ERROR_SUCCESS);
        ASSERT_EQ(MsiDatabaseCommit(createdDatabase.handle), ERROR_SUCCESS);
    }
    {
        std::ifstream stream(file, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(stream), {});
        ASSERT_FALSE(stream.bad());
        ASSERT_FALSE(bytes.empty());
    }
    release.size = bytes.size();
    crypto_hash_sha256(release.digest.data(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    auto held = frost::update::PayloadGuard::acquire(file, release);
    ASSERT_TRUE(held) << held.error().message();
    Database reader;
    // Database read only: no package execution, registration or installation.
    const auto argument = held->installerPath();
    const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument.data(),
                                          static_cast<int>(argument.size()), nullptr, 0);
    ASSERT_GT(units, 0);
    std::wstring installerArgument(static_cast<std::size_t>(units), L'\0');
    ASSERT_EQ(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument.data(), static_cast<int>(argument.size()),
                                  installerArgument.data(), units),
              units);
    ASSERT_TRUE(installerArgument.starts_with(L"\\\\?\\"));
    ASSERT_EQ(MsiOpenDatabaseW(installerArgument.c_str(), MSIDBOPEN_READONLY, &reader.handle), ERROR_SUCCESS);
    EXPECT_TRUE(held->validate());
}
#endif
} // namespace
