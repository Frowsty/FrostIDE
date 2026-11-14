// SPDX-License-Identifier: Apache-2.0
// Real private stages and target interference; these tests make no Apply/global Undo claim.
#include "frost/io/file_system.hpp"
#include "frost/io/file_transaction_prepare.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {
using namespace frost::io;
using frost::ErrorCode;

// Interference is an independent native actor, not a change to Prepare's retained handles.
// MoveFileEx replacement and MSVC fstream's default share mode cannot express these Windows
// operations on retained open objects. Use explicit sharing/POSIX rename, retaining every oracle.
#ifdef _WIN32
struct MutationHandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
using MutationHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, MutationHandleCloser>;

frost::Result<FILE_ID_INFO> mutationIdentity(HANDLE file, std::string_view path) {
    FILE_ID_INFO identity{};
    if (!GetFileInformationByHandleEx(file, FileIdInfo, &identity, sizeof(identity)))
        return std::unexpected(lastWin32Error("interference: actual file identity", path));
    return identity;
}

bool sameMutationIdentity(const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
    return a.VolumeSerialNumber == b.VolumeSerialNumber &&
           std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0;
}

frost::Result<void> mutationWrite(HANDLE file, std::string_view bytes, std::string_view path) {
    std::size_t used = 0;
    while (used < bytes.size()) {
        const auto wanted = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - used, 65536));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + used, wanted, &written, nullptr))
            return std::unexpected(lastWin32Error("interference: write actual bytes", path));
        if (written == 0)
            return frost::fail(ErrorCode::IoError, "interference: native write made no progress");
        used += written;
    }
    if (!FlushFileBuffers(file))
        return std::unexpected(lastWin32Error("interference: flush actual bytes", path));
    return {};
}
#endif

frost::Result<void> replaceForInterference(std::string_view path, std::string_view bytes) {
#ifdef _WIN32
    auto destination = toWin32Path(path);
    if (!destination)
        return std::unexpected(destination.error());
    const std::string replacementPath = std::string(path) + ".owned-interference";
    auto replacement = toWin32Path(replacementPath);
    if (!replacement)
        return std::unexpected(replacement.error());
    constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    MutationHandle old(CreateFileW(destination->c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (old.get() == INVALID_HANDLE_VALUE)
        return std::unexpected(lastWin32Error("interference: open actual displaced file", path));
    auto oldId = mutationIdentity(old.get(), path);
    if (!oldId)
        return std::unexpected(oldId.error());
    MutationHandle source(CreateFileW(replacement->c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE, sharing,
                                      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (source.get() == INVALID_HANDLE_VALUE)
        return std::unexpected(lastWin32Error("interference: create exclusive replacement", replacementPath));
    auto written = mutationWrite(source.get(), bytes, replacementPath);
    if (!written)
        return written;
    auto sourceId = mutationIdentity(source.get(), replacementPath);
    if (!sourceId)
        return std::unexpected(sourceId.error());
    if (sameMutationIdentity(*oldId, *sourceId))
        return frost::fail(ErrorCode::Internal, "interference replacement did not create a distinct actual object");
    const auto bytesInName = destination->size() * sizeof(wchar_t);
    if (bytesInName > std::numeric_limits<DWORD>::max() - offsetof(FILE_RENAME_INFO, FileName) - sizeof(wchar_t))
        return frost::fail(ErrorCode::OutOfRange, "interference: native destination name too long");
    // The padded NULL-root absolute form is the actual previously probed Windows profile.
    // POSIX_SEMANTICS explicitly permits replacing a destination with retained open handles:
    // https://learn.microsoft.com/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information
    std::vector<std::byte> storage(offsetof(FILE_RENAME_INFO, FileName) + bytesInName + sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(bytesInName);
    std::memcpy(rename->FileName, destination->data(), bytesInName);
    if (!SetFileInformationByHandle(source.get(), FileRenameInfoEx, rename, static_cast<DWORD>(storage.size())))
        return std::unexpected(lastWin32Error("interference: POSIX replace retained destination", path));
    MutationHandle installed(CreateFileW(destination->c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (installed.get() == INVALID_HANDLE_VALUE)
        return std::unexpected(lastWin32Error("interference: reopen actual replacement", path));
    auto installedId = mutationIdentity(installed.get(), path);
    if (!installedId)
        return std::unexpected(installedId.error());
    if (!sameMutationIdentity(*installedId, *sourceId))
        return frost::fail(ErrorCode::Busy, "interference: actual installed object differs from owned replacement");
    return {};
#else
    return realFileSystem().writeFileAtomic(path, bytes);
#endif
}

frost::Result<void> overwriteForInterference(std::string_view path, std::string_view bytes) {
#ifdef _WIN32
    auto wide = toWin32Path(path);
    if (!wide)
        return std::unexpected(wide.error());
    MutationHandle file(CreateFileW(wide->c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE)
        return std::unexpected(lastWin32Error("interference: open same-object writer with DELETE sharing", path));
    auto before = mutationIdentity(file.get(), path);
    if (!before)
        return std::unexpected(before.error());
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file.get(), zero, nullptr, FILE_BEGIN) || !SetEndOfFile(file.get()))
        return std::unexpected(lastWin32Error("interference: truncate same owned object", path));
    auto written = mutationWrite(file.get(), bytes, path);
    if (!written)
        return written;
    auto after = mutationIdentity(file.get(), path);
    if (!after)
        return std::unexpected(after.error());
    if (!sameMutationIdentity(*before, *after))
        return frost::fail(ErrorCode::Internal, "interference: in-place mutation changed actual object identity");
    return {};
#else
    auto native = toStdPath(path);
    if (!native)
        return std::unexpected(native.error());
    std::ofstream writer(*native, std::ios::binary | std::ios::trunc);
    if (!writer)
        return frost::fail(ErrorCode::IoError, "interference: open native stream writer failed");
    writer.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    writer.flush();
    if (!writer)
        return frost::fail(ErrorCode::IoError, "interference: native stream write/flush failed");
    return {};
#endif
}

class FileTransactionPrepare : public ::testing::Test {
protected:
    frost::test::ScratchDir scratch{"transaction-prepare"};
    IFileSystem& fs = realFileSystem();
    std::string target = scratch.join("source.cpp");
    std::string store = scratch.join("private-recovery");

    void SetUp() override {
        ASSERT_TRUE(fs.createDirectories(store));
#ifndef _WIN32
        ASSERT_EQ(::chmod(store.c_str(), 0700), 0);
#endif
        ASSERT_TRUE(fs.writeFileAtomic(target, "before\r\n"));
    }
};

TEST_F(FileTransactionPrepare, NativePrepareKeepsTargetAndStagesExactBinaryImage) {
    const std::string before("\xEF\xBB\xBFone\r\n\0two", 12);
    const std::string after("new\0bytes\r\n", 11);
    ASSERT_TRUE(fs.writeFileAtomic(target, before));
    auto prepared = prepareFileImages(target, store, after, before.size(), after.size());
    ASSERT_TRUE(prepared) << prepared.error().describe();
    ASSERT_NE(prepared->receipt(), nullptr);
    const auto receipt = *prepared->receipt();
    EXPECT_EQ(prepared->beforeBytes(), before);
    EXPECT_EQ(prepared->afterBytes(), after);
    EXPECT_EQ(receipt.beforeSha256, sha256Hex(before));
    EXPECT_EQ(receipt.afterSha256, sha256Hex(after));
    EXPECT_EQ(fs.readFile(target), before);
    EXPECT_EQ(fs.readFile(receipt.stagePath), after);
    EXPECT_EQ(prepared->revalidate(), FileImageCheck::Matches);
    EXPECT_EQ(prepared->discard(), FileImageDiscard::Removed);
    EXPECT_EQ(prepared->discard(), FileImageDiscard::AlreadyAbsent);
    EXPECT_FALSE(fs.status(receipt.stagePath));
    EXPECT_EQ(fs.readFile(target), before);
}

TEST_F(FileTransactionPrepare, ExistingEmptyFileAndEmptyAfterAreNotAbsence) {
    ASSERT_TRUE(fs.writeFileAtomic(target, ""));
    auto prepared = prepareFileImages(target, store, "", 0, 0);
    ASSERT_TRUE(prepared) << prepared.error().describe();
    EXPECT_EQ(prepared->beforeBytes(), "");
    EXPECT_TRUE(fs.status(prepared->receipt()->stagePath));
    EXPECT_EQ(prepared->revalidate(), FileImageCheck::Matches);
    EXPECT_EQ(prepared->discard(), FileImageDiscard::Removed);
    EXPECT_TRUE(fs.status(target));
}

TEST_F(FileTransactionPrepare, LimitsRefuseBeforeAnyPrivateStageOrTargetWrite) {
    const auto original = fs.readFile(target);
    auto beforeLimit = prepareFileImages(target, store, "after", 1, 5);
    ASSERT_FALSE(beforeLimit);
    EXPECT_EQ(beforeLimit.error().code(), ErrorCode::OutOfRange);
    auto afterLimit = prepareFileImages(target, store, "after", 100, 4);
    ASSERT_FALSE(afterLimit);
    EXPECT_EQ(afterLimit.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(fs.readFile(target), original);
    auto entries = fs.listDirectory(store);
    ASSERT_TRUE(entries);
    EXPECT_TRUE(entries->empty());
}

TEST_F(FileTransactionPrepare, TargetInterferenceCannotReviseTheCapturedBefore) {
    auto prepared = prepareFileImages(target, store, "after", 100, 100);
    ASSERT_TRUE(prepared) << prepared.error().describe();
    auto externalEdit = replaceForInterference(target, "external edit");
    ASSERT_TRUE(externalEdit) << externalEdit.error().describe();
    EXPECT_EQ(prepared->revalidate(), FileImageCheck::TargetChanged);
    EXPECT_EQ(prepared->beforeBytes(), "before\r\n");
    EXPECT_EQ(prepared->discard(), FileImageDiscard::Removed);
    EXPECT_EQ(fs.readFile(target), "external edit");
}

TEST_F(FileTransactionPrepare, ForeignStageOccupantIsRetainedByDiscard) {
    auto prepared = prepareFileImages(target, store, "after", 100, 100);
    ASSERT_TRUE(prepared) << prepared.error().describe();
    const auto stage = prepared->receipt()->stagePath;
    auto foreignOccupant = replaceForInterference(stage, "foreign occupant");
    ASSERT_TRUE(foreignOccupant) << foreignOccupant.error().describe();
    EXPECT_EQ(prepared->revalidate(), FileImageCheck::StageChanged);
    EXPECT_EQ(prepared->discard(), FileImageDiscard::CleanupPending);
    EXPECT_EQ(fs.readFile(stage), "foreign occupant");
    EXPECT_EQ(fs.readFile(target), "before\r\n");
}

TEST_F(FileTransactionPrepare, InPlaceStageByteTamperIsNotMistakenForAnOwnedUnchangedImage) {
    auto prepared = prepareFileImages(target, store, "after", 100, 100);
    ASSERT_TRUE(prepared) << prepared.error().describe();
    const auto stage = prepared->receipt()->stagePath;
    auto inPlace = overwriteForInterference(stage, "other");
    ASSERT_TRUE(inPlace) << inPlace.error().describe();
    EXPECT_EQ(prepared->revalidate(), FileImageCheck::StageChanged);
    EXPECT_EQ(prepared->discard(), FileImageDiscard::CleanupPending);
    EXPECT_EQ(fs.readFile(stage), "other");
}

TEST_F(FileTransactionPrepare, MoveTransfersOwnershipAndAbandonmentRetainsTheReceiptArtifact) {
    static_assert(!std::is_copy_constructible_v<PreparedFileImages>);
    static_assert(std::is_nothrow_move_constructible_v<PreparedFileImages>);
    std::string stage;
    {
        auto prepared = prepareFileImages(target, store, "after", 100, 100);
        ASSERT_TRUE(prepared) << prepared.error().describe();
        stage = prepared->receipt()->stagePath;
        PreparedFileImages moved(std::move(*prepared));
        EXPECT_EQ(prepared->receipt(), nullptr);
        auto invalid = prepared->discard();
        ASSERT_FALSE(invalid);
        EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(moved.revalidate(), FileImageCheck::Matches);
    }
    EXPECT_EQ(fs.readFile(stage), "after"); // Destructor closes resources; it does not silently discard.
    EXPECT_EQ(fs.readFile(target), "before\r\n");
}

TEST_F(FileTransactionPrepare, MissingStageReportsActualAbsenceWithoutDeletingTarget) {
    auto prepared = prepareFileImages(target, store, "after", 100, 100);
    ASSERT_TRUE(prepared) << prepared.error().describe();
    ASSERT_TRUE(fs.removeFile(prepared->receipt()->stagePath));
    EXPECT_EQ(prepared->revalidate(), FileImageCheck::StageChanged);
    EXPECT_EQ(prepared->discard(), FileImageDiscard::AlreadyAbsent);
    EXPECT_EQ(fs.readFile(target), "before\r\n");
}

TEST_F(FileTransactionPrepare, RenamedAwayStageRemainsOwnedAndCannotBeReportedAsDiscarded) {
    auto prepared = prepareFileImages(target, store, "after", 100, 100);
    ASSERT_TRUE(prepared) << prepared.error().describe();
    const auto stage = prepared->receipt()->stagePath;
    const auto moved = scratch.join("retained-renamed-stage");
    auto oldNative = toStdPath(stage);
    auto newNative = toStdPath(moved);
    ASSERT_TRUE(oldNative);
    ASSERT_TRUE(newNative);
    std::error_code error;
    std::filesystem::rename(*oldNative, *newNative, error);
    ASSERT_FALSE(error) << error.message();
    EXPECT_EQ(prepared->revalidate(), FileImageCheck::StageChanged);
    EXPECT_EQ(prepared->discard(), FileImageDiscard::CleanupPending);
    EXPECT_EQ(fs.readFile(moved), "after");
}

TEST_F(FileTransactionPrepare, DirectoryTargetIsNotFabricatedAsAFileImage) {
    auto prepared = prepareFileImages(store, store, "after", 100, 100);
    ASSERT_FALSE(prepared);
    EXPECT_EQ(prepared.error().code(), ErrorCode::Unsupported);
    auto entries = fs.listDirectory(store);
    ASSERT_TRUE(entries);
    EXPECT_TRUE(entries->empty());
}

TEST_F(FileTransactionPrepare, MultipleTargetLinksRefuseBeforeCreatingAStage) {
    auto source = toStdPath(target);
    auto alias = toStdPath(scratch.join("target-hardlink"));
    ASSERT_TRUE(source);
    ASSERT_TRUE(alias);
    std::error_code error;
    std::filesystem::create_hard_link(*source, *alias, error);
    ASSERT_FALSE(error) << error.message();
    auto prepared = prepareFileImages(target, store, "after", 100, 100);
    ASSERT_FALSE(prepared);
    EXPECT_EQ(prepared.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(fs.readFile(target), "before\r\n");
    auto entries = fs.listDirectory(store);
    ASSERT_TRUE(entries);
    EXPECT_TRUE(entries->empty());
}

#ifndef _WIN32
TEST_F(FileTransactionPrepare, SharedPosixStoreIsRefusedBeforeCreatingAStage) {
    ASSERT_EQ(::chmod(store.c_str(), 0755), 0);
    auto prepared = prepareFileImages(target, store, "after", 100, 100);
    ASSERT_FALSE(prepared);
    EXPECT_EQ(prepared.error().code(), ErrorCode::InvalidArgument);
    auto entries = fs.listDirectory(store);
    ASSERT_TRUE(entries);
    EXPECT_TRUE(entries->empty());
}

TEST_F(FileTransactionPrepare, LosingPrivateStorePermissionsRetainsStageUntilOwnershipIsReadmitted) {
    auto prepared = prepareFileImages(target, store, "after", 100, 100);
    ASSERT_TRUE(prepared) << prepared.error().describe();
    ASSERT_EQ(::chmod(store.c_str(), 0755), 0);
    EXPECT_EQ(prepared->revalidate(), FileImageCheck::StageChanged);
    EXPECT_EQ(prepared->discard(), FileImageDiscard::CleanupPending);
    ASSERT_EQ(::chmod(store.c_str(), 0700), 0);
    EXPECT_EQ(prepared->discard(), FileImageDiscard::Removed);
}
#endif
} // namespace
