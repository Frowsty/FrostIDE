// SPDX-License-Identifier: Apache-2.0
//
// TrustStore: the folders the user trusts to run project code (PLAN.md section 3.5, the W1 part of
// the trust model; ADR 0032). Builds run MSBuild targets from the project, so a build checks
// here first (checkBuildTrust); the trust prompt in the UI comes in a later slice.
//
/*
 * Semantics. A root is an absolute folder. Trusting a root covers everything below it; untrust
 * removes that root only (a trusted parent still covers the folder). Paths are compared after
 * normalisation (io::normalizePath), component by component, with ASCII case ignored on Windows.
 * On POSIX a path with a ".." component is never trusted and cannot be trusted: a symlink makes
 * lexical ".." resolution unsound there (io/path.hpp). TODO(M2): resolve symlinks (realpath, final
 * paths) before comparing, and fold non-ASCII case on Windows.
 *
 * Storage. One small UTF-8 text file in the user data directory (defaultStorePath): a header line
 * "# FrostIDE trusted roots v1", then one root per line. It is written with
 * IFileSystem::writeFileAtomic (old or new content after a crash, never a mixture), and trust() and
 * untrust() re-read it first, so roots another FrostIDE instance added meanwhile are kept (two
 * instances writing at the same instant can still lose one change; TODO(M2): single instance or a
 * lock). PLAN.md 3.5 keeps trust in application settings; this file moves there with M2's settings.
 * A file whose header names a newer version is Unsupported and never rewritten; one without the
 * header is Corrupt. Lines that are not absolute paths are skipped with a warning.
 *
 * Thread safety: every member may be called from any thread; one mutex guards the root list.
 * The file system must outlive the store.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::project {

class SolutionModel;

struct TrustSnapshot {
    bool trusted = false;
    std::uint64_t revision = 0;
};

class TrustStore {
public:
    /*
     * Opens the store kept in `storePath` (absolute; the file need not exist yet, it is created by
     * the first trust()). Errors: InvalidArgument (storePath), Corrupt or Unsupported (above),
     * readFile's errors other than NotFound.
     */
    [[nodiscard]] static Result<std::unique_ptr<TrustStore>> open(io::IFileSystem& fs, std::string_view storePath);

    // %LOCALAPPDATA%\FrostIDE\data\trusted-roots.txt on Windows; on Linux
    // $XDG_DATA_HOME/FrostIDE/trusted-roots.txt, else ~/.local/share/FrostIDE/trusted-roots.txt.
    // Errors: NotFound when the variables are unset or not absolute.
    [[nodiscard]] static Result<std::string> defaultStorePath();

    // True when `path` (absolute) is a trusted root or lies below one; false for invalid paths.
    [[nodiscard]] bool isTrusted(std::string_view path) const;
    // Atomic trust decision and in-process revision; any observed root-list change advances it.
    [[nodiscard]] TrustSnapshot snapshot(std::string_view path) const;

    // Success when isTrusted(path); otherwise PermissionDenied naming the path, for gates such as
    // the build driver's.
    [[nodiscard]] Result<void> requireTrusted(std::string_view path) const;

    // Adds `root` (absolute) and saves. Trusting the same root twice saves nothing; a root below a
    // trusted one is still added, so it stays trusted if the parent is untrusted later. Errors:
    // InvalidArgument (relative, NUL, line breaks, POSIX ".."), the file errors of open,
    // createDirectories and writeFileAtomic (the list is unchanged then).
    [[nodiscard]] Result<void> trust(std::string_view root);

    // Removes `root` if it is in the list (nothing happens otherwise) and saves. Errors: as trust.
    [[nodiscard]] Result<void> untrust(std::string_view root);

    // The trusted roots, normalised, in the order they were added.
    [[nodiscard]] std::vector<std::string> roots() const;
    [[nodiscard]] const std::string& storePath() const noexcept { return m_storePath; }
    // Lines skipped by the last read, for the log.
    [[nodiscard]] std::vector<std::string> warnings() const;

    TrustStore(const TrustStore&) = delete ("owns its mutex");
    TrustStore& operator=(const TrustStore&) = delete ("owns its mutex");
    ~TrustStore() = default;

private:
    TrustStore(io::IFileSystem& fs, std::string storePath) : m_fs(fs), m_storePath(std::move(storePath)) {}
    // Reads the file into m_roots and m_warnings.
    [[nodiscard]] Result<void> reload() FROST_REQUIRES(m_mutex);
    [[nodiscard]] Result<void> save(const std::vector<std::string>& roots) FROST_REQUIRES(m_mutex);

    io::IFileSystem& m_fs;
    const std::string m_storePath;
    mutable Mutex m_mutex;
    std::vector<std::string> m_roots FROST_GUARDED_BY(m_mutex);
    std::vector<std::string> m_warnings FROST_GUARDED_BY(m_mutex);
    std::uint64_t m_revision FROST_GUARDED_BY(m_mutex) = 0;
};

// The folders a build of `model` runs project code from: the solution's directory and the
// directory of every loaded project outside it, without duplicates.
[[nodiscard]] std::vector<std::string> buildTrustRoots(const SolutionModel& model);

// The build gate: success when every folder of buildTrustRoots(model) is trusted; otherwise
// PermissionDenied naming the first untrusted folder (the UI then offers to trust it).
[[nodiscard]] Result<void> checkBuildTrust(const TrustStore& store, const SolutionModel& model);

} // namespace frost::project
