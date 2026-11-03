// SPDX-License-Identifier: Apache-2.0
// Parent-exit observation through a retained process handle prevents PID reuse.
#include <frost/update/parent_process.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::update {
struct ParentProcess::Impl {
#ifdef _WIN32
    HANDLE handle = nullptr;
    ~Impl() {
        if (handle)
            CloseHandle(handle);
    }
#endif
};
ParentProcess::ParentProcess(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
ParentProcess::~ParentProcess() = default;
ParentProcess::ParentProcess(ParentProcess&&) noexcept = default;
ParentProcess& ParentProcess::operator=(ParentProcess&&) noexcept = default;

#ifdef _WIN32
namespace {
Result<std::uint64_t> creationTime(HANDLE handle) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(handle, &creation, &exit, &kernel, &user))
        return fail(ErrorCode::IoError, "Cannot query updater parent identity");
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}
} // namespace
#endif
Result<ParentIdentity> currentParentIdentity() {
#ifdef _WIN32
    auto creation = creationTime(GetCurrentProcess());
    if (!creation)
        return std::unexpected(creation.error());
    return ParentIdentity{GetCurrentProcessId(), *creation};
#else
    return fail(ErrorCode::Unsupported, "Windows updater parent identity is unavailable");
#endif
}
Result<ParentProcess> ParentProcess::acquire(ParentIdentity identity) {
    if (identity.pid == 0 || identity.creationTime == 0)
        return fail(ErrorCode::InvalidArgument, "Updater parent identity is incomplete");
#ifdef _WIN32
    auto impl = std::make_unique<Impl>();
    impl->handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, identity.pid);
    if (!impl->handle)
        return fail(ErrorCode::IoError, "Cannot retain updater parent process");
    auto creation = creationTime(impl->handle);
    if (!creation)
        return std::unexpected(creation.error());
    if (*creation != identity.creationTime)
        return fail(ErrorCode::PermissionDenied, "Updater parent identity changed");
    const auto state = WaitForSingleObject(impl->handle, 0);
    if (state == WAIT_OBJECT_0)
        return fail(ErrorCode::Cancelled, "Updater parent exited before handshake");
    if (state != WAIT_TIMEOUT)
        return fail(ErrorCode::IoError, "Cannot query updater parent state");
    return ParentProcess(std::move(impl));
#else
    return fail(ErrorCode::Unsupported, "Windows updater parent barrier is unavailable");
#endif
}
Result<bool> ParentProcess::exited() const {
    if (!m_impl)
        return fail(ErrorCode::InvalidArgument, "Updater parent guard is empty");
#ifdef _WIN32
    const auto state = WaitForSingleObject(m_impl->handle, 0);
    if (state == WAIT_OBJECT_0)
        return true;
    if (state == WAIT_TIMEOUT)
        return false;
    return fail(ErrorCode::IoError, "Cannot query retained updater parent state");
#else
    return fail(ErrorCode::Unsupported, "Windows updater parent barrier is unavailable");
#endif
}
} // namespace frost::update
