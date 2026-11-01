// SPDX-License-Identifier: Apache-2.0
//
// Shared private helpers of frost::io (see internal.hpp).

#include "internal.hpp"

#include <algorithm>
#include <atomic>
#include <format>

namespace frost::io::detail {
namespace {

// constinit and trivially destructible: no code runs before main or after exit.
constinit std::atomic<std::uint64_t> g_nextId{1};

// maxWaitSliceMs(). Relaxed: only the value matters, and a test sets it before starting the waits it
// exercises (from the same thread), so no other memory has to be ordered with it.
constinit std::atomic<std::int64_t> g_maxWaitSliceMs{kDefaultMaxWaitSliceMs};

// Bytes of a UTF-8 string that fit in `limit` without splitting a character.
std::size_t utf8Prefix(std::string_view text, std::size_t limit) noexcept {
    if (text.size() <= limit) {
        return text.size();
    }
    std::size_t end = limit;
    // Back up over continuation bytes (10xxxxxx) to the start of a character.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) {
        --end;
    }
    return end;
}

} // namespace

std::uint64_t nextUniqueId() noexcept { return g_nextId.fetch_add(1, std::memory_order_relaxed); }

std::int64_t maxWaitSliceMs() noexcept { return g_maxWaitSliceMs.load(std::memory_order_relaxed); }

void setMaxWaitSliceMsForTesting(std::int64_t milliseconds) noexcept {
    g_maxWaitSliceMs.store(std::clamp<std::int64_t>(milliseconds, 1, kDefaultMaxWaitSliceMs),
                           std::memory_order_relaxed);
}

std::int64_t waitSliceMs(IClock::Duration left) noexcept {
    if (left <= IClock::Duration::zero()) {
        return 0;
    }
    return std::min<std::int64_t>(std::chrono::ceil<std::chrono::milliseconds>(left).count(), maxWaitSliceMs());
}

std::uint32_t currentProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

std::string atomicTempName(std::string_view targetName, std::uint64_t id) {
    constexpr std::size_t kMaxNameBytes = 64;
    const std::string_view stem = targetName.substr(0, utf8Prefix(targetName, kMaxNameBytes));
    return std::format(".{}{}{}-{}", stem, kAtomicTempMarker, currentProcessId(), id);
}

Error isADirectoryError(std::string_view operation, std::string_view path) {
    return Error(ErrorCode::InvalidArgument, std::format("{} '{}': is a directory", operation, path));
}

Error notADirectoryError(std::string_view operation, std::string_view path) {
    return Error(ErrorCode::InvalidArgument, std::format("{} '{}': not a directory", operation, path));
}

bool isBatchFileName(std::string_view path) noexcept {
    while (!path.empty() && (path.back() == '.' || path.back() == ' ')) {
        path.remove_suffix(1);
    }
    if (path.size() < 4) {
        return false;
    }
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    std::string extension(path.substr(path.size() - 4));
    std::ranges::transform(extension, extension.begin(), lower);
    return extension == ".bat" || extension == ".cmd";
}

std::optional<AclFloor> accessAclFloor(std::string_view xattr) noexcept {
    constexpr std::size_t kHeaderBytes = 4;
    constexpr std::size_t kEntryBytes = 8;
    constexpr std::uint32_t kVersion = 2;
    // Tags (linux/posix_acl.h, acl(5)).
    constexpr std::uint32_t kUserObj = 0x01;
    constexpr std::uint32_t kUser = 0x02;
    constexpr std::uint32_t kGroupObj = 0x04;
    constexpr std::uint32_t kGroup = 0x08;
    constexpr std::uint32_t kMask = 0x10;
    constexpr std::uint32_t kOther = 0x20;
    const auto byteAt = [xattr](std::size_t at) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(xattr[at]));
    };
    const auto le16 = [&byteAt](std::size_t at) { return byteAt(at) | (byteAt(at + 1) << 8); };
    if (xattr.size() < kHeaderBytes || (xattr.size() - kHeaderBytes) % kEntryBytes != 0 ||
        (le16(0) | (le16(2) << 16)) != kVersion) {
        return std::nullopt;
    }
    std::optional<std::uint32_t> groupObj;
    std::optional<std::uint32_t> other;
    std::uint32_t mask = 7; // Without a mask entry (no named entries) nothing is limited.
    // The AND of every named-user (named-group) entry; nullopt while there is none.
    std::optional<std::uint32_t> namedUsers;
    std::optional<std::uint32_t> namedGroups;
    for (std::size_t at = kHeaderBytes; at < xattr.size(); at += kEntryBytes) {
        const std::uint32_t permissions = le16(at + 2) & 7u;
        switch (le16(at)) {
        case kUserObj: break; // The owner's entry, which the mask does not limit.
        case kUser: namedUsers = namedUsers.value_or(7) & permissions; break;
        case kGroupObj: groupObj = permissions; break;
        case kGroup: namedGroups = namedGroups.value_or(7) & permissions; break;
        case kMask: mask = permissions; break;
        case kOther: other = permissions; break;
        default: return std::nullopt;
        }
    }
    if (!groupObj || !other) {
        return std::nullopt;
    }
    /*
     * The mask limits the whole group class: the owning-group entry and every named
     * entry. The other entry is not in that class and is never masked (acl(5)), so a
     * mask without named entries (`setfacl -m m::r`) limits only the owning group.
     */
    const std::uint32_t users = namedUsers ? *namedUsers & mask : 7;
    const std::uint32_t groups = namedGroups ? *namedGroups & mask : 7;
    return AclFloor{*groupObj & mask & users, *other & users & groups};
}

std::uint32_t replacementMode(std::uint32_t mode, bool ownerKept, bool groupKept,
                              std::optional<AclFloor> acl) noexcept {
    constexpr std::uint32_t kSetUid = 04000;
    constexpr std::uint32_t kSetGid = 02000;
    std::uint32_t special = mode & 07000;
    const std::uint32_t owner = (mode >> 6) & 7u;
    std::uint32_t group = (mode >> 3) & 7u;
    std::uint32_t other = mode & 7u;
    if (acl) {
        group &= acl->group;
        other &= acl->other;
    }
    if (!groupKept) {
        special &= ~kSetGid;
        group &= other;
        other = group;
    }
    if (!ownerKept) {
        special &= ~kSetUid;
        group &= owner;
        other &= owner;
    }
    return special | (owner << 6) | (group << 3) | other;
}

} // namespace frost::io::detail
