// SPDX-License-Identifier: Apache-2.0
//
// The Windows backend of RealFileWatcher (ADR 0020): ReadDirectoryChangesExW on the reactor's completion
// port, then plain ReadDirectoryChangesW, then polling.
//
/*
 * One directory handle per watch (FILE_SHARE_DELETE, so the root stays renamable and deletable), assigned
 * to an asio stream_handle so its completions arrive on the reactor's IOCP; every read is issued through
 * an asio overlapped_ptr whose handler runs on the watch's strand (the pattern of asio's
 * windows/transmit_file example, BSL-1.0; no code copied). Two buffers alternate: a completed buffer is
 * parsed after the next read has been issued. 256 KiB on fixed drives; 64 KiB elsewhere, the limit of
 * SMB redirectors.
 *
 * Completion table:
 *   success, n > 0              parse; re-arm
 *   success, 0 bytes            the kernel's buffer overflowed: Rescan(root); re-arm
 *   ERROR_NOTIFY_ENUM_DIR       the same
 *   ERROR_OPERATION_ABORTED,    the handle was closed (stop or shutdown): end without re-arming
 *   ERROR_NOTIFY_CLEANUP
 *   anything else               if the root is gone: Removed(root), then NotFound; otherwise retry after
 *                               100 ms, and after three consecutive failures poll (RepeatedErrors); the
 *                               first good read after failures adds Rescan(root) (their changes are lost)
 * A read refused at call time with ERROR_INVALID_FUNCTION, ERROR_INVALID_PARAMETER, ERROR_NOT_SUPPORTED or
 * ERROR_INVALID_LEVEL falls back from the Ex call to the plain one, and from the plain one to polling
 * (Unsupported).
 *
 * Root identity. The handle follows the directory, so a renamed or replaced root would go on reporting
 * under the old path. After every completion and every 2 s the root path is opened again and its volume
 * serial and file id compared with the handle's (also its delete-pending flag); a mismatch ends the watch
 * with Removed(root), then NotFound.
 *
 * Names. RDCW reports names as they were used; components containing '~' may be 8.3 aliases and are
 * expanded with GetLongPathNameW while the entry exists (removed entries keep the reported spelling).
 * Names that are not valid UTF-16 become Rescan(root). The Ex call reports attributes, so every entry
 * has its type; Modified of a directory is dropped (its mtime changes with every child, see
 * file_watcher.hpp). The plain call reports no types; rename destinations are resolved with
 * GetFileAttributesW.
 */

#include "frost/core/assert.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"

#include "internal.hpp"
#include "real_file_watcher_impl.hpp" // asio first: it includes winsock2.h before windows.h.
#include "watcher_testing.hpp"
#include <asio/windows/overlapped_ptr.hpp>
#include <asio/windows/stream_handle.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <optional>
#include <utility>
#include <vector>

#include <windows.h>

namespace frost::io {
namespace {

// Test hooks (watcher_testing.hpp). Relaxed: tests set them before starting the watch they target.
constinit std::atomic<unsigned> g_faultEnumDir{0};
constinit std::atomic<unsigned> g_faultExRefused{0};
constinit std::atomic<unsigned> g_faultPlainRefused{0};
constinit std::atomic<unsigned> g_faultReadError{0};
constinit std::atomic<std::size_t> g_bufferBytes{0};

std::atomic<unsigned>& faultCounter(testing::WindowsWatchFault fault) noexcept {
    switch (fault) {
    case testing::WindowsWatchFault::NotifyEnumDir: return g_faultEnumDir;
    case testing::WindowsWatchFault::ExRefused: return g_faultExRefused;
    case testing::WindowsWatchFault::PlainRefused: return g_faultPlainRefused;
    case testing::WindowsWatchFault::ReadError:
    case testing::WindowsWatchFault::None: break;
    }
    return g_faultReadError;
}

// Consumes one injected occurrence of `fault`.
[[nodiscard]] bool takeFault(std::atomic<unsigned>& counter) noexcept {
    unsigned current = counter.load(std::memory_order_relaxed);
    while (current != 0) {
        if (counter.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

} // namespace

void testing::injectWindowsWatchFaultForTesting(WindowsWatchFault fault, unsigned count) noexcept {
    if (fault == WindowsWatchFault::None) {
        return;
    }
    faultCounter(fault).store(count, std::memory_order_relaxed);
}

void testing::setWindowsReadBufferBytesForTesting(std::size_t bytes) noexcept {
    g_bufferBytes.store(bytes, std::memory_order_relaxed);
}

namespace detail {
namespace {

using namespace std::chrono_literals;

constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
constexpr std::size_t kFixedDriveBuffer = 256 * 1024;
constexpr std::size_t kOtherDriveBuffer = 64 * 1024; // The SMB redirector's limit.
constexpr unsigned kMaxConsecutiveErrors = 3;
constexpr auto kIdentityPeriod = 2s;
constexpr auto kRetryDelay = 100ms;

// A directory's identity: volume serial number and 128-bit file id (FileIdInfo), or the 64-bit index of
// GetFileInformationByHandle where FileIdInfo is not supported (FAT, some redirectors).
struct Identity {
    std::uint64_t volume = 0;
    std::array<std::uint8_t, 16> id{};
    friend bool operator==(const Identity&, const Identity&) = default;
};

[[nodiscard]] Result<Identity> identityOf(HANDLE handle) {
    Identity identity;
    FILE_ID_INFO info{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) {
        identity.volume = info.VolumeSerialNumber;
        std::memcpy(identity.id.data(), info.FileId.Identifier, identity.id.size());
        return identity;
    }
    BY_HANDLE_FILE_INFORMATION basic{};
    if (!GetFileInformationByHandle(handle, &basic)) {
        return std::unexpected(lastWin32Error("GetFileInformationByHandle"));
    }
    identity.volume = basic.dwVolumeSerialNumber;
    const std::uint64_t index = (std::uint64_t{basic.nFileIndexHigh} << 32) | basic.nFileIndexLow;
    std::memcpy(identity.id.data(), &index, sizeof(index));
    return identity;
}

[[nodiscard]] bool deletePending(HANDLE handle) noexcept {
    FILE_STANDARD_INFO info{};
    return GetFileInformationByHandleEx(handle, FileStandardInfo, &info, sizeof(info)) && info.DeletePending;
}

[[nodiscard]] bool refusedAtCallTime(DWORD error) noexcept {
    return error == ERROR_INVALID_FUNCTION || error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
           error == ERROR_INVALID_LEVEL;
}

[[nodiscard]] std::optional<FileType> typeFromAttributes(DWORD attributes) noexcept {
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return std::nullopt;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return FileType::Symlink; // Symlinks and junctions, as listDirectory reports them.
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? FileType::Directory : FileType::Regular;
}

class WinWatchState final : public BoundState, public WatchProducer {
public:
    enum class Api : std::uint8_t { Extended, Plain };

    WinWatchState(Reactor::Impl& reactor, std::shared_ptr<RealFileWatcher::Impl> owner,
                  std::shared_ptr<WatchQueue> queue, UniqueHandle directory, std::wstring win32Root, Identity identity,
                  std::size_t bufferBytes)
        : BoundState(reactor), m_owner(std::move(owner)), m_queue(std::move(queue)), m_handle(strand()),
          m_identityTimer(strand()), m_retryTimer(strand()), m_directory(std::move(directory)),
          m_win32Root(std::move(win32Root)), m_identity(identity), m_bufferBytes(bufferBytes) {
        for (auto& buffer : m_buffers) {
            buffer = std::make_unique<DWORD[]>(m_bufferBytes / sizeof(DWORD));
        }
    }

    // The stream_handle closes the directory handle if it still owns it; m_directory closes it otherwise.
    ~WinWatchState() override = default;

    void shutdownOnStrand() noexcept override {
        stopOnStrand();
        m_queue->close(Error(ErrorCode::Cancelled, "the reactor is shut down"), /*deliverPending=*/false);
    }

    void requestStop() noexcept override {
        // Refused once the reactor shuts down; shutdownOnStrand then stops the state.
        static_cast<void>(post([this] { stopOnStrand(); }));
    }

    // On the strand: registers the handle and issues the first read. A value: the reason to poll instead.
    [[nodiscard]] Result<std::optional<DegradeTo>> startOnStrand() {
        if (m_stopped) {
            return fail(ErrorCode::Cancelled, "the reactor is shut down");
        }
        asio::error_code ec;
        static_cast<void>(m_handle.assign(m_directory.get(), ec));
        if (ec) {
            return fail(ErrorCode::IoError, std::format("watch '{}': cannot register the directory handle: {}",
                                                        m_queue->root(), ec.message()));
        }
        static_cast<void>(m_directory.release()); // Owned by m_handle from here on.
        for (;;) {
            const DWORD error = issueRead();
            if (error == ERROR_SUCCESS) {
                break;
            }
            if (!refusedAtCallTime(error)) {
                return std::unexpected(win32Error(error, "ReadDirectoryChangesW", m_queue->root()));
            }
            if (m_api == Api::Extended) {
                m_api = Api::Plain;
                continue;
            }
            return std::optional<DegradeTo>(
                DegradeTo{DegradeReason::Unsupported, error,
                          std::format("ReadDirectoryChangesW is not supported here (error {})", error)});
        }
        armIdentityTimer();
        return std::optional<DegradeTo>();
    }

    [[nodiscard]] Api api() const noexcept { return m_api; }

private:
    // Idempotent. A polling fallback (set after the first stop, by degrade) is stopped by later calls.
    void stopOnStrand() noexcept {
        if (m_fallback) {
            m_fallback->requestStop();
        }
        if (m_stopped) {
            return;
        }
        m_stopped = true;
        m_identityTimer.cancel();
        m_retryTimer.cancel();
        asio::error_code ignored;
        static_cast<void>(m_handle.close(ignored)); // The pending read completes with ABORTED or CLEANUP.
    }

    // Issues the next read into the free buffer. Returns ERROR_SUCCESS once it is pending.
    [[nodiscard]] DWORD issueRead() {
        const unsigned index = m_nextBuffer;
        asio::windows::overlapped_ptr overlapped(
            strand(),
            bindHandler([this, index](const asio::error_code& ec, std::size_t bytes) { onRead(ec, bytes, index); }));
        BOOL ok = FALSE;
        if (m_api == Api::Extended) {
            if (takeFault(g_faultExRefused)) {
                SetLastError(ERROR_INVALID_FUNCTION);
            } else {
                ok = ReadDirectoryChangesExW(m_handle.native_handle(), m_buffers[index].get(),
                                             static_cast<DWORD>(m_bufferBytes), m_queue->config().options.recursive,
                                             kNotifyFilter, nullptr, overlapped.get(), nullptr,
                                             ReadDirectoryNotifyExtendedInformation);
            }
        } else if (takeFault(g_faultPlainRefused)) {
            SetLastError(ERROR_INVALID_FUNCTION);
        } else {
            ok = ReadDirectoryChangesW(m_handle.native_handle(), m_buffers[index].get(),
                                       static_cast<DWORD>(m_bufferBytes), m_queue->config().options.recursive,
                                       kNotifyFilter, nullptr, overlapped.get(), nullptr);
        }
        if (!ok) {
            return GetLastError(); // `overlapped` is destroyed without running the handler.
        }
        static_cast<void>(overlapped.release()); // Completes through the IOCP.
        m_nextBuffer = 1 - index;
        return ERROR_SUCCESS;
    }

    void onRead(const asio::error_code& completion, std::size_t bytes, unsigned index) {
        if (m_stopped) {
            return;
        }
        DWORD error = static_cast<DWORD>(completion.value());
        if (error == ERROR_SUCCESS && takeFault(g_faultEnumDir)) {
            error = ERROR_NOTIFY_ENUM_DIR;
        } else if (error == ERROR_SUCCESS && takeFault(g_faultReadError)) {
            error = ERROR_ACCESS_DENIED;
        }
        if (error == ERROR_OPERATION_ABORTED || error == ERROR_NOTIFY_CLEANUP) {
            return; // Closed.
        }
        if (error != ERROR_SUCCESS && error != ERROR_NOTIFY_ENUM_DIR) {
            failedRead(error);
            return;
        }
        // The changes the failed reads carried are lost: the first good read after them rescans.
        const bool recovered = m_consecutiveErrors != 0;
        m_consecutiveErrors = 0;
        // Re-arm before parsing so the next read is pending while this buffer is processed.
        const DWORD rearm = issueRead();
        if (error == ERROR_NOTIFY_ENUM_DIR || bytes == 0 || recovered) {
            m_queue->pushOverflow(m_queue->root());
        } else {
            parse(index, bytes);
        }
        if (!checkRoot()) {
            return;
        }
        if (rearm != ERROR_SUCCESS) {
            failedRead(rearm);
        }
    }

    // A failed read or re-arm: the root may be gone; otherwise retry, and poll after three failures.
    void failedRead(DWORD error) {
        if (!checkRoot()) {
            return;
        }
        if (++m_consecutiveErrors >= kMaxConsecutiveErrors) {
            degrade(DegradeReason::RepeatedErrors, error,
                    std::format("ReadDirectoryChangesW failed {} times in a row (last error {})", m_consecutiveErrors,
                                error));
            return;
        }
        m_retryTimer.expires_after(kRetryDelay);
        m_retryTimer.async_wait(bindHandler([this](const asio::error_code& ec) {
            if (ec || m_stopped) {
                return;
            }
            const DWORD again = issueRead();
            if (again != ERROR_SUCCESS) {
                failedRead(again);
            }
        }));
    }

    // Switches the watch to polling: Rescan(root) covers everything until the first scan.
    void degrade(DegradeReason reason, std::uint64_t value, std::string detail) {
        stopOnStrand();
        m_queue->setMode(WatchMode::Polling, reason, value, std::move(detail));
        m_queue->updateStatus([](WatchStatus& status) { status.osWatches = 0; });
        m_queue->pushOverflow(m_queue->root());
        m_fallback = startPollingFromStrand(m_owner, m_queue).producer;
    }

    // True while the root path still names the watched directory; otherwise ends the watch.
    bool checkRoot() {
        if (m_stopped) {
            return false;
        }
        bool gone = deletePending(m_handle.native_handle());
        if (!gone) {
            UniqueHandle fresh(CreateFileW(m_win32Root.c_str(), FILE_READ_ATTRIBUTES,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            if (!fresh.valid()) {
                const DWORD error = GetLastError();
                gone = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
            } else if (auto identity = identityOf(fresh.get()); identity && *identity != m_identity) {
                gone = true;
            }
        }
        if (!gone) {
            return true;
        }
        const std::string& root = m_queue->root();
        static_cast<void>(m_queue->push(RawEvent{RawKind::Removed, root, FileType::Directory, false, 0}));
        m_queue->close(Error(ErrorCode::NotFound, std::format("watch '{}': the root was removed or moved", root)),
                       /*deliverPending=*/true);
        stopOnStrand();
        return false;
    }

    void armIdentityTimer() {
        m_identityTimer.expires_after(kIdentityPeriod);
        m_identityTimer.async_wait(bindHandler([this](const asio::error_code& ec) {
            if (ec || m_stopped) {
                return;
            }
            if (checkRoot()) {
                armIdentityTimer();
            }
        }));
    }

    // The event path for a name relative to the root, with 8.3 aliases expanded while the entry exists.
    [[nodiscard]] std::optional<std::string> eventPath(std::wstring_view name, bool exists) const {
        std::wstring relative(name);
        if (exists && relative.find(L'~') != std::wstring::npos) {
            std::wstring full = m_win32Root;
            if (full.back() != L'\\') {
                full += L'\\';
            }
            const std::size_t prefix = full.size();
            full += relative;
            const DWORD needed = GetLongPathNameW(full.c_str(), nullptr, 0);
            if (needed != 0) {
                std::wstring expanded(needed, L'\0');
                const DWORD written = GetLongPathNameW(full.c_str(), expanded.data(), needed);
                if (written != 0 && written < needed) {
                    expanded.resize(written);
                    if (expanded.size() > prefix &&
                        CompareStringOrdinal(expanded.data(), static_cast<int>(prefix), full.data(),
                                             static_cast<int>(prefix), TRUE) == CSTR_EQUAL) {
                        relative = expanded.substr(prefix);
                    }
                }
            }
        }
        auto utf8 = fromWide(relative);
        if (!utf8) {
            return std::nullopt;
        }
        std::string path = m_queue->root();
        if (path.back() != '\\') {
            path += '\\';
        }
        path += *utf8;
        return path;
    }

    // Converts one completed buffer into raw events.
    void parse(unsigned index, std::size_t bytes) {
        const auto* base = reinterpret_cast<const std::byte*>(m_buffers[index].get());
        std::vector<RawEvent> events;
        bool lastWasOldName = false;
        bool unrepresentable = false;
        std::size_t offset = 0;
        for (;;) {
            if (offset + sizeof(DWORD) * 3 > bytes) {
                break; // Truncated (never expected): the rest is lost, so rescan.
            }
            DWORD next = 0;
            DWORD action = 0;
            std::wstring_view name;
            std::optional<FileType> type;
            if (m_api == Api::Extended) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_EXTENDED_INFORMATION*>(base + offset);
                next = info->NextEntryOffset;
                action = info->Action;
                name = std::wstring_view(info->FileName, info->FileNameLength / sizeof(WCHAR));
                type = typeFromAttributes(info->FileAttributes);
            } else {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
                next = info->NextEntryOffset;
                action = info->Action;
                name = std::wstring_view(info->FileName, info->FileNameLength / sizeof(WCHAR));
            }
            const bool exists =
                action == FILE_ACTION_ADDED || action == FILE_ACTION_MODIFIED || action == FILE_ACTION_RENAMED_NEW_NAME;
            auto path = eventPath(name, exists);
            if (!path) {
                unrepresentable = true;
            } else {
                switch (action) {
                case FILE_ACTION_ADDED: events.push_back({RawKind::Created, std::move(*path), type, false, 0}); break;
                case FILE_ACTION_REMOVED: events.push_back({RawKind::Removed, std::move(*path), type, false, 0}); break;
                case FILE_ACTION_MODIFIED:
                    if (type != FileType::Directory) {
                        events.push_back({RawKind::Modified, std::move(*path), type, false, 0});
                    }
                    break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    events.push_back({RawKind::MovedFrom, std::move(*path), type, false, ++m_cookie});
                    break;
                case FILE_ACTION_RENAMED_NEW_NAME: {
                    if (m_api == Api::Plain) {
                        std::wstring full = m_win32Root;
                        if (full.back() != L'\\') {
                            full += L'\\';
                        }
                        full += name;
                        type = typeFromAttributes(GetFileAttributesW(full.c_str()));
                    }
                    const std::uint64_t cookie = lastWasOldName ? m_cookie : 0;
                    events.push_back({RawKind::MovedTo, std::move(*path), type, false, cookie});
                    break;
                }
                default: break;
                }
            }
            lastWasOldName = action == FILE_ACTION_RENAMED_OLD_NAME;
            if (next == 0) {
                break;
            }
            offset += next;
        }
        if (!events.empty()) {
            static_cast<void>(m_queue->push(events));
        }
        if (unrepresentable) {
            m_queue->pushOverflow(m_queue->root());
        }
    }

    std::shared_ptr<RealFileWatcher::Impl> m_owner;
    std::shared_ptr<WatchQueue> m_queue;
    asio::windows::stream_handle m_handle;
    asio::steady_timer m_identityTimer;
    asio::steady_timer m_retryTimer;
    UniqueHandle m_directory; // Until startOnStrand hands it to m_handle.
    const std::wstring m_win32Root;
    const Identity m_identity;
    const std::size_t m_bufferBytes;
    std::array<std::unique_ptr<DWORD[]>, 2> m_buffers;
    unsigned m_nextBuffer = 0;
    Api m_api = Api::Extended;
    bool m_stopped = false;
    unsigned m_consecutiveErrors = 0;
    std::uint64_t m_cookie = 0;
    std::shared_ptr<WatchProducer> m_fallback;
};

// The read buffer size for `win32Root`: 256 KiB on fixed drives, 64 KiB elsewhere (or the test override).
[[nodiscard]] std::size_t bufferBytesFor(const std::wstring& win32Root) {
    if (const std::size_t forced = g_bufferBytes.load(std::memory_order_relaxed); forced != 0) {
        return (forced + sizeof(DWORD) - 1) / sizeof(DWORD) * sizeof(DWORD);
    }
    std::array<wchar_t, 1024> volume{};
    if (!GetVolumePathNameW(win32Root.c_str(), volume.data(), static_cast<DWORD>(volume.size()))) {
        return kOtherDriveBuffer;
    }
    return GetDriveTypeW(volume.data()) == DRIVE_FIXED ? kFixedDriveBuffer : kOtherDriveBuffer;
}

} // namespace

sched::Task<Result<NativeOutcome>> startNativeWatch(std::shared_ptr<RealFileWatcher::Impl> owner,
                                                    std::shared_ptr<WatchQueue> queue, std::stop_token token) {
    if (token.stop_requested()) {
        co_return fail(ErrorCode::Cancelled, "watch() was cancelled");
    }
    auto win32Root = toWin32Path(queue->root());
    if (!win32Root) {
        co_return std::unexpected(std::move(win32Root.error()));
    }
    UniqueHandle directory(CreateFileW(win32Root->c_str(), FILE_LIST_DIRECTORY,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr));
    if (!directory.valid()) {
        co_return std::unexpected(lastWin32Error("watch: CreateFileW", queue->root()));
    }
    auto identity = identityOf(directory.get());
    if (!identity) {
        co_return std::unexpected(std::move(identity.error()));
    }
    const std::size_t bufferBytes = bufferBytesFor(*win32Root);
    auto bound = makeBound<WinWatchState>(owner->reactor, owner, queue, std::move(directory), std::move(*win32Root),
                                          *identity, bufferBytes);
    if (!bound) {
        co_return std::unexpected(std::move(bound.error()));
    }
    std::shared_ptr<WinWatchState> state = std::move(*bound);
    std::optional<DegradeTo> degrade;
    auto start = [&]() -> Result<void> {
        auto started = state->startOnStrand();
        if (!started) {
            return std::unexpected(std::move(started.error()));
        }
        degrade = std::move(*started);
        return {};
    };
    Result<void> started = co_await RunOnStrand<decltype(start)>(*state, owner->scheduler, start);
    if (!started) {
        state->requestStop();
        co_return std::unexpected(std::move(started.error()));
    }
    if (degrade) {
        state->requestStop();
        co_return NativeOutcome{nullptr, std::move(*degrade)};
    }
    if (state->api() == WinWatchState::Api::Plain) {
        queue->updateStatus([](WatchStatus& status) {
            status.detail = "ReadDirectoryChangesExW is not supported here; using ReadDirectoryChangesW";
        });
    }
    queue->updateStatus([](WatchStatus& status) { status.osWatches = 1; });
    co_return NativeOutcome{std::move(state), {}};
}

} // namespace detail
} // namespace frost::io
