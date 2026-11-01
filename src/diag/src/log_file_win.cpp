// SPDX-License-Identifier: Apache-2.0
//
// Windows log files (frost/diag/log_backend.hpp): CreateFileW on the \\?\ path with CREATE_NEW,
// FILE_APPEND_DATA and FILE_SHARE_READ only, so other processes can read a running log but not
// delete or write it; each record is one WriteFile call, without user-space buffering.

#include "frost/diag/log_backend.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"

#include <windows.h>

namespace frost::diag {
namespace {

class WinLogFile final : public ILogFile {
public:
    explicit WinLogFile(HANDLE handle) noexcept : m_handle(handle) {}
    WinLogFile(const WinLogFile&) = delete ("owns a file handle");
    WinLogFile& operator=(const WinLogFile&) = delete ("owns a file handle");
    ~WinLogFile() override { CloseHandle(m_handle); }

    Result<void> write(std::string_view bytes) noexcept override {
        while (!bytes.empty()) {
            const DWORD chunk = bytes.size() > 0x40000000u ? 0x40000000u : static_cast<DWORD>(bytes.size());
            DWORD wrote = 0;
            if (WriteFile(m_handle, bytes.data(), chunk, &wrote, nullptr) == FALSE || wrote == 0) {
                return fail(ErrorCode::IoError, "WriteFile on a log file failed");
            }
            bytes.remove_prefix(wrote);
        }
        return {};
    }

private:
    HANDLE m_handle;
};

class WinLogFileOpener final : public ILogFileOpener {
public:
    constexpr WinLogFileOpener() noexcept = default;

    Result<std::unique_ptr<ILogFile>> create(const std::string& path) override {
        auto wide = io::toWin32Path(path);
        if (!wide) {
            return std::unexpected(std::move(wide.error()));
        }
        const HANDLE handle = CreateFileW(wide->c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return std::unexpected(io::lastWin32Error("create log file", path));
        }
        return std::unique_ptr<ILogFile>(new WinLogFile(handle));
    }

    void remove(const std::string& path) noexcept override {
        if (auto wide = io::toWin32Path(path)) {
            (void)DeleteFileW(wide->c_str());
        }
    }
};

constinit WinLogFileOpener g_opener;

} // namespace

ILogFileOpener& realLogFileOpener() noexcept { return g_opener; }

} // namespace frost::diag
