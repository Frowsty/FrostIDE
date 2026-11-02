// SPDX-License-Identifier: Apache-2.0
// Linux local IPC: user-owned private AF_UNIX endpoints and flock owner election.
#include "frost/io/local_ipc.hpp"

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace frost::io {
namespace {
using namespace std::chrono_literals;
constexpr std::size_t kMaxPayload = 65536;
constexpr std::size_t kMaxFrame = kMaxPayload + 1;
bool validKey(std::string_view key) {
    return key.size() == 64 &&
           std::ranges::all_of(key, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
Result<std::string> socketDirectory() {
    const std::string dir = "/tmp/frostide-ipc-" + std::to_string(geteuid());
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
        return fail(ErrorCode::IoError, "cannot create IPC directory");
    struct stat st{};
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) != 0)
        return fail(ErrorCode::PermissionDenied, "IPC directory is not private to the current user");
    return dir;
}
Result<std::string> pathFor(std::string_view key) {
    if (!validKey(key))
        return fail(ErrorCode::InvalidArgument, "invalid IPC key");
    auto dir = socketDirectory();
    if (!dir)
        return std::unexpected(std::move(dir.error()));
    return *dir + "/" + std::string(key);
}
struct Fd {
    int fd = -1;
    ~Fd() {
        if (fd >= 0)
            close(fd);
    }
    Fd() = default;
    explicit Fd(int value) : fd(value) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
using Deadline = std::chrono::steady_clock::time_point;
bool waitFd(int fd, short events, Deadline deadline) {
    for (;;) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left <= 0ms)
            return false;
        pollfd p{fd, events, 0};
        int r = poll(&p, 1, static_cast<int>(std::min(left, 200ms).count()));
        if (r > 0)
            return (p.revents & events) != 0;
        if (r < 0 && errno != EINTR)
            return false;
    }
}
bool transfer(int fd, char* bytes, std::size_t size, bool write, Deadline deadline) {
    while (size) {
        if (!waitFd(fd, write ? POLLOUT : POLLIN, deadline))
            return false;
        ssize_t count = write ? send(fd, bytes, size, MSG_NOSIGNAL) : recv(fd, bytes, size, 0);
        if (count < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (count <= 0)
            return false;
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}
bool putFrame(int fd, std::string_view value, Deadline deadline) {
    if (value.size() > kMaxFrame)
        return false;
    const auto size = static_cast<std::uint32_t>(value.size());
    std::array<char, 4> head{static_cast<char>(size & 255U), static_cast<char>((size >> 8U) & 255U),
                             static_cast<char>((size >> 16U) & 255U), static_cast<char>((size >> 24U) & 255U)};
    return transfer(fd, head.data(), head.size(), true, deadline) &&
           transfer(fd, const_cast<char*>(value.data()), value.size(), true, deadline);
}
Result<std::string> getFrame(int fd, Deadline deadline, std::size_t limit) {
    std::array<char, 4> head{};
    if (!transfer(fd, head.data(), head.size(), false, deadline))
        return fail(ErrorCode::Timeout, "IPC read timed out");
    std::uint32_t size = 0;
    for (unsigned i = 0; i < 4; ++i)
        size |= static_cast<std::uint32_t>(static_cast<unsigned char>(head[i])) << (8U * i);
    if (size > limit)
        return fail(ErrorCode::InvalidArgument, "IPC frame is too large");
    std::string result(size, '\0');
    if (!transfer(fd, result.data(), result.size(), false, deadline))
        return fail(ErrorCode::Timeout, "IPC read timed out");
    return result;
}
sockaddr_un address(std::string_view path) {
    sockaddr_un out{};
    out.sun_family = AF_UNIX;
    std::memcpy(out.sun_path, path.data(), path.size());
    return out;
}
} // namespace
struct LocalIpcOwner::Impl {
    std::string path;
    Fd lock;
    Fd listener;
    std::jthread thread;
};
LocalIpcOwner::LocalIpcOwner(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
LocalIpcOwner::~LocalIpcOwner() { stop(); }
Result<std::unique_ptr<LocalIpcOwner>> LocalIpcOwner::claim(std::string_view key) {
    auto path = pathFor(key);
    if (!path)
        return std::unexpected(std::move(path.error()));
    auto impl = std::make_unique<Impl>();
    impl->path = *path;
    impl->lock.fd = open((impl->path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (impl->lock.fd < 0)
        return fail(ErrorCode::IoError, "cannot open IPC owner lock");
    if (flock(impl->lock.fd, LOCK_EX | LOCK_NB) != 0)
        return fail(ErrorCode::Busy, "project already has an IPC owner");
    return std::unique_ptr<LocalIpcOwner>(new LocalIpcOwner(std::move(impl)));
}
Result<void> LocalIpcOwner::start(Handler handler) {
    if (m_impl->thread.joinable())
        return fail(ErrorCode::Busy, "IPC server already started");
    m_impl->listener.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_impl->listener.fd < 0)
        return fail(ErrorCode::IoError, "cannot create local socket");
    (void)unlink(m_impl->path.c_str()); // Owner lock excludes another live server.
    auto addr = address(m_impl->path);
    if (bind(m_impl->listener.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        chmod(m_impl->path.c_str(), 0600) != 0 || listen(m_impl->listener.fd, 8) != 0)
        return fail(ErrorCode::IoError, "cannot start local socket");
    auto* impl = m_impl.get();
    impl->thread = std::jthread([impl, handler = std::move(handler)](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (!waitFd(impl->listener.fd, POLLIN, std::chrono::steady_clock::now() + 100ms))
                continue;
            Fd client(accept4(impl->listener.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (client.fd < 0)
                continue;
            struct ucred peer{};
            socklen_t len = sizeof(peer);
            if (getsockopt(client.fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) != 0 || peer.uid != geteuid())
                continue;
            auto deadline = std::chrono::steady_clock::now() + 3s;
            auto request = getFrame(client.fd, deadline, kMaxPayload);
            if (!request)
                continue;
            auto reply = handler(std::move(*request));
            const std::string& body = reply ? *reply : reply.error().message();
            std::string response;
            if (body.size() > kMaxPayload)
                response = std::string(1, '\1') + "IPC reply exceeds size limit";
            else {
                response.assign(1, reply ? '\0' : '\1');
                response += body;
            }
            (void)putFrame(client.fd, response, deadline);
        }
    });
    return {};
}
void LocalIpcOwner::stop() noexcept {
    if (!m_impl)
        return;
    if (m_impl->thread.joinable()) {
        m_impl->thread.request_stop();
        m_impl->thread.join();
    }
    if (m_impl->listener.fd >= 0) {
        close(m_impl->listener.fd);
        m_impl->listener.fd = -1;
        (void)unlink(m_impl->path.c_str());
    }
}
Result<std::string> requestLocalIpc(std::string_view key, std::string_view request, std::chrono::milliseconds timeout) {
    if (request.size() > kMaxPayload || timeout <= 0ms)
        return fail(ErrorCode::InvalidArgument, "invalid IPC request or timeout");
    auto path = pathFor(key);
    if (!path)
        return std::unexpected(std::move(path.error()));
    auto deadline = std::chrono::steady_clock::now() + timeout;
    auto addr = address(*path);
    while (std::chrono::steady_clock::now() < deadline) {
        Fd client(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (client.fd < 0)
            return fail(ErrorCode::IoError, "cannot create local socket");
        int result = connect(client.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (result == 0 || (errno == EINPROGRESS && waitFd(client.fd, POLLOUT, deadline))) {
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(client.fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                struct ucred peer{};
                socklen_t peerLen = sizeof(peer);
                if (getsockopt(client.fd, SOL_SOCKET, SO_PEERCRED, &peer, &peerLen) != 0 || peer.uid != geteuid())
                    return fail(ErrorCode::PermissionDenied, "IPC server belongs to another user");
                if (!putFrame(client.fd, request, deadline))
                    return fail(ErrorCode::Timeout, "IPC write timed out");
                auto reply = getFrame(client.fd, deadline, kMaxFrame);
                if (!reply)
                    return reply;
                if (reply->empty())
                    return fail(ErrorCode::Corrupt, "empty IPC response");
                if ((*reply)[0] != '\0')
                    return fail(ErrorCode::InvalidArgument, reply->substr(1));
                return reply->substr(1);
            }
        }
        std::this_thread::sleep_for(20ms);
    }
    return fail(ErrorCode::Timeout, "local IPC owner did not respond");
}
} // namespace frost::io
