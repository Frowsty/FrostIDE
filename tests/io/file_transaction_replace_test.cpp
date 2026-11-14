// SPDX-License-Identifier: Apache-2.0
// Real replacement/inverse primitives and foreign/tampered/partial-effect retention, not global Undo.
#include "frost/io/file_system.hpp"
#include "frost/io/file_transaction_replace.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"

#include "file_transaction_replace_testing.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#else
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using namespace frost::io;

frost::Result<void> privateDirectory(std::string_view path, bool inherit = true) {
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return std::unexpected(lastWin32Error("test: open current token", path));
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        const auto error = lastWin32Error("test: size current token", path);
        CloseHandle(token);
        return std::unexpected(error);
    }
    std::vector<unsigned char> bytes(size);
    if (!GetTokenInformation(token, TokenUser, bytes.data(), size, &size)) {
        const auto error = lastWin32Error("test: current token", path);
        CloseHandle(token);
        return std::unexpected(error);
    }
    CloseHandle(token);
    wchar_t* sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid))
        return std::unexpected(lastWin32Error("test: current SID", path));
    const std::wstring sddl = L"D:P(A;" + std::wstring(inherit ? L"OICI" : L"") + L";FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return std::unexpected(lastWin32Error("test: private DACL", path));
    BOOL present = FALSE, defaulted = FALSE;
    PACL acl = nullptr;
    if (!GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) || !present) {
        const auto error = lastWin32Error("test: decode private DACL", path);
        LocalFree(descriptor);
        return std::unexpected(error);
    }
    auto wide = toWin32Path(path);
    if (!wide) {
        LocalFree(descriptor);
        return std::unexpected(wide.error());
    }
    const auto error = SetNamedSecurityInfoW(wide->data(), SE_FILE_OBJECT,
                                             DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                             nullptr, acl, nullptr);
    LocalFree(descriptor);
    if (error != ERROR_SUCCESS)
        return std::unexpected(win32Error(error, "test: restrict private directory", path));
#else
    static_cast<void>(inherit);
    if (::chmod(std::string(path).c_str(), 0700) != 0)
        return std::unexpected(errnoError(errno, "test: private directory", path));
#endif
    return {};
}

frost::Result<void> overwriteNative(std::string_view path, std::string_view bytes) {
#ifdef _WIN32
    auto wide = toWin32Path(path);
    if (!wide)
        return std::unexpected(wide.error());
    HANDLE file = CreateFileW(wide->c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return std::unexpected(lastWin32Error("test: full-share native overwrite", path));
    DWORD count = 0;
    if (!SetEndOfFile(file) || !WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) ||
        count != bytes.size() || !FlushFileBuffers(file)) {
        const auto error = lastWin32Error("test: native overwrite", path);
        CloseHandle(file);
        return std::unexpected(error);
    }
    CloseHandle(file);
#else
    const int file = ::open(std::string(path).c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (file < 0)
        return std::unexpected(errnoError(errno, "test: native overwrite", path));
    const auto written = ::write(file, bytes.data(), bytes.size());
    const int flushed = ::fsync(file);
    const int error = errno;
    ::close(file);
    if (written < 0 || static_cast<std::size_t>(written) != bytes.size() || flushed != 0)
        return std::unexpected(errnoError(error, "test: native overwrite", path));
#endif
    return {};
}

frost::Result<std::string> supportedSnapshot(std::string_view path) {
#ifdef _WIN32
    auto wide = toWin32Path(path);
    if (!wide)
        return std::unexpected(wide.error());
    HANDLE file = CreateFileW(wide->c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return std::unexpected(lastWin32Error("test: open metadata oracle", path));
    FILE_ID_INFO id{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) ||
        !GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))) {
        auto error = lastWin32Error("test: actual metadata oracle", path);
        CloseHandle(file);
        return std::unexpected(error);
    }
    CloseHandle(file);
    DWORD count = 0;
    constexpr DWORD fields = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    GetFileSecurityW(wide->c_str(), fields, nullptr, 0, &count);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || count == 0)
        return std::unexpected(lastWin32Error("test: size independent security oracle", path));
    std::string security(count, '\0');
    if (!GetFileSecurityW(wide->c_str(), fields, security.data(), count, &count))
        return std::unexpected(lastWin32Error("test: independent security oracle", path));
    return std::format("{}:{}:{}:{}:", id.VolumeSerialNumber, basic.FileAttributes, basic.CreationTime.QuadPart,
                       basic.LastWriteTime.QuadPart) +
           std::string(reinterpret_cast<const char*>(id.FileId.Identifier), sizeof(id.FileId.Identifier)) + security;
#else
    struct stat info{};
    if (::stat(std::string(path).c_str(), &info) != 0)
        return std::unexpected(errnoError(errno, "test: actual inode/metadata oracle", path));
    return std::format("{}:{}:{}:{}:{}:{}:{}", info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_gid,
                       info.st_mtim.tv_sec, info.st_mtim.tv_nsec);
#endif
}

class FileTransactionReplace : public ::testing::Test {
protected:
    frost::test::ScratchDir scratch{"native-replace"};
    IFileSystem& fs = realFileSystem();
    std::string target = scratch.join("a.cpp");
    std::string store = scratch.join("private");
    const std::string before = std::string("\xEF\xBB\xBF", 3) + "Before\r\n" + std::string(1, '\0');
    const std::string after = "After\n";
    void SetUp() override {
        ASSERT_TRUE(fs.createDirectories(store));
        auto restricted = privateDirectory(store);
        ASSERT_TRUE(restricted) << restricted.error().describe();
        ASSERT_TRUE(fs.writeFileAtomic(target, before));
    }
    void TearDown() override { detail::setReplacementObserver(nullptr, nullptr); }
    frost::Result<PreparedFileReplacement> prepare() {
        return prepareFileReplacement(target, store, after, 65536, 65536);
    }
    void expectBytes(std::string_view path, std::string_view expected) {
        auto bytes = fs.readFile(path);
        ASSERT_TRUE(bytes) << bytes.error().describe();
        EXPECT_EQ(*bytes, expected);
    }
    struct Fault {
        detail::ReplacementBoundary boundary;
        bool fired = false;
        bool wrote = false;
    };
    static void tamperAtBoundary(void* context, detail::ReplacementBoundary boundary,
                                 const FileReplacementReceipt& receipt) {
        auto& fault = *static_cast<Fault*>(context);
        if (boundary != fault.boundary)
            return;
        fault.fired = true;
        const auto& path = boundary == detail::ReplacementBoundary::OriginalRetired ? receipt.retainedBeforePath
                           : boundary == detail::ReplacementBoundary::AfterRetired  ? receipt.replacementPath
                                                                                    : receipt.images.targetPath;
        fault.wrote = overwriteNative(path, "boundary-tamper").has_value();
    }
    void postEffectFailure(detail::ReplacementBoundary boundary, FileReplacementPhase expected) {
        auto value = prepare();
        ASSERT_TRUE(value) << value.error().describe();
        if (boundary != detail::ReplacementBoundary::OriginalRetired) {
            auto retired = value->retireOriginal();
            ASSERT_TRUE(retired.complete()) << retired.error->describe();
        }
        if (boundary == detail::ReplacementBoundary::AfterRetired ||
            boundary == detail::ReplacementBoundary::BeforeRestored) {
            auto installed = value->installAfter();
            ASSERT_TRUE(installed.complete()) << installed.error->describe();
        }
        Fault fault{boundary};
        detail::setReplacementObserver(tamperAtBoundary, &fault);
        auto outcome = boundary == detail::ReplacementBoundary::OriginalRetired  ? value->retireOriginal()
                       : boundary == detail::ReplacementBoundary::AfterInstalled ? value->installAfter()
                                                                                 : value->restoreBefore();
        detail::setReplacementObserver(nullptr, nullptr);
        ASSERT_TRUE(fault.fired);
        ASSERT_TRUE(fault.wrote);
        ASSERT_TRUE(outcome.error.has_value());
        EXPECT_EQ(outcome.receipt.phase, expected);
        EXPECT_EQ(value->receipt()->phase, expected);
        const auto& changed = boundary == detail::ReplacementBoundary::OriginalRetired
                                  ? outcome.receipt.retainedBeforePath
                              : boundary == detail::ReplacementBoundary::AfterRetired ? outcome.receipt.replacementPath
                                                                                      : target;
        expectBytes(changed, "boundary-tamper");
    }
};

TEST_F(FileTransactionReplace, PositiveReplacementAndInverseRestoreExactBinaryBefore) {
    const auto nativeBefore = supportedSnapshot(target);
    ASSERT_TRUE(nativeBefore) << nativeBefore.error().describe();
    auto value = prepare();
    ASSERT_TRUE(value) << value.error().describe();
    const auto receipt = *value->receipt();
    expectBytes(target, before);
    expectBytes(receipt.replacementPath, after);
    auto retired = value->retireOriginal();
    ASSERT_TRUE(retired.complete()) << retired.error->describe();
    EXPECT_EQ(retired.receipt.phase, FileReplacementPhase::OriginalRetained);
    ASSERT_EQ(fs.exists(target), frost::Result<bool>(false));
    expectBytes(retired.receipt.retainedBeforePath, before);
    auto installed = value->installAfter();
    ASSERT_TRUE(installed.complete()) << installed.error->describe();
    EXPECT_EQ(installed.receipt.phase, FileReplacementPhase::AfterInstalled);
    expectBytes(target, after);
    auto restored = value->restoreBefore();
    ASSERT_TRUE(restored.complete()) << restored.error->describe();
    EXPECT_EQ(restored.receipt.phase, FileReplacementPhase::BeforeRestored);
    expectBytes(target, before);
    const auto nativeRestored = supportedSnapshot(target);
    ASSERT_TRUE(nativeRestored) << nativeRestored.error().describe();
    EXPECT_EQ(*nativeBefore, *nativeRestored); // Actual original object and supported metadata, not a reencoded copy.
    expectBytes(restored.receipt.replacementPath, after);
    EXPECT_EQ(restored.receipt.images.beforeSha256, receipt.images.beforeSha256);
    EXPECT_FALSE(receipt.beforeMetadataSha256.empty());
    EXPECT_FALSE(receipt.privateAfterMetadataSha256.empty());
}

TEST_F(FileTransactionReplace, EmptyImagesAreActualExistingFiles) {
    ASSERT_TRUE(fs.writeFileAtomic(target, ""));
    auto value = prepareFileReplacement(target, store, "", 0, 0);
    ASSERT_TRUE(value) << value.error().describe();
    ASSERT_TRUE(value->retireOriginal().complete());
    ASSERT_TRUE(value->installAfter().complete());
    ASSERT_TRUE(value->restoreBefore().complete());
    expectBytes(target, "");
}

TEST_F(FileTransactionReplace, ChangedTargetOrPrivateStageRefusesBeforeRetirement) {
    auto value = prepare();
    ASSERT_TRUE(value) << value.error().describe();
    ASSERT_TRUE(overwriteNative(target, "foreign target"));
    auto result = value->retireOriginal();
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.receipt.phase, FileReplacementPhase::Prepared);
    expectBytes(target, "foreign target");
    expectBytes(result.receipt.replacementPath, after);
}

TEST_F(FileTransactionReplace, StageByteTamperRefusesBeforeRetirement) {
    auto value = prepare();
    ASSERT_TRUE(value) << value.error().describe();
    ASSERT_TRUE(overwriteNative(value->receipt()->replacementPath, "tampered after"));
    auto result = value->retireOriginal();
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.receipt.phase, FileReplacementPhase::Prepared);
    expectBytes(target, before);
    expectBytes(result.receipt.replacementPath, "tampered after");
}

TEST_F(FileTransactionReplace, ForeignStageNameRetainsForeignAndRenamedOwnedImage) {
    auto value = prepare();
    ASSERT_TRUE(value) << value.error().describe();
    const auto originalStage = value->receipt()->replacementPath;
    const auto movedStage = scratch.join("moved-after");
    ASSERT_TRUE(fs.rename(originalStage, movedStage));
    ASSERT_TRUE(fs.writeFileAtomic(originalStage, "foreign stage"));
    auto outcome = value->retireOriginal();
    ASSERT_TRUE(outcome.error);
    EXPECT_EQ(outcome.receipt.phase, FileReplacementPhase::Prepared);
    expectBytes(target, before);
    expectBytes(originalStage, "foreign stage");
    expectBytes(movedStage, after);
}

TEST_F(FileTransactionReplace, MetadataChangeDuringExplicitPreparationIsNotSilentlyRecaptured) {
    struct Actor {
        bool changed = false;
    } actor;
    detail::setReplacementObserver(
        [](void* context, detail::ReplacementBoundary boundary, const FileReplacementReceipt& receipt) {
            if (boundary != detail::ReplacementBoundary::MetadataCaptured)
                return;
            auto& changed = static_cast<Actor*>(context)->changed;
#ifdef _WIN32
            auto wide = toWin32Path(receipt.images.targetPath);
            if (!wide)
                return;
            const auto attributes = GetFileAttributesW(wide->c_str());
            changed = attributes != INVALID_FILE_ATTRIBUTES &&
                      SetFileAttributesW(wide->c_str(), attributes ^ FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
#else
            struct stat metadata{};
            changed = ::stat(receipt.images.targetPath.c_str(), &metadata) == 0 &&
                      ::chmod(receipt.images.targetPath.c_str(), (metadata.st_mode & 0777) ^ S_IXUSR) == 0;
#endif
        },
        &actor);
    auto outcome = prepare();
    detail::setReplacementObserver(nullptr, nullptr);
    ASSERT_TRUE(actor.changed);
    ASSERT_FALSE(outcome);
    EXPECT_EQ(outcome.error().code(), frost::ErrorCode::Busy);
    expectBytes(target, before);
}

TEST_F(FileTransactionReplace, NameSwappedByteStageIsNotAdoptedOrMetadataMutated) {
    struct Actor {
        IFileSystem* fs;
        std::string moved;
        std::string foreignSnapshot;
        std::string stage;
        bool changed = false;
    } actor{&fs, scratch.join("displaced-stage"), {}, {}, false};
    detail::setReplacementObserver(
        [](void* context, detail::ReplacementBoundary boundary, const FileReplacementReceipt& receipt) {
            if (boundary != detail::ReplacementBoundary::ByteImagesPrepared)
                return;
            auto& state = *static_cast<Actor*>(context);
            state.stage = receipt.replacementPath;
            if (!state.fs->rename(state.stage, state.moved) || !state.fs->writeFileAtomic(state.stage, "foreign-stage"))
                return;
#ifndef _WIN32
            if (::chmod(state.stage.c_str(), 0640) != 0)
                return;
#endif
            auto snapshot = supportedSnapshot(state.stage);
            if (!snapshot)
                return;
            state.foreignSnapshot = std::move(*snapshot);
            state.changed = true;
        },
        &actor);
    auto refused = prepare();
    detail::setReplacementObserver(nullptr, nullptr);
    ASSERT_TRUE(actor.changed);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Busy);
    auto actual = supportedSnapshot(actor.stage);
    ASSERT_TRUE(actual) << actual.error().describe();
    EXPECT_EQ(*actual, actor.foreignSnapshot); // No chmod/basic/DACL setters touched foreign stage.
    expectBytes(actor.stage, "foreign-stage");
    expectBytes(actor.moved, after);
    expectBytes(target, before);
}

TEST_F(FileTransactionReplace, MovedPublishedImageIsNotGivenWorkspacePermissions) {
    auto value = prepare();
    ASSERT_TRUE(value) << value.error().describe();
    ASSERT_TRUE(value->retireOriginal().complete());
    struct Actor {
        IFileSystem* fs;
        bool moved = false;
        std::string snapshot;
    } actor{&fs, false, {}};
    detail::setReplacementObserver(
        [](void* context, detail::ReplacementBoundary boundary, const FileReplacementReceipt& receipt) {
            if (boundary != detail::ReplacementBoundary::AfterInstalled)
                return;
            auto& state = *static_cast<Actor*>(context);
            if (!state.fs->rename(receipt.images.targetPath, receipt.replacementPath) ||
                !state.fs->writeFileAtomic(receipt.images.targetPath, "foreign endpoint"))
                return;
            auto snapshot = supportedSnapshot(receipt.replacementPath);
            if (!snapshot)
                return;
            state.snapshot = std::move(*snapshot);
            state.moved = true;
        },
        &actor);
    auto result = value->installAfter();
    detail::setReplacementObserver(nullptr, nullptr);
    ASSERT_TRUE(actor.moved);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.receipt.phase, FileReplacementPhase::AfterInstalledUnverified);
    auto actual = supportedSnapshot(result.receipt.replacementPath);
    ASSERT_TRUE(actual) << actual.error().describe();
    EXPECT_EQ(*actual, actor.snapshot); // In particular, private DACL did not broaden after movement.
    expectBytes(result.receipt.replacementPath, after);
    expectBytes(target, "foreign endpoint");
    expectBytes(result.receipt.retainedBeforePath, before);
}

TEST_F(FileTransactionReplace, ForeignInstallEndpointIsNeverReplaced) {
    auto value = prepare();
    ASSERT_TRUE(value) << value.error().describe();
    ASSERT_TRUE(value->retireOriginal().complete());
    ASSERT_TRUE(fs.writeFileAtomic(target, "foreign install"));
    auto result = value->installAfter();
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.receipt.phase, FileReplacementPhase::OriginalRetained);
    expectBytes(target, "foreign install");
    expectBytes(result.receipt.retainedBeforePath, before);
    expectBytes(result.receipt.replacementPath, after);
}

TEST_F(FileTransactionReplace, RetainedBeforeTamperCannotBecomeInverseAuthority) {
    auto value = prepare();
    ASSERT_TRUE(value) << value.error().describe();
    ASSERT_TRUE(value->retireOriginal().complete());
    ASSERT_TRUE(value->installAfter().complete());
    ASSERT_TRUE(overwriteNative(value->receipt()->retainedBeforePath, "changed retained Before"));
    auto result = value->restoreBefore();
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.receipt.phase, FileReplacementPhase::AfterInstalled);
    expectBytes(target, after);
    expectBytes(result.receipt.retainedBeforePath, "changed retained Before");
}

TEST_F(FileTransactionReplace, ForeignUndoEndpointRetainsBothImagesAndCanBeResolvedExplicitly) {
    auto value = prepare();
    ASSERT_TRUE(value) << value.error().describe();
    ASSERT_TRUE(value->retireOriginal().complete());
    ASSERT_TRUE(value->installAfter().complete());
    struct Occupy {
        IFileSystem* fs;
        bool created = false;
    } actor{&fs};
    detail::setReplacementObserver(
        [](void* context, detail::ReplacementBoundary boundary, const FileReplacementReceipt& receipt) {
            if (boundary != detail::ReplacementBoundary::AfterRetired)
                return;
            auto& state = *static_cast<Occupy*>(context);
            state.created = state.fs->writeFileAtomic(receipt.images.targetPath, "foreign inverse").has_value();
        },
        &actor);
    auto result = value->restoreBefore();
    detail::setReplacementObserver(nullptr, nullptr);
    ASSERT_TRUE(actor.created);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.receipt.phase, FileReplacementPhase::AfterRetained);
    expectBytes(target, "foreign inverse");
    expectBytes(result.receipt.replacementPath, after);
    expectBytes(result.receipt.retainedBeforePath, before);
    // The owner explicitly moves its foreign fixture away; the participant never overwrites it.
    ASSERT_TRUE(fs.rename(target, scratch.join("saved-foreign")));
    auto restored = value->restoreBefore();
    ASSERT_TRUE(restored.complete()) << restored.error->describe();
    expectBytes(target, before);
    expectBytes(scratch.join("saved-foreign"), "foreign inverse");
}

TEST_F(FileTransactionReplace, RetirementPostcheckFailureReportsActualEffect) {
    postEffectFailure(detail::ReplacementBoundary::OriginalRetired, FileReplacementPhase::OriginalRetiredUnverified);
}
TEST_F(FileTransactionReplace, InstallPostcheckFailureReportsActualEffect) {
    postEffectFailure(detail::ReplacementBoundary::AfterInstalled, FileReplacementPhase::AfterInstalledUnverified);
}
TEST_F(FileTransactionReplace, InverseRetirementPostcheckFailureReportsActualEffect) {
    postEffectFailure(detail::ReplacementBoundary::AfterRetired, FileReplacementPhase::AfterRetiredUnverified);
}
TEST_F(FileTransactionReplace, InversePublicationPostcheckFailureReportsActualEffect) {
    postEffectFailure(detail::ReplacementBoundary::BeforeRestored, FileReplacementPhase::BeforeRestoredUnverified);
}

TEST_F(FileTransactionReplace, MoveAndDestructionRetainRecoverableObjectsWithoutRollback) {
    FileReplacementReceipt receipt;
    {
        auto value = prepare();
        ASSERT_TRUE(value) << value.error().describe();
        auto moved = std::move(*value);
        EXPECT_EQ(value->receipt(), nullptr);
        auto retired = moved.retireOriginal();
        ASSERT_TRUE(retired.complete()) << retired.error->describe();
        receipt = retired.receipt;
    }
    expectBytes(receipt.retainedBeforePath, before);
    expectBytes(receipt.replacementPath, after);
    ASSERT_EQ(fs.exists(target), frost::Result<bool>(false));
}

TEST_F(FileTransactionReplace, TwoParticipantsRetainIndependentProgressWithoutGlobalSuccess) {
    auto first = prepare();
    ASSERT_TRUE(first) << first.error().describe();
    const auto other = scratch.join("b.cpp");
    ASSERT_TRUE(fs.writeFileAtomic(other, "second before"));
    auto second = prepareFileReplacement(other, store, "second after", 1024, 1024);
    ASSERT_TRUE(second) << second.error().describe();
    ASSERT_TRUE(first->retireOriginal().complete());
    ASSERT_TRUE(first->installAfter().complete());
    ASSERT_TRUE(second->retireOriginal().complete());
    ASSERT_TRUE(fs.writeFileAtomic(other, "foreign second"));
    auto failed = second->installAfter();
    ASSERT_TRUE(failed.error);
    EXPECT_EQ(failed.receipt.phase, FileReplacementPhase::OriginalRetained);
    expectBytes(target, after);
    expectBytes(other, "foreign second");
    expectBytes(failed.receipt.retainedBeforePath, "second before");
    ASSERT_TRUE(first->restoreBefore().complete());
    expectBytes(target, before);
}

TEST_F(FileTransactionReplace, LimitsAndFailedNativeInspectionDoNotMeanMissingMetadata) {
    auto oversized = prepareFileReplacement(target, store, after, 65536, 0);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code(), frost::ErrorCode::OutOfRange);
    auto missing = prepareFileReplacement(scratch.join("missing"), store, after, 65536, 65536);
    ASSERT_FALSE(missing);
    expectBytes(target, before);
}

#ifdef _WIN32
TEST_F(FileTransactionReplace, NonInheritablePrivateStoreRefusesBeforeWritingAnyAfterBytes) {
    auto restricted = privateDirectory(store, false);
    ASSERT_TRUE(restricted) << restricted.error().describe();
    auto refused = prepare();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Unsupported);
    auto entries = fs.listDirectory(store);
    ASSERT_TRUE(entries) << entries.error().describe();
    EXPECT_TRUE(entries->empty());
    expectBytes(target, before);
}

TEST_F(FileTransactionReplace, ObservedAlternateStreamRefusesWithoutPretendingItIsAbsent) {
    auto wide = toWin32Path(target + ":extra");
    ASSERT_TRUE(wide);
    HANDLE stream = CreateFileW(wide->c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(stream, INVALID_HANDLE_VALUE);
    DWORD written = 0;
    const bool success = WriteFile(stream, "secret", 6, &written, nullptr) && written == 6;
    CloseHandle(stream);
    ASSERT_TRUE(success);
    auto refused = prepare();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Unsupported);
    expectBytes(target, before);
}

TEST_F(FileTransactionReplace, LimitedSecurityProfileDoesNotClaimUnobservedSaclPreservation) {
    auto value = prepare();
    ASSERT_TRUE(value) << value.error().describe();
    EXPECT_NE(value->receipt()->metadataProfile.find("SACL unobserved/unproven"), std::string::npos);
    EXPECT_NE(value->receipt()->metadataProfile.find("not metadata-complete/Apply-ready"), std::string::npos);
}
#else
TEST_F(FileTransactionReplace, FifoTargetIsOpenedNonblockingAndRefusedBeforeRead) {
    const auto fifo = scratch.join("fifo");
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    auto refused = prepareFileReplacement(fifo, store, after, 65536, 65536);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Unsupported);
    auto entries = fs.listDirectory(store);
    ASSERT_TRUE(entries) << entries.error().describe();
    EXPECT_TRUE(entries->empty());
    expectBytes(target, before);
}

TEST_F(FileTransactionReplace, ObservedXattrRefusesAndRetainsOriginalBytes) {
    ASSERT_EQ(::setxattr(target.c_str(), "user.frost-test", "owned", 5, 0), 0);
    auto refused = prepare();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Unsupported);
    expectBytes(target, before);
}

TEST_F(FileTransactionReplace, ObservedSpecialInodeFlagsRefuseBeforeAnyEffects) {
    const int file = ::open(target.c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_GE(file, 0);
    int flags = 0;
    const bool queried = ::ioctl(file, FS_IOC_GETFLAGS, &flags) == 0;
    ASSERT_TRUE(queried);
    int special = flags | FS_NODUMP_FL;
    const bool set = ::ioctl(file, FS_IOC_SETFLAGS, &special) == 0;
    ::close(file);
    ASSERT_TRUE(set);
    auto refused = prepare();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Unsupported);
    expectBytes(target, before);
}
#endif

TEST_F(FileTransactionReplace, ReversibleMoveAndRedoPreserveExactSourceAndForeignDestination) {
    auto value = prepare();
    ASSERT_TRUE(value) << value.error().describe();
    const auto destination = scratch.join("b.cpp");
    ASSERT_TRUE(value->prepareMoveDestination(destination));
    auto moved = value->moveOriginalTo(destination);
    ASSERT_TRUE(moved.complete()) << moved.error->describe();
    EXPECT_EQ(moved.receipt.phase, FileReplacementPhase::OriginalMoved);
    expectBytes(destination, before);
    ASSERT_TRUE(value->validateCurrent(true));
    auto restored = value->restoreMove();
    ASSERT_TRUE(restored.complete()) << restored.error->describe();
    EXPECT_EQ(restored.receipt.phase, FileReplacementPhase::MoveRestored);
    expectBytes(target, before);
    ASSERT_TRUE(value->reapplyMove().complete());
    expectBytes(destination, before);
}
TEST_F(FileTransactionReplace, PrivateStoreProvisioningReusesOnlyRestrictedOwnedDirectories) {
    const auto requested = scratch.join("new-private");
    auto created = ensurePrivateTransactionStore(target, requested, scratch.path());
    ASSERT_TRUE(created) << created.error().describe();
    auto reused = ensurePrivateTransactionStore(target, requested, scratch.path());
    ASSERT_TRUE(reused) << reused.error().describe();
    EXPECT_EQ(*created, *reused);
    auto first = acquireTransactionStoreLease(*created);
    ASSERT_TRUE(first) << first.error().describe();
    EXPECT_FALSE(acquireTransactionStoreLease(*created));
}
TEST_F(FileTransactionReplace, FreshRecoveryReacquiresOwnedObjectsWithoutTrustingClaimedPhase) {
    FileReplacementReceipt saved;
    {
        auto value = prepare();
        ASSERT_TRUE(value) << value.error().describe();
        saved = *value->receipt(); // Initial immutable receipt, before either effect.
        ASSERT_TRUE(value->retireOriginal().complete());
        ASSERT_TRUE(value->installAfter().complete());
    }
    saved.phase = FileReplacementPhase::Prepared; // Recovery must observe actual native state.
    auto recovered = recoverFileReplacement(saved, before, after, true, 65536);
    ASSERT_TRUE(recovered) << recovered.error().describe();
    EXPECT_EQ(recovered->receipt()->phase, FileReplacementPhase::AfterInstalled);
    auto undone = recovered->restoreBefore();
    ASSERT_TRUE(undone.complete()) << undone.error->describe();
    expectBytes(target, before);
}
TEST_F(FileTransactionReplace, RecoveryDoesNotAdoptTamperedRetainedBefore) {
    FileReplacementReceipt saved;
    {
        auto value = prepare();
        ASSERT_TRUE(value) << value.error().describe();
        saved = *value->receipt();
        ASSERT_TRUE(value->retireOriginal().complete());
        ASSERT_TRUE(value->installAfter().complete());
    }
    ASSERT_TRUE(overwriteNative(saved.retainedBeforePath, "foreign retained bytes"));
    EXPECT_FALSE(recoverFileReplacement(saved, before, after, true, 65536));
    expectBytes(target, after);
    expectBytes(saved.retainedBeforePath, "foreign retained bytes");
}
} // namespace
