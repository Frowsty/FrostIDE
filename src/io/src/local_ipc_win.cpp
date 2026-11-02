// SPDX-License-Identifier: Apache-2.0
// Windows local IPC: current-user ACL named mutex reservation and bounded overlapped named pipe.
#include "frost/io/local_ipc.hpp"
#include "frost/io/utf.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <sddl.h>

namespace frost::io {
namespace {
using namespace std::chrono_literals;
constexpr std::size_t kMaxPayload = 65536;
constexpr std::size_t kMaxFrame = kMaxPayload + 1;
bool validKey(std::string_view key) {
    return key.size() == 64 &&
           std::ranges::all_of(key, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Security {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attrs{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    ~Security() {
        if (descriptor)
            LocalFree(descriptor);
    }
};
struct Names {
    std::wstring mutex;
    std::wstring pipe;
    std::wstring sid;
};
Result<Names> namesFor(std::string_view key) {
    if (!validKey(key))
        return fail(ErrorCode::InvalidArgument, "invalid IPC key");
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
        return fail(ErrorCode::IoError, "cannot query user token");
    DWORD size = 0;
    (void)GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> bytes(size);
    if (!size || !GetTokenInformation(token.value, TokenUser, bytes.data(), size, &size))
        return fail(ErrorCode::IoError, "cannot query user identity");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid))
        return fail(ErrorCode::IoError, "cannot format user identity");
    std::wstring user(sid);
    LocalFree(sid);
    auto wide = toWide(key);
    if (!wide)
        return std::unexpected(std::move(wide.error()));
    std::wstring stem = L"FrostIDE-" + user + L"-" + *wide;
    // Microsoft documents SeCreateGlobalPrivilege for global file mappings and symbolic
    // links, not mutexes: https://learn.microsoft.com/en-us/windows/win32/termserv/kernel-object-namespaces
    return Names{L"Global\\" + stem, L"\\\\.\\pipe\\" + stem, std::move(user)};
}
bool secure(Security& security, const std::wstring& sid) {
    std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security.descriptor,
                                                              nullptr))
        return false;
    security.attrs.lpSecurityDescriptor = security.descriptor;
    return true;
}
bool currentUserServer(HANDLE pipe, const std::wstring& expectedSid) {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid))
        return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.value)
        return false;
    Handle token;
    if (!OpenProcessToken(process.value, TOKEN_QUERY, &token.value))
        return false;
    DWORD size = 0;
    (void)GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> bytes(size);
    if (!size || !GetTokenInformation(token.value, TokenUser, bytes.data(), size, &size))
        return false;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid))
        return false;
    bool same = expectedSid == sid;
    LocalFree(sid);
    return same;
}
using Deadline = std::chrono::steady_clock::time_point;
DWORD remaining(Deadline deadline) {
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return ms <= 0 ? 0 : static_cast<DWORD>(std::min<std::int64_t>(ms, MAXDWORD - 1));
}
bool operation(HANDLE file, char* bytes, DWORD size, bool write, Deadline deadline) {
    DWORD transferred = 0;
    while (size) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.value)
            return false;
        OVERLAPPED ov{};
        ov.hEvent = event.value;
        DWORD done = 0;
        BOOL ok = write ? WriteFile(file, bytes, size, &done, &ov) : ReadFile(file, bytes, size, &done, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(event.value, remaining(deadline)) != WAIT_OBJECT_0) {
                CancelIoEx(file, &ov);
                (void)WaitForSingleObject(event.value, INFINITE);
                return false;
            }
            ok = GetOverlappedResult(file, &ov, &done, FALSE);
        }
        if (!ok || done == 0)
            return false;
        transferred += done;
        bytes += done;
        size -= done;
    }
    return transferred > 0;
}
bool putFrame(HANDLE file, std::string_view value, Deadline deadline) {
    if (value.size() > kMaxFrame)
        return false;
    auto size = static_cast<std::uint32_t>(value.size());
    std::array<char, 4> h{static_cast<char>(size & 255U), static_cast<char>((size >> 8U) & 255U),
                          static_cast<char>((size >> 16U) & 255U), static_cast<char>((size >> 24U) & 255U)};
    return operation(file, h.data(), 4, true, deadline) &&
           (value.empty() || operation(file, const_cast<char*>(value.data()), size, true, deadline));
}
Result<std::string> getFrame(HANDLE file, Deadline deadline, std::size_t limit) {
    std::array<char, 4> h{};
    if (!operation(file, h.data(), 4, false, deadline))
        return fail(ErrorCode::Timeout, "IPC read timed out");
    std::uint32_t size = 0;
    for (unsigned i = 0; i < 4; ++i)
        size |= static_cast<std::uint32_t>(static_cast<unsigned char>(h[i])) << (8U * i);
    if (size > limit)
        return fail(ErrorCode::InvalidArgument, "IPC frame is too large");
    std::string value(size, '\0');
    if (size && !operation(file, value.data(), size, false, deadline))
        return fail(ErrorCode::Timeout, "IPC read timed out");
    return value;
}
} // namespace
struct LocalIpcOwner::Impl {
    Names names;
    Handle mutex;
    std::jthread thread;
};
LocalIpcOwner::LocalIpcOwner(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
LocalIpcOwner::~LocalIpcOwner() { stop(); }
Result<std::unique_ptr<LocalIpcOwner>> LocalIpcOwner::claim(std::string_view key) {
    auto names = namesFor(key);
    if (!names)
        return std::unexpected(std::move(names.error()));
    Security security;
    if (!secure(security, names->sid))
        return fail(ErrorCode::IoError, "cannot secure IPC owner");
    auto impl = std::make_unique<Impl>();
    impl->names = std::move(*names);
    impl->mutex.value = CreateMutexW(&security.attrs, FALSE, impl->names.mutex.c_str());
    if (!impl->mutex.value)
        return fail(ErrorCode::IoError, "cannot create IPC owner mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return fail(ErrorCode::Busy, "project already has an IPC owner");
    return std::unique_ptr<LocalIpcOwner>(new LocalIpcOwner(std::move(impl)));
}
Result<void> LocalIpcOwner::start(Handler handler) {
    if (m_impl->thread.joinable())
        return fail(ErrorCode::Busy, "IPC server already started");
    Security security;
    if (!secure(security, m_impl->names.sid))
        return fail(ErrorCode::IoError, "cannot secure IPC pipe");
    Handle pipe(CreateNamedPipeW(m_impl->names.pipe.c_str(),
                                 PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
                                 65536, 0, &security.attrs));
    if (pipe.value == INVALID_HANDLE_VALUE)
        return fail(ErrorCode::IoError, "cannot create IPC pipe");
    HANDLE raw = std::exchange(pipe.value, nullptr);
    m_impl->thread = std::jthread([raw, handler = std::move(handler)](std::stop_token stop) {
        Handle pipeHandle(raw);
        while (!stop.stop_requested()) {
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event.value)
                break;
            OVERLAPPED ov{};
            ov.hEvent = event.value;
            bool connected = ConnectNamedPipe(pipeHandle.value, &ov) != FALSE;
            if (!connected) {
                DWORD error = GetLastError();
                if (error == ERROR_PIPE_CONNECTED)
                    connected = true;
                else if (error == ERROR_NO_DATA) {
                    // A client disconnected between opening the pipe and ConnectNamedPipe.
                    (void)DisconnectNamedPipe(pipeHandle.value);
                } else if (error == ERROR_IO_PENDING) {
                    while (!stop.stop_requested()) {
                        DWORD wait = WaitForSingleObject(event.value, 100);
                        if (wait == WAIT_OBJECT_0) {
                            DWORD done = 0;
                            connected = GetOverlappedResult(pipeHandle.value, &ov, &done, FALSE) != FALSE;
                            break;
                        }
                        if (wait != WAIT_TIMEOUT)
                            break;
                    }
                    if (!connected) {
                        CancelIoEx(pipeHandle.value, &ov);
                        (void)WaitForSingleObject(event.value, INFINITE);
                    }
                }
            }
            if (!connected)
                continue;
            auto deadline = std::chrono::steady_clock::now() + 3s;
            auto request = getFrame(pipeHandle.value, deadline, kMaxPayload);
            if (request) {
                auto reply = handler(std::move(*request));
                const std::string& body = reply ? *reply : reply.error().message();
                std::string response;
                if (body.size() > kMaxPayload)
                    response = std::string(1, '\1') + "IPC reply exceeds size limit";
                else {
                    response.assign(1, reply ? '\0' : '\1');
                    response += body;
                }
                if (putFrame(pipeHandle.value, response, deadline)) {
                    // DisconnectNamedPipe discards unread data. Wait for the client's bounded
                    // acknowledgement rather than flushing (which could wait indefinitely).
                    char acknowledged = 0;
                    (void)operation(pipeHandle.value, &acknowledged, 1, false, deadline);
                }
            }
            (void)DisconnectNamedPipe(pipeHandle.value);
        }
    });
    return {};
}
void LocalIpcOwner::stop() noexcept {
    if (m_impl && m_impl->thread.joinable()) {
        m_impl->thread.request_stop();
        m_impl->thread.join();
    }
}
Result<std::string> requestLocalIpc(std::string_view key, std::string_view request, std::chrono::milliseconds timeout) {
    if (request.size() > kMaxPayload || timeout <= 0ms)
        return fail(ErrorCode::InvalidArgument, "invalid IPC request or timeout");
    auto names = namesFor(key);
    if (!names)
        return std::unexpected(std::move(names.error()));
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        Handle pipe(CreateFileW(names->pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, nullptr));
        if (pipe.value != INVALID_HANDLE_VALUE) {
            if (!currentUserServer(pipe.value, names->sid))
                return fail(ErrorCode::PermissionDenied, "IPC server belongs to another user");
            if (!putFrame(pipe.value, request, deadline))
                return fail(ErrorCode::Timeout, "IPC write timed out");
            auto reply = getFrame(pipe.value, deadline, kMaxFrame);
            if (!reply)
                return reply;
            char acknowledged = 1;
            if (!operation(pipe.value, &acknowledged, 1, true, deadline))
                return fail(ErrorCode::IoError, "cannot acknowledge IPC reply");
            if (reply->empty())
                return fail(ErrorCode::Corrupt, "empty IPC response");
            if ((*reply)[0] != '\0')
                return fail(ErrorCode::InvalidArgument, reply->substr(1));
            return reply->substr(1);
        }
        std::this_thread::sleep_for(20ms);
    }
    return fail(ErrorCode::Timeout, "local IPC owner did not respond");
}
} // namespace frost::io
