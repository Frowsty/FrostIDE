// SPDX-License-Identifier: Apache-2.0
// Bounded positioned reads with retained native identity and mutation detection.
#include "random_access_reader.hpp"

#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"

#include <algorithm>
#include <cerrno>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>
#endif
namespace frost::io::detail {
namespace {
#ifdef _WIN32
using Native = HANDLE;
using Stamp = BY_HANDLE_FILE_INFORMATION;
bool same(const Stamp& a, const Stamp& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
           a.nFileIndexLow == b.nFileIndexLow && a.nFileSizeHigh == b.nFileSizeHigh &&
           a.nFileSizeLow == b.nFileSizeLow && CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
}
#else
using Native = int;
using Stamp = struct stat;
bool same(const Stamp& a, const Stamp& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
#endif
class Reader final : public IRandomAccessReader {
public:
    Reader(Native handle, std::string path, Stamp stamp, std::uint64_t size)
        : m_handle(handle), m_path(std::move(path)), m_stamp(stamp), m_size(size) {}
    ~Reader() override {
#ifdef _WIN32
        CloseHandle(m_handle);
#else
        close(m_handle);
#endif
    }
    std::uint64_t size() const noexcept override { return m_size; }
    Result<std::string> read(std::uint64_t offset, std::size_t count, std::stop_token stop) override {
        if (count > kMaximumReadBytes || offset > m_size)
            return fail(ErrorCode::OutOfRange, "Page exceeds reader bounds");
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Page cancelled");
        if (auto check = unchanged(); !check)
            return std::unexpected(check.error());
        std::string bytes(static_cast<std::size_t>(std::min<std::uint64_t>(count, m_size - offset)), '\0');
        std::size_t done = 0;
        while (done < bytes.size()) {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "Page cancelled");
            const auto amount = std::min<std::size_t>(4096, bytes.size() - done);
#ifdef _WIN32
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(offset + done);
            if (!SetFilePointerEx(m_handle, position, nullptr, FILE_BEGIN))
                return std::unexpected(lastWin32Error("seek page", m_path));
            DWORD got = 0;
            if (!ReadFile(m_handle, bytes.data() + done, static_cast<DWORD>(amount), &got, nullptr))
                return std::unexpected(lastWin32Error("read page", m_path));
#else
            const auto got = pread(m_handle, bytes.data() + done, amount, static_cast<off_t>(offset + done));
            if (got < 0) {
                if (errno == EINTR)
                    continue;
                return std::unexpected(errnoError(errno, "read page", m_path));
            }
#endif
            if (got == 0)
                return fail(ErrorCode::Busy, "File shortened during page read");
            done += static_cast<std::size_t>(got);
        }
        if (auto check = unchanged(); !check)
            return std::unexpected(check.error());
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Page cancelled");
        return bytes;
    }

private:
    Result<void> unchanged() {
        Stamp current{}, named{};
#ifdef _WIN32
        if (!GetFileInformationByHandle(m_handle, &current))
            return std::unexpected(lastWin32Error("page identity", m_path));
        auto wide = toWin32Path(m_path);
        if (!wide)
            return std::unexpected(wide.error());
        HANDLE probe =
            CreateFileW(wide->c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (probe == INVALID_HANDLE_VALUE)
            return std::unexpected(lastWin32Error("page path identity", m_path));
        const bool ok = GetFileInformationByHandle(probe, &named) != FALSE;
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(probe);
        if (!ok)
            return std::unexpected(win32Error(error, "page path identity", m_path));
#else
        if (fstat(m_handle, &current) != 0 || stat(m_path.c_str(), &named) != 0)
            return std::unexpected(errnoError(errno, "page identity", m_path));
#endif
        if (!same(m_stamp, current) || !same(m_stamp, named))
            return fail(ErrorCode::Busy, "File changed or was replaced; reopen the large-file view");
        return {};
    }
    Native m_handle;
    std::string m_path;
    Stamp m_stamp;
    std::uint64_t m_size;
};
} // namespace
Result<std::unique_ptr<IRandomAccessReader>> openNativeRandomAccess(std::string_view path) {
#ifdef _WIN32
    auto checked = toWin32Path(path);
    if (!checked)
        return std::unexpected(checked.error());
    const auto handle = CreateFileW(checked->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::unexpected(lastWin32Error("open pages", path));
    Stamp stamp{};
    if (!GetFileInformationByHandle(handle, &stamp)) {
        auto error = lastWin32Error("page status", path);
        CloseHandle(handle);
        return std::unexpected(error);
    }
    if (GetFileType(handle) != FILE_TYPE_DISK || (stamp.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(handle);
        return fail(ErrorCode::InvalidArgument, "Pages require a regular file");
    }
    const auto size = (std::uint64_t{stamp.nFileSizeHigh} << 32) | stamp.nFileSizeLow;
#else
    auto checked = normalizePath(path);
    if (!checked)
        return std::unexpected(checked.error());
    const int handle = open(checked->c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (handle < 0)
        return std::unexpected(errnoError(errno, "open pages", path));
    Stamp stamp{};
    if (fstat(handle, &stamp) != 0) {
        auto error = errnoError(errno, "page status", path);
        close(handle);
        return std::unexpected(error);
    }
    if (!S_ISREG(stamp.st_mode) || stamp.st_size < 0) {
        close(handle);
        return fail(ErrorCode::InvalidArgument, "Pages require a regular file");
    }
    const auto size = static_cast<std::uint64_t>(stamp.st_size);
#endif
    return std::unique_ptr<IRandomAccessReader>(new Reader(handle, std::string(path), stamp, size));
}
} // namespace frost::io::detail
