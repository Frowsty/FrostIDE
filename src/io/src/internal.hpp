// SPDX-License-Identifier: Apache-2.0
//
// Private helpers shared by frost::io's implementation files: RAII wrappers for
// OS handles and descriptors, process-unique names, small error builders, and the
// POSIX permission rules of writeFileAtomic. Not installed; outside src/io only
// io_unit_tests includes this, to test the pure helpers on every platform.
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

namespace frost::io::detail {

// A number that is unique within this process (a relaxed atomic counter; only
// uniqueness matters, not ordering). Used for temp-file and pipe names.
[[nodiscard]] std::uint64_t nextUniqueId() noexcept;

// The current process id.
[[nodiscard]] std::uint32_t currentProcessId() noexcept;

// The sibling temp-file name used by writeFileAtomic for `targetName`:
// ".<name>.frost-tmp-<pid>-<id>". At most 64 bytes of the target name are used
// (cut at a UTF-8 character boundary) so long names stay under the 255-unit
// component limit of NTFS and ext4.
[[nodiscard]] std::string atomicTempName(std::string_view targetName, std::uint64_t id);

// The marker every temp name contains; tests use it to prove none are left over.
inline constexpr std::string_view kAtomicTempMarker = ".frost-tmp-";

// Errors with the operation-specific refinements documented in file_system.hpp.
[[nodiscard]] Error isADirectoryError(std::string_view operation, std::string_view path);
[[nodiscard]] Error notADirectoryError(std::string_view operation, std::string_view path);

// Whether `path` names a Windows batch file (.bat or .cmd, ASCII case-insensitive),
// ignoring trailing dots and spaces, which Win32 strips from names.
[[nodiscard]] bool isBatchFileName(std::string_view path) noexcept;

// CreateProcessW's command-line limit in UTF-16 units, including the terminator.
inline constexpr std::size_t kMaxWindowsCommandLineUnits = 32767;

/*
 * What a POSIX access ACL (acl(5)) granted every non-owner, as permission triples
 * (0-7, read 4, write 2, execute 1). `group`: the permissions every member of the
 * owning group had, whichever entry applied to them (the owning-group entry, or a
 * named-user entry, which takes precedence), each limited by the mask. `other`: the
 * permissions every other non-owner had (the other entry, or a named-user or
 * named-group entry, masked). Named entries can grant less than their class, so a
 * file that loses the ACL must not give the classes more than these.
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

#ifdef _WIN32

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
