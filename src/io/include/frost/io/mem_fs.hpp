// SPDX-License-Identifier: Apache-2.0
//
// MemFs: the deterministic IFileSystem double (PLAN.md section 6.2). An
// in-memory tree whose modification times come from an IClock (normally a
// ManualClock) and that can be told to fail specific operations.
//
/*
 * Semantics follow the real implementations closely enough that code tested
 * against MemFs behaves the same on disk:
 *   - Paths must be absolute under the MemFs's PathStyle (native by default)
 *     and are normalised with normalizePath, so "C:/a" and "C:\a" are the same
 *     file. Roots ("/", "C:\", "\\server\share\") always exist. Names are
 *     case-sensitive in both styles. TODO(M2): optional case-insensitive
 *     lookups for Windows-style trees, matching NTFS.
 *   - Errors use the same codes and refinements as file_system.hpp.
 *   - writeFileAtomic runs the same protocol as the real one: it creates a
 *     visible ".<name>.frost-tmp-*" entry, fills it, then renames it over the
 *     target, so faults can be injected at either step (FsOp::Write fails
 *     before the temp file exists, FsOp::AtomicRename after it has been
 *     written). Either way the target keeps its old content and the temp
 *     entry is removed.
 *   - Every successful mutation stamps the touched entry (and, for creations
 *     and removals, the parent directory) with clock.systemNow().
 *   - Symlinks are not modelled.
 *
 * Fault injection. injectFault(op, path, code, count) makes the next `count`
 * operations of kind `op` on exactly `path` (after normalisation) fail with
 * `code` before they change anything. Faults are consumed in the order they
 * were injected; an empty path matches every path.
 *
 * Thread safety: every member function may be called concurrently; one mutex
 * guards the whole tree (contention is irrelevant for a test double). The
 * clock must outlive the MemFs.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {

// The operations faults can target. AtomicRename is the second step of
// writeFileAtomic (temp file written, about to replace the target).
enum class FsOp : unsigned char {
    Read,
    Write,
    AtomicRename,
    Status,
    List,
    CreateDirectories,
    RemoveFile,
    RemoveEmptyDirectory, // removeDirectory (named so it cannot collide with the windows.h RemoveDirectory macro)
    Exists,
};

class MemFs final : public IFileSystem {
public:
    explicit MemFs(const IClock& clock, PathStyle style = kNativePathStyle);

    Result<std::string> readFile(std::string_view path) override;
    Result<void> writeFileAtomic(std::string_view path, std::string_view data) override;
    Result<FileStatus> status(std::string_view path) override;
    Result<std::vector<DirEntry>> listDirectory(std::string_view path) override;
    Result<void> createDirectories(std::string_view path) override;
    Result<void> removeFile(std::string_view path) override;
    Result<void> removeDirectory(std::string_view path) override;
    Result<bool> exists(std::string_view path) override;

    // Makes the next `count` `op` operations on `path` fail with `code` (see above).
    // `path` must be absolute (or empty for "any path"); a relative path is a contract violation.
    void injectFault(FsOp op, std::string_view path, ErrorCode code, std::uint32_t count = 1);

    // Number of injected faults not yet triggered.
    [[nodiscard]] std::size_t pendingFaults() const;

    // Every normalised path in the tree (files and directories, roots excluded),
    // sorted. For assertions such as "no temp files are left".
    [[nodiscard]] std::vector<std::string> allPaths() const;

private:
    struct Node {
        FileType type = FileType::Regular;
        std::string content;
        IClock::SystemTime mtime{};
    };

    struct Fault {
        FsOp op;
        std::string path; // Normalised; empty matches any path.
        ErrorCode code;
        std::uint32_t remaining;
    };

    // Normalises `path` under m_style; InvalidArgument for relative paths.
    [[nodiscard]] Result<std::string> normalize(std::string_view path) const;
    // Consumes a matching fault and returns its error, if any.
    [[nodiscard]] Result<void> takeFault(FsOp op, const std::string& path, std::string_view original)
        FROST_REQUIRES(m_mutex);
    // The node at `path`, or nullptr. Roots are reported as directories through isRoot().
    [[nodiscard]] Node* find(const std::string& path) FROST_REQUIRES(m_mutex);
    [[nodiscard]] bool isRoot(std::string_view path) const noexcept;
    // Whether `path` names an existing directory (roots included).
    [[nodiscard]] bool isDirectory(const std::string& path) FROST_REQUIRES(m_mutex);
    // Stamps the parent directory of `path` (if it is not a root) with the current time.
    void touchParent(std::string_view path) FROST_REQUIRES(m_mutex);

    const IClock& m_clock;
    const PathStyle m_style;
    mutable Mutex m_mutex;
    // Keyed by normalised path; std::map keeps listings sorted and deterministic.
    std::map<std::string, Node, std::less<>> m_nodes FROST_GUARDED_BY(m_mutex);
    std::deque<Fault> m_faults FROST_GUARDED_BY(m_mutex);
};

} // namespace frost::io
