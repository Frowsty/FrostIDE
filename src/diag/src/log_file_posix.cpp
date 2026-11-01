// SPDX-License-Identifier: Apache-2.0
//
// POSIX log files (frost/diag/log_backend.hpp): open(2) with O_WRONLY | O_APPEND | O_CREAT |
// O_EXCL | O_CLOEXEC | O_NOFOLLOW and mode 0600, then a shared flock that retention probes
// (frost/diag/retention.hpp); each record is one write(2), without user-space buffering.

#include "frost/diag/log_backend.hpp"
#include "frost/io/os_error.hpp"

#include <sys/file.h>

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace frost::diag {
namespace {

class PosixLogFile final : public ILogFile {
public:
    explicit PosixLogFile(int fd) noexcept : m_fd(fd) {}
    PosixLogFile(const PosixLogFile&) = delete ("owns a descriptor");
    PosixLogFile& operator=(const PosixLogFile&) = delete ("owns a descriptor");
    ~PosixLogFile() override { ::close(m_fd); }

    Result<void> write(std::string_view bytes) noexcept override {
        while (!bytes.empty()) {
            const ssize_t wrote = ::write(m_fd, bytes.data(), bytes.size());
            if (wrote < 0 && errno == EINTR) {
                continue;
            }
            if (wrote <= 0) {
                return fail(ErrorCode::IoError, "write on a log file failed");
            }
            bytes.remove_prefix(static_cast<std::size_t>(wrote));
        }
        return {};
    }

private:
    int m_fd;
};

class PosixLogFileOpener final : public ILogFileOpener {
public:
    constexpr PosixLogFileOpener() noexcept = default;

    Result<std::unique_ptr<ILogFile>> create(const std::string& path) override {
        const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            return std::unexpected(io::errnoError(errno, "create log file", path));
        }
        // Shared, so retention's exclusive probe fails while this process runs (LOCK_NB below
        // cannot fail on a file nobody else has opened yet, but is non-blocking regardless).
        (void)::flock(fd, LOCK_SH | LOCK_NB);
        return std::unique_ptr<ILogFile>(new PosixLogFile(fd));
    }

    void remove(const std::string& path) noexcept override { (void)::unlink(path.c_str()); }
};

constinit PosixLogFileOpener g_opener;

} // namespace

ILogFileOpener& realLogFileOpener() noexcept { return g_opener; }

} // namespace frost::diag
