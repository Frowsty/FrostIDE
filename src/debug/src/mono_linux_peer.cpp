// SPDX-License-Identifier: Apache-2.0
// Check Linux selected-process lifetime and exact reverse loopback socket ownership before I/O.
#include "frost/debug/mono_linux_peer.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <sstream>
#include <utility>
#ifdef __linux__
#include <sys/stat.h>
#include <sys/syscall.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif
namespace frost::debug::mono {
namespace {
std::string endpoint(std::uint16_t port) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result = "0100007F:0000";
    for (unsigned i = 0; i < 4; ++i)
        result[12 - i] = hex[(port >> (i * 4)) & 15];
    return result;
}
bool number(std::string_view text, std::uint64_t& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
} // namespace
Result<std::uint64_t> linuxMonoSocketInode(std::string_view table, std::uint16_t localPort, std::uint16_t remotePort) {
    // Linux kernel proc_net_tcp documentation: state/uid/inode columns; IPv4 loopback only.
    // https://docs.kernel.org/networking/proc_net_tcp.html
    if (!localPort || !remotePort || table.size() > 4 * 1024 * 1024)
        return fail(ErrorCode::InvalidArgument, "invalid Linux Mono socket table");
    std::istringstream input{std::string(table)};
    std::string line;
    std::uint64_t found = 0;
    std::size_t count = 0;
    while (std::getline(input, line)) {
        if (++count > 65536 || line.size() > 4096)
            return fail(ErrorCode::OutOfRange, "Linux socket table limit");
        std::istringstream row(line);
        std::array<std::string, 10> fields;
        bool complete = true;
        for (auto& field : fields)
            if (!(row >> field)) {
                complete = false;
                break;
            }
        if (!complete || fields[1] != endpoint(remotePort) || fields[2] != endpoint(localPort) || fields[3] != "01")
            continue;
        std::uint64_t inode = 0;
        if (!number(fields[9], inode) || !inode || found)
            return fail(ErrorCode::Corrupt, "ambiguous Linux Mono socket tuple");
        found = inode;
    }
    if (!found)
        return fail(ErrorCode::PermissionDenied, "selected Mono reverse loopback socket is unavailable");
    return found;
}
struct LinuxPeer::Impl {
    LinuxProcessIdentity identity;
    std::function<bool()> current;
#ifdef __linux__
    int pidfd = -1, proc = -1;
    ~Impl() {
        if (pidfd >= 0)
            ::close(pidfd);
        if (proc >= 0)
            ::close(proc);
    }
#else
    ~Impl() = default;
#endif
};
LinuxPeer::LinuxPeer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
LinuxPeer::~LinuxPeer() = default;
LinuxProcessIdentity LinuxPeer::identity() const { return m_impl->identity; }
#ifdef __linux__
namespace {
Result<std::string> readAt(int dir, const char* name, std::size_t limit) {
    const int fd = ::openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return fail(ErrorCode::PermissionDenied, "Mono process evidence cannot be read");
    std::string result;
    std::array<char, 4096> bytes{};
    for (;;) {
        const auto got = ::read(fd, bytes.data(), bytes.size());
        if (got < 0) {
            ::close(fd);
            return fail(ErrorCode::IoError, "Mono process evidence read failed");
        }
        if (!got)
            break;
        if (static_cast<std::size_t>(got) > limit - result.size()) {
            ::close(fd);
            return fail(ErrorCode::OutOfRange, "Mono process evidence exceeds bound");
        }
        result.append(bytes.data(), static_cast<std::size_t>(got));
    }
    ::close(fd);
    return result;
}
Result<LinuxProcessIdentity> readIdentity(const LinuxPeer::Impl& impl) {
    pollfd poller{impl.pidfd, POLLIN, 0};
    struct stat owner{}, ownNet{}, selectedNet{};
    if (::poll(&poller, 1, 0) != 0 || ::fstat(impl.proc, &owner) != 0 || owner.st_uid != ::geteuid() ||
        ::stat("/proc/self/ns/net", &ownNet) != 0 || ::fstatat(impl.proc, "ns/net", &selectedNet, 0) != 0 ||
        ownNet.st_dev != selectedNet.st_dev || ownNet.st_ino != selectedNet.st_ino)
        return fail(ErrorCode::PermissionDenied, "Mono process lifetime, user or network namespace changed");
    auto stat = readAt(impl.proc, "stat", 16384);
    if (!stat)
        return std::unexpected(std::move(stat.error()));
    const auto end = stat->rfind(')');
    if (end == stat->npos || end + 2 >= stat->size())
        return fail(ErrorCode::Corrupt, "invalid Mono process stat");
    std::istringstream fields(stat->substr(end + 2));
    std::string field;
    for (unsigned i = 0; i <= 19; ++i)
        if (!(fields >> field))
            return fail(ErrorCode::Corrupt, "missing Mono process start identity");
    std::uint64_t start = 0;
    if (!number(field, start) || !start)
        return fail(ErrorCode::Corrupt, "invalid Mono process start identity");
    std::array<char, 32768> image{};
    const auto length = ::readlinkat(impl.proc, "exe", image.data(), image.size());
    if (length <= 0 || static_cast<std::size_t>(length) >= image.size() || image[0] != '/')
        return fail(ErrorCode::PermissionDenied, "Mono process executable identity unavailable");
    return LinuxProcessIdentity{impl.identity.pid, start, std::string(image.data(), static_cast<std::size_t>(length))};
}
} // namespace
#endif
Result<std::shared_ptr<LinuxPeer>> LinuxPeer::select(std::uint32_t pid, std::function<bool()> current) {
    if (!pid || pid > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || !current)
        return fail(ErrorCode::InvalidArgument, "select a live Mono process");
#ifndef __linux__
    return fail(ErrorCode::Unsupported, "Linux Mono process ownership requires Linux");
#else
    auto impl = std::make_unique<Impl>();
    impl->identity.pid = pid;
    impl->current = std::move(current);
    impl->pidfd = static_cast<int>(::syscall(SYS_pidfd_open, static_cast<int>(pid), 0));
    if (impl->pidfd < 0)
        return fail(ErrorCode::Unsupported, "Linux pidfd ownership is unavailable");
    impl->proc = ::open(("/proc/" + std::to_string(pid)).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (impl->proc < 0 || !impl->current())
        return fail(ErrorCode::PermissionDenied, "selected Mono process cannot be held");
    auto identity = readIdentity(*impl);
    if (!identity)
        return std::unexpected(std::move(identity.error()));
    impl->identity = std::move(*identity);
    return std::shared_ptr<LinuxPeer>(new LinuxPeer(std::move(impl)));
#endif
}
Result<void> LinuxPeer::verify(std::uint16_t localPort, std::uint16_t remotePort, MonoDeadline deadline,
                               std::stop_token stop) const {
#ifndef __linux__
    (void)localPort;
    (void)remotePort;
    (void)deadline;
    (void)stop;
    return fail(ErrorCode::Unsupported, "Linux Mono process ownership requires Linux");
#else
    const auto current = [&] {
        return !stop.stop_requested() && std::chrono::steady_clock::now() < deadline && m_impl->current();
    };
    if (!current())
        return fail(ErrorCode::Cancelled, "Linux Mono admission expired");
    auto before = readIdentity(*m_impl);
    if (!before || before->startTime != m_impl->identity.startTime || before->image != m_impl->identity.image)
        return fail(ErrorCode::PermissionDenied, "selected Mono process identity changed");
    auto table = readAt(m_impl->proc, "net/tcp", 4 * 1024 * 1024);
    if (!table)
        return std::unexpected(std::move(table.error()));
    auto inode = linuxMonoSocketInode(*table, localPort, remotePort);
    if (!inode)
        return std::unexpected(std::move(inode.error()));
    const int fd = ::openat(m_impl->proc, "fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    DIR* entries = fd >= 0 ? ::fdopendir(fd) : nullptr;
    if (!entries) {
        if (fd >= 0)
            ::close(fd);
        return fail(ErrorCode::PermissionDenied, "Mono socket owner descriptors unavailable");
    }
    bool owned = false;
    std::size_t count = 0;
    const auto expected = "socket:[" + std::to_string(*inode) + "]";
    while (auto* item = ::readdir(entries)) {
        if (++count > 65536 || !current())
            break;
        std::uint64_t descriptor = 0;
        if (!number(item->d_name, descriptor))
            continue;
        std::array<char, 128> target{};
        const auto length = ::readlinkat(fd, item->d_name, target.data(), target.size());
        if (length > 0 && static_cast<std::size_t>(length) < target.size() &&
            std::string_view(target.data(), static_cast<std::size_t>(length)) == expected) {
            owned = true;
            break;
        }
    }
    ::closedir(entries);
    auto after = readIdentity(*m_impl);
    if (!owned || !current() || !after || after->startTime != m_impl->identity.startTime ||
        after->image != m_impl->identity.image)
        return fail(ErrorCode::PermissionDenied, "reverse Mono socket is not owned by the held process");
    return {};
#endif
}
} // namespace frost::debug::mono
