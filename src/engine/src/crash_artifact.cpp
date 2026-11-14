// SPDX-License-Identifier: Apache-2.0
// Bounded local crash recovery with physical-path checks and immutable minidump snapshots.
#include "frost/engine/crash_artifact.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>

#include <sodium.h>

namespace frost::engine {
namespace {
constexpr std::size_t kMaxEntries = 256;
constexpr std::size_t kMaxAncestorEntries = 4096;
bool sameSpelling(std::string_view left, std::string_view right) {
#ifdef _WIN32
    const auto fold = [](unsigned char value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; };
    return std::ranges::equal(left, right, [&](unsigned char a, unsigned char b) { return fold(a) == fold(b); });
#else
    return left == right;
#endif
}
Result<std::string> digest(std::string_view bytes) {
    if (sodium_init() < 0)
        return fail(ErrorCode::Internal, "artifact hashing unavailable");
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    std::array<char, 65> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), hash.data(), hash.size());
    return std::string(encoded.data());
}
Result<std::string> physical(io::IFileSystem& fs, std::string_view input, io::FileType expected) {
    // Includes cache preflight: reject network/device namespaces before any filesystem call.
    if (input.starts_with("\\\\") || input.starts_with("//") || input.starts_with("\\/") || input.starts_with("/\\"))
        return fail(ErrorCode::PermissionDenied, "artifact paths must be local");
    auto path = io::normalizePath(input);
    if (!path)
        return std::unexpected(path.error());
    if (path->starts_with("\\\\") || path->starts_with("//"))
        return fail(ErrorCode::PermissionDenied, "artifact paths must be local");
    auto current = *path;
    // The file-system seam exposes links through parent listings (status deliberately follows them).
    for (unsigned depth = 0; depth < 256; ++depth) {
        const auto parent = io::parentPath(current);
        if (parent.empty())
            break;
        auto entries = fs.listDirectory(parent);
        if (!entries)
            return std::unexpected(entries.error());
        if (entries->size() > kMaxAncestorEntries)
            return fail(ErrorCode::OutOfRange, "artifact ancestor listing exceeds its limit");
        const auto name = io::fileName(current);
        auto found = std::ranges::find_if(*entries, [&](const io::DirEntry& entry) { return entry.name == name; });
#ifdef _WIN32
        // Windows canonicalization resolves actual component case. Preserve exact entries first,
        // including case-sensitive directories; accept only an unambiguous ASCII-case alias.
        if (found == entries->end()) {
            for (auto entry = entries->begin(); entry != entries->end(); ++entry) {
                if (!sameSpelling(entry->name, name))
                    continue;
                if (found != entries->end())
                    return fail(ErrorCode::PermissionDenied, "ambiguous artifact path spelling");
                found = entry;
            }
        }
#endif
        if (found == entries->end())
            return fail(ErrorCode::NotFound, "artifact path component disappeared");
        if (found->type == io::FileType::Symlink)
            return fail(ErrorCode::PermissionDenied, "artifact paths cannot traverse links or reparse points");
        current = std::string(parent);
        if (depth == 255)
            return fail(ErrorCode::OutOfRange, "artifact path exceeds its component limit");
    }
    auto canonical = fs.canonicalPath(*path);
    if (!canonical)
        return std::unexpected(canonical.error());
    auto normalized = io::normalizePath(*canonical);
    if (!normalized)
        return std::unexpected(normalized.error());
    if (!sameSpelling(*normalized, *path))
        return fail(ErrorCode::PermissionDenied, "artifact path is not its final physical spelling");
    auto status = fs.status(*normalized);
    if (!status)
        return std::unexpected(status.error());
    if (status->type != expected)
        return fail(ErrorCode::InvalidArgument, "artifact path has the wrong file type");
    return normalized;
}
Result<std::string> child(std::string_view root, std::string_view name) {
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string_view::npos ||
        name.find('\0') != std::string_view::npos)
        return fail(ErrorCode::InvalidArgument, "invalid crash report entry name");
    return io::joinPath(root, name);
}
Result<std::string> readStable(io::IFileSystem& fs, std::string_view path, std::size_t limit) {
    auto checked = physical(fs, path, io::FileType::Regular);
    if (!checked)
        return std::unexpected(checked.error());
    auto before = fs.status(*checked);
    if (!before)
        return std::unexpected(before.error());
    if (before->size == 0 || before->size > limit)
        return fail(ErrorCode::OutOfRange, "artifact file is empty or exceeds its size limit");
    auto bytes = fs.readFileSnapshot(*checked, limit);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto afterPath = physical(fs, *checked, io::FileType::Regular);
    if (!afterPath)
        return std::unexpected(afterPath.error());
    auto after = fs.status(*afterPath);
    if (!after)
        return std::unexpected(after.error());
    if (*before != *after || bytes->size() != after->size || bytes->size() > limit)
        return fail(ErrorCode::Busy, "artifact file changed during its bounded snapshot");
    return bytes;
}
std::uint32_t u32(std::string_view bytes, std::size_t offset) {
    std::uint32_t result = 0;
    for (unsigned index = 0; index < 4; ++index)
        result |= std::uint32_t(static_cast<unsigned char>(bytes[offset + index])) << (8 * index);
    return result;
}
std::uint64_t u64(std::string_view bytes, std::size_t offset) {
    return std::uint64_t(u32(bytes, offset)) | (std::uint64_t(u32(bytes, offset + 4)) << 32);
}
bool range(std::string_view bytes, std::uint64_t offset, std::uint64_t size) {
    return offset <= bytes.size() && size <= bytes.size() - offset;
}
bool location(std::string_view bytes, std::size_t offset) {
    return range(bytes, offset, 8) && range(bytes, u32(bytes, offset + 4), u32(bytes, offset));
}
bool dumpString(std::string_view bytes, std::uint32_t rva) {
    return range(bytes, rva, 4) && u32(bytes, rva) <= 65536 && u32(bytes, rva) % 2 == 0 &&
           range(bytes, std::uint64_t(rva) + 4, u32(bytes, rva));
}
bool reportName(std::string_view name, std::int64_t pid) {
    if (name.size() < 18 || name[8] != 'T' || name[15] != 'Z' || name[16] != '-')
        return false;
    for (std::size_t i = 0; i < 15; ++i)
        if (i != 8 && (name[i] < '0' || name[i] > '9'))
            return false;
    return name.substr(17) == std::to_string(pid);
}
} // namespace

Result<void> validateCrashMinidump(std::string_view bytes, std::int64_t expectedProcessId) {
    const auto invalid = [] { return fail(ErrorCode::Corrupt, "incomplete or invalid engine minidump"); };
    if (bytes.size() < 32 || bytes.size() > CrashArtifactObserver::MaxDumpBytes || expectedProcessId <= 0 ||
        expectedProcessId > std::numeric_limits<std::uint32_t>::max() || u32(bytes, 0) != 0x504d444d ||
        (u32(bytes, 4) & 0xffff) != 0xa793)
        return invalid();
    const std::uint32_t count = u32(bytes, 8), directory = u32(bytes, 12);
    if (count == 0 || count > 1024 || !range(bytes, directory, std::uint64_t(count) * 12))
        return invalid();
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> streams;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto entry = std::size_t(directory) + std::size_t(index) * 12;
        const auto type = u32(bytes, entry), size = u32(bytes, entry + 4), rva = u32(bytes, entry + 8);
        if (!range(bytes, rva, size) || !streams.emplace(type, std::pair{rva, size}).second)
            return invalid();
    }
    for (const auto required : {3U, 4U, 6U, 7U, 15U})
        if (!streams.contains(required))
            return invalid();
    const auto [misc, miscSize] = streams.at(15);
    if (miscSize < 12 || u32(bytes, misc) < 12 || u32(bytes, misc) > miscSize || !(u32(bytes, misc + 4) & 1) ||
        u32(bytes, misc + 8) != static_cast<std::uint32_t>(expectedProcessId))
        return invalid();
    const auto [system, systemSize] = streams.at(7);
    if (systemSize < 56 || (u32(bytes, system + 24) != 0 && !dumpString(bytes, u32(bytes, system + 24))))
        return invalid();
    const auto [exception, exceptionSize] = streams.at(6);
    if (exceptionSize < 168 || !location(bytes, exception + 160) || u32(bytes, exception + 160) == 0 ||
        u32(bytes, exception + 32) > 15)
        return invalid();
    const auto faultThread = u32(bytes, exception);
    const auto [threads, threadSize] = streams.at(3);
    if (threadSize < 4)
        return invalid();
    const auto threadCount = u32(bytes, threads);
    if (threadCount == 0 || threadCount > 65536 || std::uint64_t(threadCount) * 48 > threadSize - 4)
        return invalid();
    bool faultFound = false;
    for (std::uint32_t index = 0; index < threadCount; ++index) {
        const auto entry = std::size_t(threads) + 4 + std::size_t(index) * 48;
        faultFound |= u32(bytes, entry) == faultThread;
        if (!location(bytes, entry + 32) || !location(bytes, entry + 40))
            return invalid();
    }
    if (!faultFound || faultThread == 0)
        return invalid();
    const auto [modules, moduleSize] = streams.at(4);
    if (moduleSize < 4)
        return invalid();
    const auto moduleCount = u32(bytes, modules);
    if (moduleCount == 0 || moduleCount > 65536 || std::uint64_t(moduleCount) * 108 > moduleSize - 4)
        return invalid();
    for (std::uint32_t index = 0; index < moduleCount; ++index) {
        const auto entry = std::size_t(modules) + 4 + std::size_t(index) * 108;
        const auto name = u32(bytes, entry + 20);
        if (!dumpString(bytes, name) || !location(bytes, entry + 76) || !location(bytes, entry + 84))
            return invalid();
    }
    if (streams.contains(5)) {
        const auto [memory, memorySize] = streams.at(5);
        if (memorySize < 4)
            return invalid();
        const auto memories = u32(bytes, memory);
        if (memories > 65536 || std::uint64_t(memories) * 16 > memorySize - 4)
            return invalid();
        for (std::uint32_t i = 0; i < memories; ++i)
            if (!location(bytes, std::size_t(memory) + 4 + std::size_t(i) * 16 + 8))
                return invalid();
    }
    if (streams.contains(9)) {
        const auto [memory, memorySize] = streams.at(9);
        if (memorySize < 16)
            return invalid();
        const auto memories = u64(bytes, memory), base = u64(bytes, memory + 8);
        if (memories > 65536 || memories * 16 > memorySize - 16)
            return invalid();
        std::uint64_t total = 0;
        for (std::uint64_t i = 0; i < memories; ++i) {
            const auto size = u64(bytes, std::size_t(memory) + 16 + std::size_t(i) * 16 + 8);
            if (size > std::numeric_limits<std::uint64_t>::max() - total)
                return invalid();
            total += size;
        }
        if (!range(bytes, base, total))
            return invalid();
    }
    return {};
}

Result<std::unique_ptr<CrashArtifactObserver>>
CrashArtifactObserver::capture(io::IFileSystem& fs, CrashArtifactSource source, IClock& clock) {
    const auto descriptor = rpc::Json::object()
                                .set("protocolVersion", 1)
                                .set("sourceId", source.sourceId)
                                .set("rootPath", source.rootPath)
                                .set("processRole", source.processRole)
                                .set("instanceId", source.instanceId)
                                .set("generation", source.generation)
                                .set("projectPath", source.projectPath)
                                .set("processId", source.processId)
                                .set("processStartUnixMs", source.processStartUnixMs);
    auto validated = parseCrashArtifactSource(descriptor, source.instanceId, source.generation, source.projectPath,
                                              source.processId);
    if (!validated)
        return std::unexpected(validated.error());
    source = std::move(*validated);
    auto root = physical(fs, source.rootPath, io::FileType::Directory);
    if (!root)
        return std::unexpected(root.error());
    auto entries = fs.listDirectory(*root);
    if (!entries)
        return std::unexpected(entries.error());
    if (entries->size() > kMaxEntries)
        return fail(ErrorCode::OutOfRange, "crash source exceeds its report-count limit");
    auto observer = std::unique_ptr<CrashArtifactObserver>(new CrashArtifactObserver);
    observer->m_source = std::move(source);
    observer->m_root = *root;
    observer->m_clock = &clock;
    observer->m_capturedUnixMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock.systemNow().time_since_epoch()).count();
    for (const auto& entry : *entries)
        observer->m_baseline.push_back(entry.name);
    return observer;
}

Result<CrashArtifactScan> CrashArtifactObserver::poll(io::IFileSystem& fs, std::stop_token stop) {
    CrashArtifactScan result;
    if (m_delivered)
        return result;
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "crash artifact observation cancelled");
    auto root = physical(fs, m_root, io::FileType::Directory);
    if (!root)
        return std::unexpected(root.error());
    auto entries = fs.listDirectory(*root);
    if (!entries)
        return std::unexpected(entries.error());
    if (entries->size() > kMaxEntries)
        return fail(ErrorCode::OutOfRange, "crash source exceeds its report-count limit");
    std::uint64_t inspectedDumpBytes = 0;
    for (const auto& entry : *entries) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "crash artifact observation cancelled");
        if (std::ranges::find(m_baseline, entry.name) != m_baseline.end() ||
            !reportName(entry.name, m_source.processId))
            continue;
        if (entry.type != io::FileType::Directory) {
            ++result.rejectedReports;
            continue;
        }
        auto reportDir = child(m_root, entry.name);
        if (!reportDir) {
            ++result.rejectedReports;
            continue;
        }
        auto checked = physical(fs, *reportDir, io::FileType::Directory);
        if (!checked) {
            ++result.rejectedReports;
            continue;
        }
        auto reportFile = child(*checked, "crash.json"), dumpFile = child(*checked, "crash.dmp");
        if (!reportFile || !dumpFile) {
            ++result.rejectedReports;
            continue;
        }
        auto report = readStable(fs, *reportFile, MaxReportBytes);
        if (!report) {
            ++result.partialReports;
            continue;
        }
        auto json = rpc::parseJson(*report);
        if (!json || !json->isObject()) {
            ++result.partialReports;
            continue;
        }
        if (json->integerAt("ProcessId") != m_source.processId ||
            json->integerAt("StartUnixMs") != m_source.processStartUnixMs ||
            json->stringAt("ProcessRole") != m_source.processRole || json->integerAt("Version") != 1) {
            ++result.rejectedReports;
            continue;
        }
        const auto crash = json->integerAt("TimeUnixMs");
        if (!crash || *crash < m_source.processStartUnixMs || *crash < m_capturedUnixMs ||
            *crash >
                std::chrono::duration_cast<std::chrono::milliseconds>(m_clock->systemNow().time_since_epoch()).count() +
                    5000) {
            ++result.rejectedReports;
            continue;
        }
        auto dumpStatus = fs.status(*dumpFile);
        if (!dumpStatus) {
            ++result.partialReports;
            continue;
        }
        if (dumpStatus->size > MaxDumpBytes) {
            ++result.rejectedReports;
            continue;
        }
        if (dumpStatus->size > MaxDumpBytes - inspectedDumpBytes) {
            ++result.partialReports;
            break;
        }
        inspectedDumpBytes += dumpStatus->size;
        // Reserve this sampled size before reading. Concurrent growth must not turn several
        // candidates into more than the poll's cumulative byte budget.
        auto dump = readStable(fs, *dumpFile, static_cast<std::size_t>(dumpStatus->size));
        if (!dump || !validateCrashMinidump(*dump, m_source.processId)) {
            ++result.partialReports;
            continue;
        }
        auto hash = digest(*dump);
        if (!hash)
            return std::unexpected(hash.error());
        auto artifact = std::shared_ptr<ValidatedCrashArtifact>(new ValidatedCrashArtifact);
        artifact->m_source = m_source;
        artifact->m_dump = std::move(*dump);
        artifact->m_sha256 = std::move(*hash);
        artifact->m_reportDirectory = *checked;
        artifact->m_crashUnixMs = *crash;
        result.artifact = std::move(artifact);
        m_delivered = true;
        return result;
    }
    return result;
}

Result<void> validateCrashCacheDirectory(io::IFileSystem& fs, std::string_view existingDirectory) {
    auto checked = physical(fs, existingDirectory, io::FileType::Directory);
    if (!checked)
        return std::unexpected(checked.error());
    return {};
}

Result<std::string> ValidatedCrashArtifact::materialize(io::IFileSystem& fs, std::string_view cacheDirectory) const {
    auto root = physical(fs, cacheDirectory, io::FileType::Directory);
    if (!root)
        return std::unexpected(root.error());
    auto path = child(*root, m_sha256 + ".dmp");
    if (!path)
        return std::unexpected(path.error());
    auto existing = fs.exists(*path);
    if (!existing)
        return std::unexpected(existing.error());
    if (*existing) {
        auto bytes = readStable(fs, *path, CrashArtifactObserver::MaxDumpBytes);
        if (!bytes || *bytes != m_dump)
            return fail(ErrorCode::Corrupt, "cached dump does not match its immutable snapshot");
    } else {
        auto written = fs.writeFileAtomic(*path, m_dump);
        if (!written)
            return std::unexpected(written.error());
    }
    auto checked = physical(fs, *path, io::FileType::Regular);
    if (!checked)
        return std::unexpected(checked.error());
    auto copied = readStable(fs, *checked, CrashArtifactObserver::MaxDumpBytes);
    if (!copied)
        return std::unexpected(copied.error());
    if (*copied != m_dump)
        return fail(ErrorCode::Corrupt, "materialized dump bytes changed");
    auto hash = digest(*copied);
    if (!hash || *hash != m_sha256)
        return fail(ErrorCode::Corrupt, "materialized dump hash mismatch");
    return checked;
}
} // namespace frost::engine
