// SPDX-License-Identifier: Apache-2.0
// Capture bounded source bytes through no-link Linux ancestry and an actual kernel read lease.
// The parent owns cancellation/timeout and must terminate this isolated helper when admission ends.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/stat.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
namespace {
struct Held {
    std::vector<int> values;
    ~Held() {
        for (int fd : values)
            ::close(fd);
    }
};
bool valid(std::string_view path) {
    if (path.size() < 2 || path.size() > 32767 || path.front() != '/' || path.back() == '/')
        return false;
    std::size_t offset = 1;
    while (offset < path.size()) {
        auto end = path.find('/', offset);
        if (end == path.npos)
            end = path.size();
        const auto part = path.substr(offset, end - offset);
        if (part.empty() || part == "." || part == "..")
            return false;
        offset = end + 1;
    }
    return true;
}
bool canonical(int fd, std::string_view expected) {
    std::array<char, 32768> path{};
    const auto count = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), path.data(), path.size());
    return count > 0 && static_cast<std::size_t>(count) < path.size() &&
           std::string_view(path.data(), static_cast<std::size_t>(count)) == expected;
}
bool same(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size && a.st_nlink == b.st_nlink &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 4)
        return 2;
    const std::string root(argv[1]), path(argv[2]);
    std::size_t limit = 0;
    const std::string_view argument(argv[3]);
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), limit);
    if (!valid(root) || !valid(path) || path.size() <= root.size() || path[root.size()] != '/' ||
        path.compare(0, root.size(), root) != 0 || parsed.ec != std::errc{} ||
        parsed.ptr != argument.data() + argument.size() || limit > 4 * 1024 * 1024)
        return 2;
    // fcntl(2) read leases prevent concurrent writable opens/truncation. Block SIGIO before acquiring
    // the lease, and refuse output if a lease-break request arrives. Unsupported filesystems refuse.
    sigset_t blocked{};
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGIO);
    if (::sigprocmask(SIG_BLOCK, &blocked, nullptr) != 0)
        return 1;
    Held held;
    int parent = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return 1;
    held.values.push_back(parent);
    int rootFd = -1;
    std::size_t offset = 1;
    for (;;) {
        auto end = path.find('/', offset);
        const bool directory = end != path.npos;
        if (!directory)
            end = path.size();
        const auto part = path.substr(offset, end - offset);
        const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (directory ? O_DIRECTORY : 0);
        parent = ::openat(parent, part.c_str(), flags);
        if (parent < 0)
            return 1;
        held.values.push_back(parent);
        if (!canonical(parent, std::string_view(path).substr(0, end)))
            return 1;
        if (end == root.size())
            rootFd = parent;
        if (!directory)
            break;
        offset = end + 1;
    }
    struct stat before{}, after{};
    if (rootFd < 0 || ::fstat(parent, &before) != 0 || !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
        before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > limit ||
        ::fcntl(parent, F_SETLEASE, F_RDLCK) != 0)
        return 1;
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offsetBytes = 0;
    while (offsetBytes < bytes.size()) {
        const auto got = ::read(parent, bytes.data() + offsetBytes, bytes.size() - offsetBytes);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return 1;
        offsetBytes += static_cast<std::size_t>(got);
    }
    timespec immediate{};
    if (::sigtimedwait(&blocked, nullptr, &immediate) >= 0 || errno != EAGAIN ||
        ::fcntl(parent, F_GETLEASE) != F_RDLCK || ::fstat(parent, &after) != 0 || !same(before, after) ||
        !canonical(rootFd, root) || !canonical(parent, path))
        return 1;
    // Keep every ancestor and the lease held through output; a partial-output failure is a failed capture.
    offsetBytes = 0;
    while (offsetBytes < bytes.size()) {
        const auto count = ::write(STDOUT_FILENO, bytes.data() + offsetBytes, bytes.size() - offsetBytes);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return 1;
        offsetBytes += static_cast<std::size_t>(count);
    }
    if (::sigtimedwait(&blocked, nullptr, &immediate) >= 0 || errno != EAGAIN ||
        ::fcntl(parent, F_GETLEASE) != F_RDLCK || !canonical(rootFd, root) || !canonical(parent, path))
        return 1;
    return 0;
}
