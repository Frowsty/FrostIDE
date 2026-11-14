// SPDX-License-Identifier: Apache-2.0
// Bind a Mono loopback tuple to the retained selected Windows process.
#include "frost/debug/mono_windows_peer.hpp"

#include <cstring>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>

#include <iphlpapi.h>
#include <winsock2.h>
#endif

namespace frost::debug::mono {
struct WindowsPeer::Impl {
    std::uint32_t pid = 0;
    std::uint64_t creation = 0;
    std::function<bool()> current;
#ifdef _WIN32
    HANDLE process = nullptr;
    ~Impl() {
        if (process)
            CloseHandle(process);
    }
#else
    ~Impl() = default;
#endif
};
WindowsPeer::WindowsPeer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
WindowsPeer::~WindowsPeer() = default;
#ifdef _WIN32
namespace {
bool identityMatches(const WindowsPeer::Impl& impl) {
    if (!impl.process || GetProcessId(impl.process) != impl.pid || WaitForSingleObject(impl.process, 0) != WAIT_TIMEOUT)
        return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(impl.process, &created, &exited, &kernel, &user))
        return false;
    const auto actual = std::uint64_t(created.dwLowDateTime) | (std::uint64_t(created.dwHighDateTime) << 32);
    return actual == impl.creation;
}
} // namespace
#endif
Result<std::shared_ptr<WindowsPeer>> WindowsPeer::open(std::uint32_t pid, std::uint64_t creationTime,
                                                       std::function<bool()> current) {
    if (!pid || !creationTime || !current)
        return fail(ErrorCode::InvalidArgument, "Mono peer identity required");
#ifndef _WIN32
    return fail(ErrorCode::Unsupported, "Windows Mono peer verification is unavailable");
#else
    auto impl = std::make_unique<Impl>();
    impl->pid = pid;
    impl->creation = creationTime;
    impl->current = std::move(current);
    impl->process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!impl->process || !impl->current() || !identityMatches(*impl))
        return fail(ErrorCode::PermissionDenied, "selected Mono process identity changed");
    return std::shared_ptr<WindowsPeer>(new WindowsPeer(std::move(impl)));
#endif
}
Result<void> WindowsPeer::verify(std::uint16_t localPort, std::uint16_t remotePort, MonoDeadline deadline,
                                 std::stop_token stop) const {
    if (!localPort || !remotePort)
        return fail(ErrorCode::InvalidArgument, "invalid Mono TCP tuple");
    const auto check = [deadline, stop = std::move(stop)]() -> Result<void> {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Mono peer verification cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            return fail(ErrorCode::Timeout, "Mono peer verification timed out");
        return {};
    };
    if (auto valid = check(); !valid)
        return std::unexpected(valid.error());
#ifndef _WIN32
    return fail(ErrorCode::Unsupported, "Windows Mono peer verification is unavailable");
#else
    if (!m_impl || !m_impl->current() || !identityMatches(*m_impl))
        return fail(ErrorCode::PermissionDenied, "selected Mono process is no longer current");
    if (auto valid = check(); !valid)
        return std::unexpected(valid.error());
    DWORD size = 0;
    DWORD result = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (auto valid = check(); !valid)
        return std::unexpected(valid.error());
    constexpr DWORD maxTable = 16 * 1024 * 1024;
    if (result != ERROR_INSUFFICIENT_BUFFER || size < sizeof(DWORD) || size > maxTable)
        return fail(ErrorCode::IoError, "cannot size Mono TCP owner table");
    std::vector<std::uint8_t> table;
    bool read = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (auto valid = check(); !valid)
            return std::unexpected(valid.error());
        if (size < sizeof(DWORD) || size > maxTable)
            return fail(ErrorCode::OutOfRange, "Mono TCP owner table too large");
        table.resize(size);
        result = GetExtendedTcpTable(table.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
        if (auto valid = check(); !valid)
            return std::unexpected(valid.error());
        if (result == NO_ERROR) {
            read = true;
            break;
        }
        if (result != ERROR_INSUFFICIENT_BUFFER)
            return fail(ErrorCode::IoError, "cannot read Mono TCP owner table");
    }
    if (!read || size < sizeof(DWORD) || size > table.size())
        return fail(ErrorCode::IoError, "Mono TCP owner table changed repeatedly");
    DWORD count = 0;
    std::memcpy(&count, table.data(), sizeof(count));
    if (count > (size - sizeof(DWORD)) / sizeof(MIB_TCPROW_OWNER_PID))
        return fail(ErrorCode::Corrupt, "invalid Mono TCP owner table");
    std::size_t matches = 0;
    DWORD owner = 0;
    for (DWORD i = 0; i < count; ++i) {
        if (auto valid = check(); !valid)
            return std::unexpected(valid.error());
        MIB_TCPROW_OWNER_PID row{};
        std::memcpy(&row, table.data() + sizeof(DWORD) + std::size_t(i) * sizeof(row), sizeof(row));
        if (row.dwState == MIB_TCP_STATE_ESTAB && row.dwLocalAddr == htonl(INADDR_LOOPBACK) &&
            row.dwRemoteAddr == htonl(INADDR_LOOPBACK) &&
            ntohs(static_cast<std::uint16_t>(row.dwLocalPort)) == remotePort &&
            ntohs(static_cast<std::uint16_t>(row.dwRemotePort)) == localPort) {
            ++matches;
            owner = row.dwOwningPid;
        }
    }
    if (auto valid = check(); !valid)
        return std::unexpected(valid.error());
    if (matches != 1 || owner != m_impl->pid || !m_impl->current() || !identityMatches(*m_impl))
        return fail(ErrorCode::PermissionDenied, "Mono TCP peer does not belong to selected process");
    if (auto valid = check(); !valid)
        return std::unexpected(valid.error());
    return {};
#endif
}
} // namespace frost::debug::mono
