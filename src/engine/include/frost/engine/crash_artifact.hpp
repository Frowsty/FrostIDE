// SPDX-License-Identifier: Apache-2.0
// Bounded local observation of real minidumps from a previously authenticated engine process.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/engine/crash_artifact_source.hpp"
#include "frost/io/file_system.hpp"

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::engine {

// Immutable inspected bytes, not a promise about the original path remaining unchanged. This
// correlates a local dump with a previously authenticated process, not its crash-time project.
class ValidatedCrashArtifact {
public:
    ValidatedCrashArtifact(const ValidatedCrashArtifact&) = delete;
    ValidatedCrashArtifact& operator=(const ValidatedCrashArtifact&) = delete;
    [[nodiscard]] const CrashArtifactSource& source() const noexcept { return m_source; }
    [[nodiscard]] std::string_view dumpBytes() const noexcept { return m_dump; }
    [[nodiscard]] std::string_view sha256() const noexcept { return m_sha256; }
    [[nodiscard]] std::string_view reportDirectory() const noexcept { return m_reportDirectory; }
    [[nodiscard]] std::int64_t crashUnixMs() const noexcept { return m_crashUnixMs; }
    // Writes an atomic, byte-verified snapshot into an existing IDE-owned cache directory. Rejects
    // symlink/reparse traversal. Only the returned cache path should be passed to the debugger.
    [[nodiscard]] Result<std::string> materialize(io::IFileSystem& fs, std::string_view cacheDirectory) const;

private:
    friend class CrashArtifactObserver;
    ValidatedCrashArtifact() = default;
    CrashArtifactSource m_source;
    std::string m_dump, m_sha256, m_reportDirectory;
    std::int64_t m_crashUnixMs = 0;
};
struct CrashArtifactScan {
    std::shared_ptr<const ValidatedCrashArtifact> artifact;
    std::uint32_t partialReports = 0, rejectedReports = 0;
};

// One owner-thread observer per live pairing. Capture while the exact authenticated session is
// still alive, before scheduling any unexpected-loss observation. Explicit disconnect, project
// change or trust revocation must discard it. Poll only after unexpected loss/actual owned exit;
// the caller bounds retry duration and rechecks captured physical-project trust before each poll
// and before Open. The clock must outlive the observer. Missing/partial artifacts remain pending. Reports predating
// capture are ignored.
class CrashArtifactObserver {
public:
    static constexpr std::size_t MaxReportBytes = 64 * 1024;
    static constexpr std::size_t MaxDumpBytes = 256 * 1024 * 1024;
    [[nodiscard]] static Result<std::unique_ptr<CrashArtifactObserver>>
    capture(io::IFileSystem& fs, CrashArtifactSource source, IClock& clock);
    [[nodiscard]] Result<CrashArtifactScan> poll(io::IFileSystem& fs, std::stop_token stop = {});
    [[nodiscard]] const CrashArtifactSource& source() const noexcept { return m_source; }

private:
    CrashArtifactObserver() = default;
    CrashArtifactSource m_source;
    std::vector<std::string> m_baseline;
    IClock* m_clock = nullptr;
    std::string m_root;
    std::int64_t m_capturedUnixMs = 0;
    bool m_delivered = false;
};
// Worker-side preflight before creating a private child in an existing IDE-owned cache base.
[[nodiscard]] Result<void> validateCrashCacheDirectory(io::IFileSystem& fs, std::string_view existingDirectory);
// Bounds-checks actual Windows minidump directories and referenced blobs, including the dump's
// own process ID. Useful independently of discovery; does not treat arbitrary bytes as a dump.
[[nodiscard]] Result<void> validateCrashMinidump(std::string_view bytes, std::int64_t expectedProcessId);
} // namespace frost::engine
