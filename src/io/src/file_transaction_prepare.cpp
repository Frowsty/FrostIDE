// SPDX-License-Identifier: Apache-2.0
// Preparation-only native byte capture/staging; never publishes a workspace replacement.
#include "frost/io/file_transaction_prepare.hpp"

#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"

#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <utility>

#ifdef _WIN32
#include "save_metadata_win.hpp"
#else
#include <sys/stat.h>

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace frost::io {
namespace {
#ifdef _WIN32
using Native = detail::UniqueHandle;
#else
using Native = detail::UniqueFd;
#endif
using Observation = FileImageObservation;

Result<Native> openNamed(std::string_view path, bool directory) {
#ifdef _WIN32
    static_cast<void>(directory);
    auto wide = toWin32Path(path);
    if (!wide)
        return std::unexpected(wide.error());
    Native handle(CreateFileW(wide->c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                              nullptr));
    if (!handle.valid())
        return std::unexpected(lastWin32Error("prepareFileImages: open", path));
#else
    Native handle(::open(std::string(path).c_str(),
                         O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (directory ? O_DIRECTORY : 0)));
    if (!handle.valid())
        return std::unexpected(errnoError(errno, "prepareFileImages: open", path));
#endif
    return handle;
}

Result<Observation> observe(const Native& handle, std::string_view path, bool directory) {
#ifdef _WIN32
    FILE_ID_INFO id{};
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(handle.get(), FileIdInfo, &id, sizeof(id)) ||
        !GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard, sizeof(standard)))
        return std::unexpected(lastWin32Error("prepareFileImages: observe", path));
    if (GetFileType(handle.get()) != FILE_TYPE_DISK || (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        static_cast<bool>(standard.Directory) != directory || (!directory && standard.NumberOfLinks != 1))
        return fail(ErrorCode::Unsupported,
                    "prepareFileImages: regular single-link file/non-reparse directory required");
    if (!directory) {
        if ((basic.FileAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0)
            return fail(ErrorCode::Unsupported, "prepareFileImages: EFS is outside the first byte-preparation profile");
        auto wide = toWin32Path(path);
        if (!wide)
            return std::unexpected(wide.error());
        save_metadata::Metadata existing;
        const auto error = save_metadata::queryMetadata(handle.get(), *wide, existing);
        if (error != ERROR_SUCCESS)
            return std::unexpected(win32Error(error, "prepareFileImages: identity metadata", path));
        if (existing.hasObjectId)
            return fail(ErrorCode::Unsupported, "prepareFileImages: NTFS object IDs are outside the first profile");
    }
    return Observation{std::to_string(id.VolumeSerialNumber),
                       std::string(reinterpret_cast<const char*>(id.FileId.Identifier), sizeof(id.FileId.Identifier)),
                       std::format("{}:{}:{}:{}:{}", basic.FileAttributes, standard.NumberOfLinks,
                                   basic.CreationTime.QuadPart, basic.LastWriteTime.QuadPart,
                                   basic.ChangeTime.QuadPart),
                       static_cast<std::uint64_t>(standard.EndOfFile.QuadPart)};
#else
    struct stat info{};
    if (::fstat(handle.get(), &info) != 0)
        return std::unexpected(errnoError(errno, "prepareFileImages: observe", path));
    if ((directory && !S_ISDIR(info.st_mode)) || (!directory && (!S_ISREG(info.st_mode) || info.st_nlink != 1)))
        return fail(ErrorCode::Unsupported, "prepareFileImages: regular single-link file/directory required");
    return Observation{std::to_string(info.st_dev), std::to_string(info.st_ino),
                       std::format("{}:{}:{}:{}:{}:{}:{}:{}", info.st_mode, info.st_uid, info.st_gid, info.st_nlink,
                                   info.st_mtim.tv_sec, info.st_mtim.tv_nsec, info.st_ctim.tv_sec,
                                   info.st_ctim.tv_nsec),
                       static_cast<std::uint64_t>(info.st_size)};
#endif
}

bool sameObject(const Observation& a, const Observation& b) { return a.volume == b.volume && a.object == b.object; }

Result<void> validatePrivateStore(const Native& store, std::string_view path) {
#ifdef _WIN32
    // Windows ACL restriction is the explicit recovery-owner contract; it is NOT observed here.
    static_cast<void>(store);
    static_cast<void>(path);
#else
    struct stat info{};
    if (::fstat(store.get(), &info) != 0)
        return std::unexpected(errnoError(errno, "prepareFileImages: private store", path));
    if (info.st_uid != ::geteuid() || (info.st_mode & 0777) != 0700)
        return fail(ErrorCode::InvalidArgument, "prepareFileImages: private store must be owner-controlled mode0700");
#endif
    return {};
}

Result<bool> stageUnlinked(const Native& stage, std::string_view path) {
#ifdef _WIN32
    FILE_STANDARD_INFO info{};
    if (!GetFileInformationByHandleEx(stage.get(), FileStandardInfo, &info, sizeof(info)))
        return std::unexpected(lastWin32Error("prepareFileImages: retained stage link state", path));
    return info.DeletePending != FALSE || info.NumberOfLinks == 0;
#else
    struct stat info{};
    if (::fstat(stage.get(), &info) != 0)
        return std::unexpected(errnoError(errno, "prepareFileImages: retained stage link state", path));
    return info.st_nlink == 0;
#endif
}

Result<std::string> readBounded(const Native& handle, std::size_t limit, std::string_view path) {
    std::string result;
    std::array<char, 64 * 1024> chunk{};
#ifdef _WIN32
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle.get(), zero, nullptr, FILE_BEGIN))
        return std::unexpected(lastWin32Error("prepareFileImages: seek", path));
#endif
    for (;;) {
        // Fixed scratch storage permits detecting growth without allocating limit+1 bytes.
        const auto want = std::min(chunk.size(), limit - result.size());
        const auto probe = want == 0 ? std::size_t{1} : want;
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(handle.get(), chunk.data(), static_cast<DWORD>(probe), &got, nullptr))
            return std::unexpected(lastWin32Error("prepareFileImages: read", path));
#else
        const auto got = ::pread(handle.get(), chunk.data(), probe, static_cast<off_t>(result.size()));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(errnoError(errno, "prepareFileImages: read", path));
        }
#endif
        if (got == 0)
            return result;
        if (static_cast<std::size_t>(got) > limit - result.size())
            return fail(ErrorCode::OutOfRange, "prepareFileImages: payload exceeds explicit byte limit");
        result.append(chunk.data(), static_cast<std::size_t>(got));
    }
}

Result<void> writeStage(const Native& stage, std::string_view bytes, std::string_view path) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto want = std::min<std::size_t>(bytes.size() - done, 64 * 1024);
#ifdef _WIN32
        DWORD count = 0;
        if (!WriteFile(stage.get(), bytes.data() + done, static_cast<DWORD>(want), &count, nullptr))
            return std::unexpected(lastWin32Error("prepareFileImages: write retained stage", path));
#else
        const auto count = ::write(stage.get(), bytes.data() + done, want);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(errnoError(errno, "prepareFileImages: write retained stage", path));
        }
#endif
        if (count == 0)
            return fail(ErrorCode::IoError,
                        "prepareFileImages: zero-progress write; stage retained at '" + std::string(path) + "'");
        done += static_cast<std::size_t>(count);
    }
#ifdef _WIN32
    if (!FlushFileBuffers(stage.get()))
        return std::unexpected(lastWin32Error("prepareFileImages: flush retained stage", path));
#else
    if (::fsync(stage.get()) != 0)
        return std::unexpected(errnoError(errno, "prepareFileImages: flush retained stage", path));
#endif
    return {};
}
} // namespace

struct PreparedFileImages::State {
    FileImageReceipt receipt;
    std::string before;
    std::string after;
    std::string targetParentPath;
    std::string storePath;
    std::string stageLeaf;
    Native target;
    Native targetParent;
    Native store;
    Native stage;
    Observation targetObservation;
    Observation parentObservation;
    Observation storeObservation;
    Observation stageObservation;
    bool discarded = false;
};

PreparedFileImages::PreparedFileImages(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}
PreparedFileImages::~PreparedFileImages() = default;
PreparedFileImages::PreparedFileImages(PreparedFileImages&&) noexcept = default;
PreparedFileImages& PreparedFileImages::operator=(PreparedFileImages&&) noexcept = default;
const FileImageReceipt* PreparedFileImages::receipt() const noexcept { return m_state ? &m_state->receipt : nullptr; }
std::string_view PreparedFileImages::beforeBytes() const noexcept {
    return m_state ? m_state->before : std::string_view{};
}
std::string_view PreparedFileImages::afterBytes() const noexcept {
    return m_state ? m_state->after : std::string_view{};
}

Result<PreparedFileImages> prepareFileImages(std::string_view target, std::string_view privateStore,
                                             std::string_view after, std::size_t maxBeforeBytes,
                                             std::size_t maxAfterBytes) {
    if (after.size() > maxAfterBytes)
        return fail(ErrorCode::OutOfRange, "prepareFileImages: After exceeds explicit limit before staging");
    auto targetPath = normalizePath(target);
    auto storePath = normalizePath(privateStore);
    if (!targetPath)
        return std::unexpected(targetPath.error());
    if (!storePath)
        return std::unexpected(storePath.error());
    if (!isAbsolutePath(*targetPath) || !isAbsolutePath(*storePath))
        return fail(ErrorCode::InvalidArgument, "prepareFileImages: absolute target/private store required");
    auto state = std::make_unique<PreparedFileImages::State>();
    state->receipt.targetPath = *targetPath;
    state->storePath = *storePath;
    state->targetParentPath = std::string(parentPath(*targetPath));
    auto parent = openNamed(state->targetParentPath, true);
    auto store = openNamed(*storePath, true);
    auto original = openNamed(*targetPath, false);
    if (!parent)
        return std::unexpected(parent.error());
    if (!store)
        return std::unexpected(store.error());
    if (!original)
        return std::unexpected(original.error());
    state->targetParent = std::move(*parent);
    state->store = std::move(*store);
    state->target = std::move(*original);
    auto parentInfo = observe(state->targetParent, state->targetParentPath, true);
    auto storeInfo = observe(state->store, *storePath, true);
    auto beforeInfo = observe(state->target, *targetPath, false);
    if (!parentInfo)
        return std::unexpected(parentInfo.error());
    if (!storeInfo)
        return std::unexpected(storeInfo.error());
    if (!beforeInfo)
        return std::unexpected(beforeInfo.error());
    if (beforeInfo->volume != storeInfo->volume)
        return fail(ErrorCode::Unsupported, "prepareFileImages: stage store must be on the target volume");
    auto ownership = validatePrivateStore(state->store, *storePath);
    if (!ownership)
        return std::unexpected(ownership.error());
    if (beforeInfo->size > maxBeforeBytes)
        return fail(ErrorCode::OutOfRange, "prepareFileImages: Before exceeds explicit limit before reading");
    auto bytes = readBounded(state->target, maxBeforeBytes, *targetPath);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto confirmed = observe(state->target, *targetPath, false);
    if (!confirmed)
        return std::unexpected(confirmed.error());
    if (*confirmed != *beforeInfo || bytes->size() != beforeInfo->size)
        return fail(ErrorCode::Busy, "prepareFileImages: target changed while capturing Before");
    state->before = std::move(*bytes);
    state->after = after;
    state->targetObservation = *beforeInfo;
    state->parentObservation = *parentInfo;
    state->storeObservation = *storeInfo;
    state->stageLeaf = std::format(".frost-prepare-{}-{}", detail::currentProcessId(), detail::nextUniqueId());
    auto stagePath = joinPath(*storePath, state->stageLeaf);
    if (!stagePath)
        return std::unexpected(stagePath.error());
    state->receipt.stagePath = *stagePath;
#ifdef _WIN32
    auto wideStage = toWin32Path(*stagePath);
    if (!wideStage)
        return std::unexpected(wideStage.error());
    state->stage.reset(CreateFileW(wideStage->c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!state->stage.valid())
        return std::unexpected(lastWin32Error("prepareFileImages: exclusive stage", *stagePath));
    state->receipt.metadataObservationProfile =
        "windows-basic/id/links/creation/write/change; NOT security/EA/ADS/short-name";
#else
    state->stage.reset(::openat(state->store.get(), state->stageLeaf.c_str(),
                                O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!state->stage.valid())
        return std::unexpected(errnoError(errno, "prepareFileImages: exclusive stage", *stagePath));
    state->receipt.metadataObservationProfile = "posix-device/inode/mode/uid/gid/links/mtime/ctime; NOT ACL/xattrs";
#endif
    auto written = writeStage(state->stage, after, *stagePath);
    if (!written)
        return std::unexpected(written.error());
    auto stageInfo = observe(state->stage, *stagePath, false);
    auto stageBytes = readBounded(state->stage, maxAfterBytes, *stagePath);
    if (!stageInfo)
        return std::unexpected(stageInfo.error().withContext("private stage retained at '" + *stagePath + "'"));
    if (!stageBytes)
        return std::unexpected(stageBytes.error().withContext("private stage retained at '" + *stagePath + "'"));
    if (stageInfo->size != after.size() || *stageBytes != after)
        return fail(ErrorCode::Busy, "prepareFileImages: stage interference; retained at '" + *stagePath + "'");
    state->stageObservation = *stageInfo;
    state->receipt.beforeSha256 = sha256Hex(state->before);
    state->receipt.afterSha256 = sha256Hex(state->after);
    state->receipt.target = state->targetObservation;
    state->receipt.stage = state->stageObservation;
    state->receipt.targetParent = state->parentObservation;
    state->receipt.store = state->storeObservation;
    PreparedFileImages prepared(std::move(state));
    auto check = prepared.revalidate();
    if (!check)
        return std::unexpected(check.error().withContext("private stage retained at '" + *stagePath + "'"));
    if (*check != FileImageCheck::Matches)
        return fail(ErrorCode::Busy, "prepareFileImages: preparation interference; retained at '" + *stagePath + "'");
    return prepared;
}

Result<FileImageCheck> PreparedFileImages::revalidate() const {
    if (!m_state || m_state->discarded)
        return fail(ErrorCode::InvalidArgument, "prepareFileImages: no live byte stage");
    const auto& s = *m_state;
    for (const auto& item :
         {std::pair{s.targetParentPath, s.parentObservation}, std::pair{s.storePath, s.storeObservation}}) {
        auto named = openNamed(item.first, true);
        if (!named) {
            if (named.error().code() == ErrorCode::NotFound)
                return item.first == s.storePath ? FileImageCheck::StageChanged : FileImageCheck::TargetChanged;
            return std::unexpected(named.error());
        }
        auto info = observe(*named, item.first, true);
        if (!info)
            return std::unexpected(info.error());
        if (!sameObject(*info, item.second))
            return item.first == s.storePath ? FileImageCheck::StageChanged : FileImageCheck::TargetChanged;
        if (item.first == s.storePath) {
            auto ownership = validatePrivateStore(*named, s.storePath);
            if (!ownership) {
                if (ownership.error().code() == ErrorCode::InvalidArgument)
                    return FileImageCheck::StageChanged;
                return std::unexpected(ownership.error());
            }
        }
    }
    for (const bool stage : {false, true}) {
        const auto& path = stage ? s.receipt.stagePath : s.receipt.targetPath;
        const auto& expected = stage ? s.stageObservation : s.targetObservation;
        const auto& expectedBytes = stage ? s.after : s.before;
        const auto changed = stage ? FileImageCheck::StageChanged : FileImageCheck::TargetChanged;
        auto named = openNamed(path, false);
        if (!named) {
            if (named.error().code() == ErrorCode::NotFound)
                return changed;
            if (stage) {
                auto gone = stageUnlinked(s.stage, path);
                if (!gone)
                    return std::unexpected(gone.error());
                if (*gone)
                    return changed;
            }
            return std::unexpected(named.error());
        }
        auto info = observe(*named, path, false);
        if (!info)
            return std::unexpected(info.error());
        if (*info != expected)
            return changed;
        auto bytes = readBounded(*named, expectedBytes.size(), path);
        if (!bytes) {
            if (bytes.error().code() == ErrorCode::OutOfRange)
                return changed;
            return std::unexpected(bytes.error());
        }
        auto final = observe(*named, path, false);
        if (!final)
            return std::unexpected(final.error());
        if (*final != expected || *bytes != expectedBytes)
            return changed;
    }
    return FileImageCheck::Matches;
}

Result<FileImageDiscard> PreparedFileImages::discard() {
    if (!m_state)
        return fail(ErrorCode::InvalidArgument, "prepareFileImages: moved-from owner");
    auto& s = *m_state;
    if (s.discarded)
        return FileImageDiscard::AlreadyAbsent;
    auto parent = openNamed(s.storePath, true);
    if (!parent) {
        if (parent.error().code() == ErrorCode::NotFound)
            return FileImageDiscard::CleanupPending; // Owned directory may have moved with the stage.
        return std::unexpected(parent.error());
    }
    auto parentInfo = observe(*parent, s.storePath, true);
    if (!parentInfo)
        return std::unexpected(parentInfo.error());
    if (!sameObject(*parentInfo, s.storeObservation))
        return FileImageDiscard::CleanupPending;
    auto ownership = validatePrivateStore(*parent, s.storePath);
    if (!ownership) {
        if (ownership.error().code() == ErrorCode::InvalidArgument)
            return FileImageDiscard::CleanupPending;
        return std::unexpected(ownership.error());
    }
    auto named = openNamed(s.receipt.stagePath, false);
    if (!named) {
        auto gone = stageUnlinked(s.stage, s.receipt.stagePath);
        if (!gone)
            return std::unexpected(gone.error());
        if (*gone) {
            s.stage.reset();
            s.discarded = true;
            return FileImageDiscard::AlreadyAbsent;
        }
        if (named.error().code() == ErrorCode::NotFound)
            return FileImageDiscard::CleanupPending; // Renamed object is still linked; retain its owner.
        return std::unexpected(named.error());
    }
    auto current = observe(*named, s.receipt.stagePath, false);
    if (!current) {
        if (current.error().code() == ErrorCode::Unsupported)
            return FileImageDiscard::CleanupPending;
        return std::unexpected(current.error());
    }
    if (*current != s.stageObservation)
        return FileImageDiscard::CleanupPending;
    auto bytes = readBounded(*named, s.after.size(), s.receipt.stagePath);
    if (!bytes || *bytes != s.after)
        return FileImageDiscard::CleanupPending;
#ifdef _WIN32
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(s.stage.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
        return std::unexpected(lastWin32Error("prepareFileImages: discard exact stage handle", s.receipt.stagePath));
    named->reset();
    s.stage.reset();
#else
    // The private owner-directory contract excludes cooperating writers here. This is not a
    // claim that Linux offers inode-conditional unlink against hostile equivalent-rights swaps.
    if (::unlinkat(s.store.get(), s.stageLeaf.c_str(), 0) != 0)
        return std::unexpected(
            errnoError(errno, "prepareFileImages: discard owned private stage", s.receipt.stagePath));
    s.stage.reset();
#endif
    auto remaining = openNamed(s.receipt.stagePath, false);
    if (remaining || remaining.error().code() != ErrorCode::NotFound)
        return FileImageDiscard::CleanupPending;
    s.discarded = true;
    return FileImageDiscard::Removed;
}

} // namespace frost::io
