// SPDX-License-Identifier: Apache-2.0
//
// ResourceGovernor (resource_governor.hpp, FND-18): host CPU/RAM probing, per-process RSS
// sampling, governed clangd and build concurrency flags, heavy-leg serialization with an optional
// cross-process lock file, and RSS budget enforcement.

#include "frost/sched/resource_governor.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// psapi.h must follow windows.h; K32GetProcessMemoryInfo lives in kernel32.
#include <psapi.h>
#else
#include <sys/file.h>
#include <sys/sysinfo.h>

#include <cerrno>
#include <cstring>
#include <fstream>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace frost::sched {
namespace {

constexpr std::uintptr_t kInvalidOsHandle = ~static_cast<std::uintptr_t>(0);
constexpr std::uint64_t kBytesPerMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kBytesPerGiB = 1024ULL * 1024ULL * 1024ULL;
#ifdef _WIN32
constexpr std::uint64_t kClangdSharedReserveBytes = 2ULL * kBytesPerGiB;
#else
constexpr std::uint64_t kClangdSharedReserveBytes = kBytesPerGiB;
#endif
constexpr std::uint64_t kClangdWorkerEstimateBytes = 512ULL * kBytesPerMiB;

[[nodiscard]] constexpr std::size_t legIndex(HeavyLegKind kind) noexcept {
    switch (kind) {
    case HeavyLegKind::Build: return 0;
    case HeavyLegKind::BackgroundIndex: return 1;
    case HeavyLegKind::Inspection: return 2;
    }
    return 0;
}

#ifdef _WIN32

[[nodiscard]] std::wstring utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

[[nodiscard]] Result<std::uintptr_t> tryLockFile(std::string_view path) {
    if (path.empty()) {
        return kInvalidOsHandle;
    }
    const std::wstring wide = utf8ToWide(path);
    if (wide.empty()) {
        return fail(ErrorCode::InvalidArgument, "invalid lock file path");
    }
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return fail(ErrorCode::IoError, std::format("cannot open lock file {} (Win32 error {})", path, GetLastError()));
    }
    OVERLAPPED overlapped{};
    if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) == 0) {
        const DWORD err = GetLastError();
        CloseHandle(handle);
        if (err == ERROR_LOCK_VIOLATION || err == ERROR_IO_PENDING) {
            return fail(ErrorCode::Busy, std::format("lock file {} is held by another process", path));
        }
        return fail(ErrorCode::IoError, std::format("cannot lock {} (Win32 error {})", path, err));
    }
    return reinterpret_cast<std::uintptr_t>(handle);
}

void unlockAndCloseFile(std::uintptr_t osHandle) noexcept {
    if (osHandle == kInvalidOsHandle) {
        return;
    }
    const auto handle = reinterpret_cast<HANDLE>(osHandle);
    OVERLAPPED overlapped{};
    UnlockFileEx(handle, 0, 1, 0, &overlapped);
    CloseHandle(handle);
}

#else

[[nodiscard]] Result<std::uintptr_t> tryLockFile(std::string_view path) {
    if (path.empty()) {
        return kInvalidOsHandle;
    }
    const std::string pathStr(path);
    const int fd = ::open(pathStr.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return fail(ErrorCode::IoError, std::format("cannot open lock file {}: {}", path, std::strerror(errno)));
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        ::close(fd);
        if (err == EWOULDBLOCK || err == EAGAIN) {
            return fail(ErrorCode::Busy, std::format("lock file {} is held by another process", path));
        }
        return fail(ErrorCode::IoError, std::format("cannot lock {}: {}", path, std::strerror(err)));
    }
    return static_cast<std::uintptr_t>(fd);
}

void unlockAndCloseFile(std::uintptr_t osHandle) noexcept {
    if (osHandle == kInvalidOsHandle) {
        return;
    }
    const int fd = static_cast<int>(osHandle);
    (void)::flock(fd, LOCK_UN | LOCK_NB);
    ::close(fd);
}

[[nodiscard]] std::optional<std::uint64_t> parseProcKbLine(std::string_view line, std::string_view key) noexcept {
    if (!line.starts_with(key)) {
        return std::nullopt;
    }
    line.remove_prefix(key.size());
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    std::uint64_t kb = 0;
    const auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), kb);
    if (ec != std::errc{}) {
        return std::nullopt;
    }
    return kb * 1024ULL;
}

#endif

} // namespace

std::string_view toString(HeavyLegKind kind) noexcept {
    switch (kind) {
    case HeavyLegKind::Build: return "Build";
    case HeavyLegKind::BackgroundIndex: return "BackgroundIndex";
    case HeavyLegKind::Inspection: return "Inspection";
    }
    return "HeavyLegKind(?)";
}

HostResources probeHostResources() noexcept {
    HostResources out;
    const unsigned hw = std::thread::hardware_concurrency();
    out.logicalCores = hw > 0 ? hw : 1u;

#ifdef _WIN32
    const DWORD active = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (active > 0) {
        out.logicalCores = static_cast<unsigned>(active);
    }
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem) != 0) {
        out.totalPhysicalBytes = static_cast<std::uint64_t>(mem.ullTotalPhys);
        out.availablePhysicalBytes = static_cast<std::uint64_t>(mem.ullAvailPhys);
        out.totalCommitBytes = static_cast<std::uint64_t>(mem.ullTotalPageFile);
        out.availableCommitBytes = static_cast<std::uint64_t>(mem.ullAvailPageFile);
    }
#else
    const long nprocs = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs > 0) {
        out.logicalCores = static_cast<unsigned>(nprocs);
    }
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    std::optional<std::uint64_t> memFree;
    std::optional<std::uint64_t> buffers;
    std::optional<std::uint64_t> cached;
    std::optional<std::uint64_t> swapTotal;
    std::optional<std::uint64_t> swapFree;
    while (std::getline(meminfo, line)) {
        if (const auto memTotalVal = parseProcKbLine(line, "MemTotal:")) {
            out.totalPhysicalBytes = *memTotalVal;
        } else if (const auto memAvailVal = parseProcKbLine(line, "MemAvailable:")) {
            out.availablePhysicalBytes = *memAvailVal;
        } else if (const auto memFreeVal = parseProcKbLine(line, "MemFree:")) {
            memFree = memFreeVal;
        } else if (const auto buffersVal = parseProcKbLine(line, "Buffers:")) {
            buffers = buffersVal;
        } else if (const auto cachedVal = parseProcKbLine(line, "Cached:")) {
            cached = cachedVal;
        } else if (const auto swapTotalVal = parseProcKbLine(line, "SwapTotal:")) {
            swapTotal = swapTotalVal;
        } else if (const auto swapFreeVal = parseProcKbLine(line, "SwapFree:")) {
            swapFree = swapFreeVal;
        }
    }
    if (out.availablePhysicalBytes == 0 && memFree) {
        out.availablePhysicalBytes = *memFree + buffers.value_or(0) + cached.value_or(0);
    }
    out.totalCommitBytes = out.totalPhysicalBytes + swapTotal.value_or(0);
    out.availableCommitBytes = out.availablePhysicalBytes + swapFree.value_or(0);
    if (out.totalPhysicalBytes == 0) {
        struct sysinfo si{};
        if (::sysinfo(&si) == 0) {
            const auto unit = static_cast<std::uint64_t>(si.mem_unit > 0 ? si.mem_unit : 1);
            out.totalPhysicalBytes = static_cast<std::uint64_t>(si.totalram) * unit;
            out.availablePhysicalBytes = static_cast<std::uint64_t>(si.freeram + si.bufferram) * unit;
            out.totalCommitBytes = out.totalPhysicalBytes + static_cast<std::uint64_t>(si.totalswap) * unit;
            out.availableCommitBytes = out.availablePhysicalBytes + static_cast<std::uint64_t>(si.freeswap) * unit;
        }
    }
#endif
    return out;
}

std::uint64_t currentProcessRssBytes() noexcept {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) != 0) {
        return static_cast<std::uint64_t>(pmc.WorkingSetSize);
    }
    return 0;
#else
    auto selfRss = queryProcessRssBytes(static_cast<std::int64_t>(::getpid()));
    return selfRss.value_or(0);
#endif
}

namespace {
Result<std::uint64_t> queryProcessMemoryBytes(std::int64_t pid, bool peak) noexcept {
    if (pid <= 0) {
        return fail(ErrorCode::InvalidArgument, "process id must be positive");
    }
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    }
    if (process == nullptr) {
        const DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER) {
            return fail(ErrorCode::NotFound, std::format("process {} does not exist", pid));
        }
        if (err == ERROR_ACCESS_DENIED) {
            return fail(ErrorCode::PermissionDenied, std::format("access denied querying process {}", pid));
        }
        return fail(ErrorCode::IoError, std::format("OpenProcess({}) failed (Win32 error {})", pid, err));
    }
    DWORD exitCode = 0;
    if (GetExitCodeProcess(process, &exitCode) != 0 && exitCode != STILL_ACTIVE) {
        CloseHandle(process);
        return fail(ErrorCode::NotFound, std::format("process {} has already exited", pid));
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    const BOOL ok = K32GetProcessMemoryInfo(process, &pmc, sizeof(pmc));
    const DWORD err = ok != 0 ? 0 : GetLastError();
    CloseHandle(process);
    if (ok == 0) {
        return fail(ErrorCode::IoError, std::format("GetProcessMemoryInfo({}) failed (Win32 error {})", pid, err));
    }
    if (peak && pmc.PeakWorkingSetSize == 0) {
        return fail(ErrorCode::Unsupported, std::format("process {} OS peak RSS is unavailable", pid));
    }
    return static_cast<std::uint64_t>(peak ? pmc.PeakWorkingSetSize : pmc.WorkingSetSize);
#else
    const std::string statusPath = std::format("/proc/{}/status", pid);
    std::ifstream status(statusPath);
    if (status.is_open()) {
        std::string line;
        while (std::getline(status, line)) {
            std::string_view trimmed(line);
            while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
                trimmed.remove_prefix(1);
            }
            if (trimmed.starts_with("State:")) {
                trimmed.remove_prefix(std::string_view("State:").size());
                while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
                    trimmed.remove_prefix(1);
                }
                if (!trimmed.empty() && (trimmed.front() == 'Z' || trimmed.front() == 'X' || trimmed.front() == 'x')) {
                    return fail(ErrorCode::NotFound, std::format("process {} has already exited", pid));
                }
            } else if (const auto rss = parseProcKbLine(line, peak ? "VmHWM:" : "VmRSS:")) {
                if (peak && *rss == 0) {
                    return fail(ErrorCode::Unsupported, std::format("process {} OS peak RSS is unavailable", pid));
                }
                return *rss;
            }
        }
    }
    if (peak) {
        return fail(status.is_open() ? ErrorCode::Unsupported : ErrorCode::NotFound,
                    std::format("process {} OS peak RSS is unavailable", pid));
    }
    const std::string statmPath = std::format("/proc/{}/statm", pid);
    std::ifstream statm(statmPath);
    if (!statm.is_open()) {
        return fail(ErrorCode::NotFound, std::format("process {} does not exist", pid));
    }
    std::uint64_t totalPages = 0;
    std::uint64_t residentPages = 0;
    if (!(statm >> totalPages >> residentPages) || residentPages == 0) {
        return fail(ErrorCode::NotFound, std::format("process {} has no resident memory", pid));
    }
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    return residentPages * static_cast<std::uint64_t>(pageSize > 0 ? pageSize : 4096);
#endif
}
} // namespace

Result<std::uint64_t> queryProcessRssBytes(std::int64_t pid) noexcept { return queryProcessMemoryBytes(pid, false); }

Result<std::uint64_t> queryProcessPeakRssBytes(std::int64_t pid) noexcept { return queryProcessMemoryBytes(pid, true); }

// --- HeavyLegLease & ScopedLegActivity -----------------------------------------------------------

HeavyLegLease::HeavyLegLease(ResourceGovernor* governor, HeavyLegKind kind, std::uintptr_t osHandle) noexcept
    : m_governor(governor), m_kind(kind), m_osHandle(osHandle) {}

HeavyLegLease::HeavyLegLease(HeavyLegLease&& other) noexcept
    : m_governor(std::exchange(other.m_governor, nullptr)), m_kind(other.m_kind),
      m_osHandle(std::exchange(other.m_osHandle, kInvalidOsHandle)) {}

HeavyLegLease& HeavyLegLease::operator=(HeavyLegLease&& other) noexcept {
    if (this != &other) {
        release();
        m_governor = std::exchange(other.m_governor, nullptr);
        m_kind = other.m_kind;
        m_osHandle = std::exchange(other.m_osHandle, kInvalidOsHandle);
    }
    return *this;
}

HeavyLegLease::~HeavyLegLease() { release(); }

void HeavyLegLease::release() noexcept {
    if (ResourceGovernor* gov = std::exchange(m_governor, nullptr)) {
        const std::uintptr_t handle = std::exchange(m_osHandle, kInvalidOsHandle);
        gov->releaseHeavyLeg(m_kind, handle);
    }
}

ScopedLegActivity::ScopedLegActivity(ResourceGovernor* governor, HeavyLegKind kind) noexcept
    : m_governor(governor), m_kind(kind) {}

ScopedLegActivity::ScopedLegActivity(ScopedLegActivity&& other) noexcept
    : m_governor(std::exchange(other.m_governor, nullptr)), m_kind(other.m_kind) {}

ScopedLegActivity& ScopedLegActivity::operator=(ScopedLegActivity&& other) noexcept {
    if (this != &other) {
        release();
        m_governor = std::exchange(other.m_governor, nullptr);
        m_kind = other.m_kind;
    }
    return *this;
}

ScopedLegActivity::~ScopedLegActivity() { release(); }

void ScopedLegActivity::release() noexcept {
    if (ResourceGovernor* gov = std::exchange(m_governor, nullptr)) {
        gov->endLegActivity(m_kind);
    }
}

// --- ResourceGovernor ----------------------------------------------------------------------------

ResourceGovernor::ResourceGovernor(ResourceGovernorOptions options)
    : m_options(std::move(options)), m_hostOverride(m_options.hostOverride) {}

HostResources ResourceGovernor::hostResources() const noexcept {
    {
        LockGuard lock(m_mutex);
        if (m_hostOverride.has_value()) {
            return *m_hostOverride;
        }
    }
    return probeHostResources();
}

void ResourceGovernor::setHostResourcesForTesting(std::optional<HostResources> overrideResources) {
    LockGuard lock(m_mutex);
    m_hostOverride = overrideResources;
}

unsigned ResourceGovernor::clangdJobs(bool residentLimitRequired) const noexcept {
    const HostResources host = hostResources();
    const unsigned cores = std::max(1u, host.logicalCores);
    const unsigned maxJobs = std::max(1u, m_options.maxClangdJobs);
    const unsigned throttledMax = std::max(1u, m_options.throttledClangdJobs);

    std::uint64_t availBytes = host.availablePhysicalBytes;
    if (host.availableCommitBytes > 0 && (availBytes == 0 || host.availableCommitBytes < availBytes)) {
        availBytes = host.availableCommitBytes;
    }

    // Scale with cores / 2 and available RAM (2 threads per GiB of available RAM, i.e. 512 MiB/thread).
    const unsigned byCores = std::max(1u, cores / 2u);
    unsigned byRam = maxJobs;
    if (availBytes > 0) {
        if (availBytes < 1536ULL * kBytesPerMiB) {
            byRam = 1u;
        } else if (availBytes < 3ULL * kBytesPerGiB) {
            byRam = std::min(2u, throttledMax);
        } else {
            const auto halfGibUnits = availBytes / (512ULL * kBytesPerMiB);
            byRam = static_cast<unsigned>(std::clamp<std::uint64_t>(halfGibUnits, 1ULL, maxJobs));
        }
    }

    // Admission estimate for the whole process: shared index and foreground ASTs
    // need headroom alongside the workers. RSS sampling records actual usage;
    // a later decision cannot change an already running child's -j argument.
    std::uint64_t budgetBytes = m_options.clangdRssCapBytes;
    if (availBytes > 0) {
        budgetBytes = std::min(budgetBytes, availBytes);
#ifdef _WIN32
        if (residentLimitRequired) {
            // Residency is enforced before execution. Commit/host availability still budgets
            // shared memory and workers; unknown host availability retains the RSS estimate.
            budgetBytes = availBytes;
        }
#endif
    }
#ifndef _WIN32
    static_cast<void>(residentLimitRequired);
#endif
    const std::uint64_t workerBytes =
        budgetBytes > kClangdSharedReserveBytes ? budgetBytes - kClangdSharedReserveBytes : 0;
    const unsigned byBudget =
        static_cast<unsigned>(std::clamp<std::uint64_t>(workerBytes / kClangdWorkerEstimateBytes, 1ULL, maxJobs));
    unsigned jobs = std::clamp(std::min({byCores, byRam, byBudget}), 1u, maxJobs);

    bool heavyBuildOrInspectActive = false;
    bool overCap = false;
    {
        LockGuard lock(m_mutex);
        heavyBuildOrInspectActive = m_legActivityCount[legIndex(HeavyLegKind::Build)] > 0 ||
                                    m_legActivityCount[legIndex(HeavyLegKind::Inspection)] > 0 ||
                                    m_exclusiveLeg == HeavyLegKind::Build || m_exclusiveLeg == HeavyLegKind::Inspection;
        std::uint64_t currentCombined = 0;
        for (const auto& [pid, entry] : m_processRss) {
            if (entry.active) {
                currentCombined += entry.currentBytes;
            }
            if (entry.leg == HeavyLegKind::BackgroundIndex && entry.active &&
                entry.currentBytes > m_options.clangdRssCapBytes) {
                overCap = true;
            }
        }
        if (currentCombined > m_options.combinedRssCapBytes) {
            overCap = true;
        }
    }

    if (overCap) {
        return 1u;
    }
    if (heavyBuildOrInspectActive) {
        const unsigned throttledByCores = std::max(1u, cores / 4u);
        jobs = std::min(jobs, std::min(throttledMax, throttledByCores));
    }
    return std::max(1u, jobs);
}

std::vector<std::string> ResourceGovernor::clangdFlags() const {
    return {
        std::format("-j={}", clangdJobs()),
        std::format("--background-index-priority={}", backgroundIndexPriority()),
        std::string(pchStorageDisk() ? "--pch-storage=disk" : "--pch-storage=memory"),
    };
}

unsigned ResourceGovernor::buildJobs() const noexcept {
    const HostResources host = hostResources();
    const unsigned cores = std::max(1u, host.logicalCores);
    const unsigned maxJobs = std::max(1u, m_options.maxBuildJobs);

    std::uint64_t availBytes = host.availablePhysicalBytes;
    if (host.availableCommitBytes > 0 && (availBytes == 0 || host.availableCommitBytes < availBytes)) {
        availBytes = host.availableCommitBytes;
    }

    const unsigned byCores = std::max(1u, cores / 2u);
    unsigned byRam = maxJobs;
    if (availBytes > 0) {
        // 1 GiB per parallel compiler process so concurrent builds stay within the RAM budget.
        const auto gibUnits = availBytes / kBytesPerGiB;
        byRam = static_cast<unsigned>(std::clamp<std::uint64_t>(gibUnits, 1ULL, maxJobs));
    }

    unsigned jobs = std::clamp(std::min(byCores, byRam), 1u, maxJobs);

    bool otherHeavyLegActive = false;
    bool overCap = false;
    {
        LockGuard lock(m_mutex);
        otherHeavyLegActive = m_legActivityCount[legIndex(HeavyLegKind::BackgroundIndex)] > 0 ||
                              m_legActivityCount[legIndex(HeavyLegKind::Inspection)] > 0 ||
                              m_exclusiveLeg == HeavyLegKind::BackgroundIndex ||
                              m_exclusiveLeg == HeavyLegKind::Inspection;
        std::uint64_t currentCombined = 0;
        for (const auto& [pid, entry] : m_processRss) {
            if (entry.active) {
                currentCombined += entry.currentBytes;
            }
        }
        if (currentCombined > m_options.combinedRssCapBytes) {
            overCap = true;
        }
    }

    if (overCap) {
        return 1u;
    }
    if (otherHeavyLegActive) {
        const unsigned throttled = std::max(1u, std::min(cores / 4u, std::max(1u, maxJobs / 2u)));
        jobs = std::min(jobs, throttled);
    }
    return std::max(1u, jobs);
}

Result<HeavyLegLease> ResourceGovernor::tryAcquireHeavyLeg(HeavyLegKind kind) {
    LockGuard lock(m_mutex);
    if (m_exclusiveLeg.has_value()) {
        return fail(ErrorCode::Busy, std::format("heavy leg {} cannot start while {} is active", toString(kind),
                                                 toString(*m_exclusiveLeg)));
    }
    auto lockedFile = tryLockFile(m_options.lockFilePath);
    if (!lockedFile) {
        return std::unexpected(std::move(lockedFile.error()));
    }
    m_exclusiveLeg = kind;
    ++m_legActivityCount[legIndex(kind)];
    return HeavyLegLease(this, kind, *lockedFile);
}

Result<HeavyLegLease> ResourceGovernor::acquireHeavyLeg(HeavyLegKind kind, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    UniqueLock lock(m_mutex);
    for (;;) {
        if (!m_exclusiveLeg.has_value()) {
            auto lockedFile = tryLockFile(m_options.lockFilePath);
            if (lockedFile) {
                m_exclusiveLeg = kind;
                ++m_legActivityCount[legIndex(kind)];
                return HeavyLegLease(this, kind, *lockedFile);
            }
            if (lockedFile.error().code() != ErrorCode::Busy) {
                return std::unexpected(std::move(lockedFile.error()));
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            const std::string holder = m_exclusiveLeg ? std::string(toString(*m_exclusiveLeg)) : "external lock holder";
            return fail(ErrorCode::Timeout,
                        std::format("timed out waiting to acquire heavy leg {} (held by {})", toString(kind), holder));
        }
        const auto waitSlice = std::min<std::chrono::nanoseconds>(deadline - now, std::chrono::milliseconds(50));
        m_cv.wait_for(lock.native(), waitSlice);
    }
}

void ResourceGovernor::releaseHeavyLeg(HeavyLegKind kind, std::uintptr_t osHandle) noexcept {
    unlockAndCloseFile(osHandle);
    {
        LockGuard lock(m_mutex);
        const std::size_t idx = legIndex(kind);
        if (m_legActivityCount[idx] > 0) {
            --m_legActivityCount[idx];
        }
        if (m_exclusiveLeg == kind) {
            m_exclusiveLeg.reset();
        }
    }
    m_cv.notify_all();
}

ScopedLegActivity ResourceGovernor::beginLegActivity(HeavyLegKind kind) {
    {
        LockGuard lock(m_mutex);
        ++m_legActivityCount[legIndex(kind)];
    }
    return ScopedLegActivity(this, kind);
}

void ResourceGovernor::endLegActivity(HeavyLegKind kind) noexcept {
    {
        LockGuard lock(m_mutex);
        const std::size_t idx = legIndex(kind);
        if (m_legActivityCount[idx] > 0) {
            --m_legActivityCount[idx];
        }
    }
    m_cv.notify_all();
}

void ResourceGovernor::setLegActive(HeavyLegKind kind, bool active) {
    {
        LockGuard lock(m_mutex);
        const std::size_t idx = legIndex(kind);
        if (active) {
            if (m_legActivityCount[idx] == 0) {
                m_legActivityCount[idx] = 1;
            }
        } else {
            m_legActivityCount[idx] = (m_exclusiveLeg == kind) ? 1u : 0u;
        }
    }
    m_cv.notify_all();
}

bool ResourceGovernor::isLegActive(HeavyLegKind kind) const noexcept {
    LockGuard lock(m_mutex);
    return m_legActivityCount[legIndex(kind)] > 0 || m_exclusiveLeg == kind;
}

std::optional<HeavyLegKind> ResourceGovernor::activeHeavyLeg() const noexcept {
    LockGuard lock(m_mutex);
    if (m_exclusiveLeg.has_value()) {
        return m_exclusiveLeg;
    }
    for (const HeavyLegKind k : {HeavyLegKind::Build, HeavyLegKind::BackgroundIndex, HeavyLegKind::Inspection}) {
        if (m_legActivityCount[legIndex(k)] > 0) {
            return k;
        }
    }
    return std::nullopt;
}

unsigned ResourceGovernor::activeLegCount() const noexcept {
    LockGuard lock(m_mutex);
    unsigned count = 0;
    for (const unsigned c : m_legActivityCount) {
        if (c > 0) {
            ++count;
        }
    }
    return count;
}

void ResourceGovernor::recordProcessRss(std::int64_t pid, std::uint64_t rssBytes, HeavyLegKind leg,
                                        std::optional<std::uint64_t> osPeakBytes) {
    if (pid <= 0) {
        return;
    }
    LockGuard lock(m_mutex);
    ProcessRssEntry& entry = m_processRss[pid];
    entry.leg = leg;
    entry.active = true;
    entry.currentBytes = rssBytes;
    const auto observedPeak = std::max(rssBytes, osPeakBytes.value_or(0));
    entry.peakBytes = std::max(entry.peakBytes, observedPeak);

    const std::size_t idx = legIndex(leg);
    m_peakLegRssBytes[idx] = std::max(m_peakLegRssBytes[idx], observedPeak);

    std::uint64_t currentCombined = 0;
    for (const auto& [otherPid, otherEntry] : m_processRss) {
        if (otherEntry.active) {
            currentCombined += otherEntry.currentBytes;
        }
    }
    m_peakCombinedRssBytes = std::max(m_peakCombinedRssBytes, currentCombined);
}

Result<std::uint64_t> ResourceGovernor::sampleProcessRss(std::int64_t pid, HeavyLegKind leg) {
    auto rss = queryProcessRssBytes(pid);
    if (!rss) {
        return std::unexpected(std::move(rss.error()));
    }
    auto peak = queryProcessPeakRssBytes(pid);
    recordProcessRss(pid, *rss, leg, peak ? std::optional{*peak} : std::nullopt);
    if (!peak) {
        return std::unexpected(std::move(peak.error()));
    }
    return *rss;
}

void ResourceGovernor::unregisterProcess(std::int64_t pid) {
    LockGuard lock(m_mutex);
    auto it = m_processRss.find(pid);
    if (it != m_processRss.end()) {
        it->second.active = false;
        it->second.currentBytes = 0;
    }
}

void ResourceGovernor::resetRssTracking() {
    LockGuard lock(m_mutex);
    m_processRss.clear();
    m_peakLegRssBytes = {0, 0, 0};
    m_peakCombinedRssBytes = 0;
}

std::uint64_t ResourceGovernor::currentProcessRssBytes(std::int64_t pid) const {
    LockGuard lock(m_mutex);
    const auto it = m_processRss.find(pid);
    if (it == m_processRss.end() || !it->second.active) {
        return 0;
    }
    return it->second.currentBytes;
}

std::uint64_t ResourceGovernor::peakProcessRssBytes(std::int64_t pid) const {
    LockGuard lock(m_mutex);
    const auto it = m_processRss.find(pid);
    return it != m_processRss.end() ? it->second.peakBytes : 0;
}

std::uint64_t ResourceGovernor::peakLegRssBytes(HeavyLegKind leg) const {
    LockGuard lock(m_mutex);
    return m_peakLegRssBytes[legIndex(leg)];
}

std::uint64_t ResourceGovernor::currentCombinedRssBytes() const {
    LockGuard lock(m_mutex);
    std::uint64_t total = 0;
    for (const auto& [pid, entry] : m_processRss) {
        if (entry.active) {
            total += entry.currentBytes;
        }
    }
    return total;
}

std::uint64_t ResourceGovernor::peakCombinedRssBytes() const {
    LockGuard lock(m_mutex);
    return m_peakCombinedRssBytes;
}

bool ResourceGovernor::isProcessOverRssCap(std::int64_t pid) const {
    LockGuard lock(m_mutex);
    const auto it = m_processRss.find(pid);
    if (it == m_processRss.end()) {
        return false;
    }
    const std::uint64_t cap =
        (it->second.leg == HeavyLegKind::BackgroundIndex) ? m_options.clangdRssCapBytes : m_options.combinedRssCapBytes;
    return (it->second.active && it->second.currentBytes > cap) || it->second.peakBytes > cap;
}

bool ResourceGovernor::isCombinedOverRssCap() const {
    LockGuard lock(m_mutex);
    std::uint64_t total = 0;
    for (const auto& [pid, entry] : m_processRss) {
        if (entry.active) {
            total += entry.currentBytes;
        }
    }
    return total > m_options.combinedRssCapBytes || m_peakCombinedRssBytes > m_options.combinedRssCapBytes;
}

} // namespace frost::sched
