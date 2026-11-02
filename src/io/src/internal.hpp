// SPDX-License-Identifier: Apache-2.0
//
// Private helpers shared by frost::io's implementation files (not installed).
//
/*
 * RAII wrappers for OS handles and descriptors, process-unique names, small error
 * builders, deadline and wait-slice arithmetic, and the POSIX permission rules of
 * writeFileAtomic. Outside src/io only the io test binaries include this: to test
 * the pure helpers on every platform, to name the temp-file marker, and to lower
 * the wait slice.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace frost::io {
struct ProcessSpec; // frost/io/process.hpp
} // namespace frost::io

namespace frost::io::detail {

// A number that is unique within this process (a relaxed atomic counter; only uniqueness matters, not
// ordering). Used for writeFileAtomic's temp-file names; pipe names use random bits instead (process_win.cpp).
[[nodiscard]] std::uint64_t nextUniqueId() noexcept;

// The current process id.
[[nodiscard]] std::uint32_t currentProcessId() noexcept;

/*
 * The sibling temp-file name used by writeFileAtomic for `targetName`:
 * ".<name>.frost-tmp-<pid>-<id>". At most 64 bytes of the target name are used
 * (cut at a UTF-8 character boundary) so long names stay under the 255-unit
 * component limit of NTFS and ext4.
 */
[[nodiscard]] std::string atomicTempName(std::string_view targetName, std::uint64_t id);

// The marker every temp name contains; the file-system tests use it to prove none are left over.
inline constexpr std::string_view kAtomicTempMarker = ".frost-tmp-";

// ReplaceFileW's two documented failures that require keeping the replacement file. The caller
// supplies a backup name, so even the displaced original has a known recovery path.
[[nodiscard]] inline std::optional<Error> timestampedReplaceRecovery(std::uint32_t error, std::string_view target,
                                                                     std::string_view replacement,
                                                                     std::string_view backup) {
    constexpr std::uint32_t kUnableToMoveReplacement = 1176;
    constexpr std::uint32_t kUnableToMoveReplacement2 = 1177;
    if (error == kUnableToMoveReplacement)
        return Error(ErrorCode::IoError, "Atomic replacement failed; original remains at '" + std::string(target) +
                                             "' and replacement remains at '" + std::string(replacement) + "'");
    if (error == kUnableToMoveReplacement2)
        return Error(ErrorCode::IoError, "Atomic replacement failed; original remains at '" + std::string(backup) +
                                             "' and replacement remains at '" + std::string(replacement) + "'");
    return std::nullopt;
}

// Errors with the operation-specific refinements documented in file_system.hpp.
[[nodiscard]] Error isADirectoryError(std::string_view operation, std::string_view path);
[[nodiscard]] Error notADirectoryError(std::string_view operation, std::string_view path);

// Whether `path` names a Windows batch file (.bat or .cmd, ASCII case-insensitive),
// ignoring trailing dots and spaces, which Win32 strips from names.
[[nodiscard]] bool isBatchFileName(std::string_view path) noexcept;

// CreateProcessW's command-line limit in UTF-16 units, including the terminator.
inline constexpr std::size_t kMaxWindowsCommandLineUnits = 32767;

// The Windows command line for `spec` with `executable` (normalised) as argv[0]: the quoted
// arguments, or spec.rawArguments verbatim after a space (process.hpp). Errors: those of
// buildWindowsCommandLine; InvalidArgument for rawArguments together with arguments.
[[nodiscard]] Result<std::string> windowsCommandLineFor(std::string_view executable, const ProcessSpec& spec);

/*
 * What a POSIX access ACL (acl(5)) granted every non-owner, as permission triples
 * (0-7, read 4, write 2, execute 1). `group`: the permissions every member of the
 * owning group had, whichever entry applied to them (the owning-group entry, or a
 * named-user entry, which takes precedence), each limited by the mask. `other`: the
 * permissions every other non-owner had (the other entry, which the mask never
 * limits, or a named-user or named-group entry, masked). Named entries can grant
 * less than their class, so a file that loses the ACL must not give the classes
 * more than these.
 */
struct AclFloor {
    std::uint32_t group = 0;
    std::uint32_t other = 0;

    friend bool operator==(const AclFloor&, const AclFloor&) = default;
};

/*
 * Parses a system.posix_acl_access attribute value in the kernel's format
 * (linux/posix_acl_xattr.h: a little-endian version word 2, then 8-byte entries of
 * a 16-bit tag, 16-bit permissions and a 32-bit id). nullopt if it is malformed:
 * wrong size or version, an unknown tag, or no owning-group or other entry.
 * Pure and portable. O(entries).
 */
[[nodiscard]] std::optional<AclFloor> accessAclFloor(std::string_view xattr) noexcept;

/*
 * The permission bits (07777) writeFileAtomic gives a POSIX replacement file, from
 * the replaced target's `mode` (st_mode & 07777), whether this process could keep
 * its owner and group, and `acl`, the floor of the target's access ACL when it has
 * one. Every rule only removes bits, so no one gets more access than the target
 * gave them:
 *   - acl: the group and other classes keep only acl->group and acl->other (the
 *     mode's group bits are the ACL mask, and named entries may deny). When the
 *     ACL is then copied, it restores the exact classes.
 *   - !groupKept: old group members become others and some others become members,
 *     so both classes keep only what both had (group & other), and S_ISGID goes:
 *     0644 stays 0644, 0664 becomes 0644, 0640 and 0604 become 0600.
 *   - !ownerKept: the old owner becomes a member or an other, so both classes keep
 *     only what the owner had, and S_ISUID goes: 0644 stays, 0077 becomes 0000.
 * Pure and portable; unit-tested on every platform.
 */
[[nodiscard]] std::uint32_t replacementMode(std::uint32_t mode, bool ownerKept, bool groupKept,
                                            std::optional<AclFloor> acl) noexcept;

// The steady-clock time `timeout` from now, saturated at time_point::max() so that
// a huge timeout (IClock::Duration::max() as "no limit") never overflows.
[[nodiscard]] inline std::chrono::steady_clock::time_point deadlineAfter(IClock::Duration timeout) noexcept {
    using TimePoint = std::chrono::steady_clock::time_point;
    static_assert(std::is_same_v<TimePoint::duration, IClock::Duration>, "steady_clock ticks in nanoseconds");
    const TimePoint now = std::chrono::steady_clock::now();
    if (timeout <= IClock::Duration::zero()) {
        return now;
    }
    return timeout >= TimePoint::max() - now ? TimePoint::max() : now + timeout;
}

// The default of maxWaitSliceMs(): 2^30 ms, about 12.4 days, which fits poll()'s int and stays below INFINITE.
inline constexpr std::int64_t kDefaultMaxWaitSliceMs = std::int64_t{1} << 30;

/*
 * The longest single blocking wait that has a deadline (one poll() or Wait*
 * call) in milliseconds, kDefaultMaxWaitSliceMs unless a test lowered it. A
 * wait whose deadline is further away blocks in slices and re-checks the
 * deadline after each, so a slice that ends is never mistaken for the deadline
 * and even a timeout of IClock::Duration::max() does not end early. Waits with
 * no deadline (wait() without a timeout, the blocking pipe reads and writes)
 * are not sliced and block in one call. Thread-safe (relaxed atomic).
 */
[[nodiscard]] std::int64_t maxWaitSliceMs() noexcept;

// Tests only: sets maxWaitSliceMs() (clamped to 1..kDefaultMaxWaitSliceMs), to exercise the slicing
// without waiting for days. Waits already blocking keep their current slice. Thread-safe.
void setMaxWaitSliceMsForTesting(std::int64_t milliseconds) noexcept;

// The milliseconds one blocking wait may take when `left` remains until its deadline: rounded up
// (so the wait does not end just short of it), at most maxWaitSliceMs(), and 0 once it has passed.
[[nodiscard]] std::int64_t waitSliceMs(IClock::Duration left) noexcept;

#ifdef _WIN32

/*
 * The 8.3 short spelling of the existing directory `normalized` (a normalised Windows path), without the
 * \\?\ prefix, when it fits a process's current directory (fewer than MAX_PATH - 12 UTF-16 units); nullopt
 * when the volume has no short names or the result is still too long. process_win.cpp retries
 * CreateProcessW with it when a long working directory is refused. Thread-safe.
 */
[[nodiscard]] std::optional<std::wstring> shortWorkingDirectory(const std::string& normalized);

// Tests only: while true, the Windows launcher treats a working directory of MAX_PATH - 12 units or more as
// refused by CreateProcessW (as with LongPathsEnabled = 0), so the short-name fallback runs on every machine.
void setRefuseLongWorkingDirectoryForTesting(bool refuse) noexcept;

// Owns a Win32 HANDLE (either INVALID_HANDLE_VALUE or nullptr means "none").
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : m_handle(handle) {}
    UniqueHandle(UniqueHandle&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.m_handle, nullptr));
        }
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete ("a handle has exactly one owner");
    UniqueHandle& operator=(const UniqueHandle&) = delete ("a handle has exactly one owner");
    ~UniqueHandle() { reset(); }

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] bool valid() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(m_handle, nullptr); }
    void reset(HANDLE handle = nullptr) noexcept {
        if (valid()) {
            CloseHandle(m_handle);
        }
        m_handle = handle;
    }

private:
    HANDLE m_handle = nullptr;
};

#else

// Owns a POSIX file descriptor (-1 means "none").
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : m_fd(fd) {}
    UniqueFd(UniqueFd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.m_fd, -1));
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete ("a descriptor has exactly one owner");
    UniqueFd& operator=(const UniqueFd&) = delete ("a descriptor has exactly one owner");
    ~UniqueFd() { reset(); }

    [[nodiscard]] int get() const noexcept { return m_fd; }
    [[nodiscard]] bool valid() const noexcept { return m_fd >= 0; }
    [[nodiscard]] int release() noexcept { return std::exchange(m_fd, -1); }
    void reset(int fd = -1) noexcept {
        if (m_fd >= 0) {
            // close() is not retried on EINTR: on Linux the descriptor is released regardless.
            ::close(m_fd);
        }
        m_fd = fd;
    }

private:
    int m_fd = -1;
};

#endif

} // namespace frost::io::detail
