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

} // namespace frost::io::detail
