// SPDX-License-Identifier: Apache-2.0
//
// The platform-independent parts of frost::io's file-system seam: IFileSystem::safeWrite's default
// for test doubles, and the temp-file name parser and stale temp-file sweep (ADR 0028).

#include "frost/io/file_system.hpp"

#include "frost/io/path.hpp"

#include "internal.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace frost::io {
namespace {

// Parses all of `digits` as a decimal number of type T; nullopt for empty input, other characters,
// leading '+' or '-', and overflow.
template<class T>
std::optional<T> parseDecimal(std::string_view digits) noexcept {
    if (digits.empty()) {
        return std::nullopt;
    }
    T value{};
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc{} || end != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

Result<void> IFileSystem::writeFileAtomicWithMtime(std::string_view, std::string_view,
                                                   std::filesystem::file_time_type) {
    return fail(ErrorCode::Unsupported, "Atomic timestamped writes are not supported by this file system");
}

Result<void> IFileSystem::rename(std::string_view, std::string_view) {
    return fail(ErrorCode::Unsupported, "Rename is not supported by this file system");
}

Result<void> IFileSystem::remove(std::string_view path) {
    auto info = status(path);
    if (!info) {
        return std::unexpected(std::move(info.error()));
    }
    if (info->type == FileType::Directory) {
        return removeDirectory(path);
    }
    return removeFile(path);
}

Result<void> IFileSystem::removeAll(std::string_view path) {
    auto info = status(path);
    if (!info) {
        return std::unexpected(std::move(info.error()));
    }
    if (info->type != FileType::Directory) {
        return removeFile(path);
    }
    auto entries = listDirectory(path);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }
    for (const DirEntry& entry : *entries) {
        auto child = joinPath(path, entry.name);
        if (!child) {
            return std::unexpected(std::move(child.error()));
        }
        if (entry.type == FileType::Directory) {
            if (auto removed = removeAll(*child); !removed) {
                return removed;
            }
        } else {
            if (auto removed = removeFile(*child); !removed) {
                return removed;
            }
        }
    }
    return removeDirectory(path);
}

Result<std::string> IFileSystem::canonicalPath(std::string_view path) {
    auto normalized = normalizePath(path);
    if (!normalized) {
        return normalized;
    }
    auto found = exists(*normalized);
    if (!found) {
        return std::unexpected(std::move(found.error()));
    }
    if (!*found) {
        return fail(ErrorCode::NotFound, std::string("no such file or directory: ").append(*normalized));
    }
    return normalized;
}

Result<SafeWriteResult> IFileSystem::safeWrite(std::string_view path, std::string_view data,
                                               const SafeWriteOptions& /*options*/) {
    auto existed = exists(path);
    if (!existed) {
        return std::unexpected(std::move(existed.error()));
    }
    if (auto written = writeFileAtomic(path, data); !written) {
        return std::unexpected(std::move(written.error()));
    }
    return SafeWriteResult{std::string(path), *existed ? SafeWriteMethod::Replaced : SafeWriteMethod::Created};
}

std::optional<AtomicTempName> parseAtomicTempName(std::string_view name) noexcept {
    // ".<stem>.frost-tmp-<pid>-<n>": the stem may itself contain the marker, so the last one counts.
    const std::size_t marker = name.rfind(detail::kAtomicTempMarker);
    if (name.empty() || name.front() != '.' || marker == std::string_view::npos || marker < 2) {
        return std::nullopt;
    }
    const std::string_view numbers = name.substr(marker + detail::kAtomicTempMarker.size());
    const std::size_t dash = numbers.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto pid = parseDecimal<std::uint32_t>(numbers.substr(0, dash));
    const auto id = parseDecimal<std::uint64_t>(numbers.substr(dash + 1));
    if (!pid || !id) {
        return std::nullopt;
    }
    return AtomicTempName{*pid, *id};
}

Result<std::size_t> removeStaleTempFiles(IFileSystem& fs, std::string_view directory, IClock::SystemTime olderThan,
                                         PathStyle style) {
    auto entries = fs.listDirectory(directory);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }
    const std::uint32_t self = detail::currentProcessId();
    std::size_t removed = 0;
    for (const DirEntry& entry : *entries) {
        const auto parsed = parseAtomicTempName(entry.name);
        if (entry.type != FileType::Regular || !parsed || parsed->processId == self) {
            continue;
        }
        auto child = joinPath(directory, entry.name, style);
        if (!child) {
            continue;
        }
        // Best effort: an entry that vanished, changed type or is recent stays (or is gone already).
        auto info = fs.status(*child);
        if (!info || info->type != FileType::Regular || info->mtime >= olderThan) {
            continue;
        }
        if (fs.removeFile(*child)) {
            ++removed;
        }
    }
    return removed;
}

} // namespace frost::io
