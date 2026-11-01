// SPDX-License-Identifier: Apache-2.0
//
// ScratchDir: a uniquely named temporary directory for one test, created under
// the system temp directory and removed (with everything in it) on destruction.
//
/*
 * The name contains a tag, the process id and a per-process counter, so tests
 * running concurrently (ctest -j, several worktrees, Windows and WSL at once)
 * never share or delete each other's directories. Creation and removal go
 * through frost::io's real file system, so trees deeper than MAX_PATH are
 * handled on Windows. Header-only; test code only.
 */
#pragma once

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace frost::test {

/*
 * Removes `path` and everything below it; missing paths are fine. Returns false on
 * failure. A link at `path` or below it (a symlink, or a Windows junction) is removed
 * itself and never followed, so nothing outside the tree is touched.
 */
inline bool removeTree(io::IFileSystem& fs, const std::string& path) {
    // removeFile never follows a link; only a real directory is refused ("is a directory").
    auto removed = fs.removeFile(path);
    if (removed || removed.error().code() == ErrorCode::NotFound) {
        return true;
    }
    if (removed.error().code() != ErrorCode::InvalidArgument) {
        return false;
    }
    auto entries = fs.listDirectory(path);
    if (!entries) {
        return entries.error().code() == ErrorCode::NotFound;
    }
    bool ok = true;
    for (const io::DirEntry& entry : *entries) {
        auto child = io::joinPath(path, entry.name);
        if (!child) {
            return false;
        }
        if (entry.type == io::FileType::Directory) {
            ok = removeTree(fs, *child) && ok;
        } else {
            ok = fs.removeFile(*child).has_value() && ok;
        }
    }
    return fs.removeDirectory(path).has_value() && ok;
}

class ScratchDir {
public:
    // Creates "<temp>/frost-<tag>-<pid>-<n>"; `tag` may contain any characters valid in a file name.
    // On failure records a test failure (ADD_FAILURE); path() is empty if no name could be formed
    // (no usable temp directory, or joinPath rejected `tag`), otherwise it names the uncreated directory.
    explicit ScratchDir(std::string_view tag) {
        // Per-process sequence for directory names; only uniqueness matters, so no ordering is
        // needed (a relaxed fetch_add would do).
        static std::atomic<unsigned> counter{0};
        std::error_code error;
        const std::filesystem::path temp = std::filesystem::temp_directory_path(error);
        auto root = io::fromStdPath(temp);
        auto normalized = root ? io::normalizePath(*root) : root;
        if (error || !normalized) {
            ADD_FAILURE() << "no usable temp directory";
            return;
        }
        auto path = io::joinPath(*normalized, std::format("frost-{}-{}-{}", tag, processId(), counter++));
        if (!path) {
            ADD_FAILURE() << path.error().describe();
            return;
        }
        m_path = std::move(*path);
        removeTree(io::realFileSystem(), m_path); // A leftover from a crashed run with a recycled pid.
        if (auto created = io::realFileSystem().createDirectories(m_path); !created) {
            ADD_FAILURE() << created.error().describe();
        }
    }

    ~ScratchDir() {
        if (!m_path.empty() && !removeTree(io::realFileSystem(), m_path)) {
            ADD_FAILURE() << "could not remove scratch directory " << m_path;
        }
    }

    ScratchDir(const ScratchDir&) = delete ("owns the directory");
    ScratchDir& operator=(const ScratchDir&) = delete ("owns the directory");

    // The absolute, normalised UTF-8 path of the directory.
    [[nodiscard]] const std::string& path() const noexcept { return m_path; }

    // path() joined with a relative `name`; records a test failure and returns "" if joinPath
    // rejects `name` (absolute, empty or containing a NUL).
    [[nodiscard]] std::string join(std::string_view name) const {
        auto joined = io::joinPath(m_path, name);
        if (!joined) {
            ADD_FAILURE() << joined.error().describe();
            return {};
        }
        return std::move(*joined);
    }

private:
    static std::uint32_t processId() {
#ifdef _WIN32
        return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
        return static_cast<std::uint32_t>(getpid());
#endif
    }

    std::string m_path;
};

} // namespace frost::test
