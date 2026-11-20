// SPDX-License-Identifier: Apache-2.0
// Retain and revalidate a selected local process before netcoredbg receives its attach request.
#include "frost/debug/coreclr_dap.hpp"
#include "frost/io/path.hpp"

#include <limits>
#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#else
#include <sys/stat.h>

#include <array>
#include <charconv>
#include <fstream>

#include <unistd.h>
#endif

namespace frost::debug {
namespace {
#ifndef _WIN32
Result<std::uint64_t> processStart(std::uint32_t pid, const std::string& executable) {
    const auto root = "/proc/" + std::to_string(pid);
    struct stat status{};
    if (::stat(root.c_str(), &status) || status.st_uid != ::geteuid())
        return fail(ErrorCode::PermissionDenied, "CoreCLR process must belong to the current user");
    std::array<char, 32768> image{};
    const auto length = ::readlink((root + "/exe").c_str(), image.data(), image.size());
    if (length <= 0 || static_cast<std::size_t>(length) >= image.size() ||
        std::string_view(image.data(), static_cast<std::size_t>(length)) != executable)
        return fail(ErrorCode::PermissionDenied, "CoreCLR process image changed or is unavailable");
    std::ifstream input(root + "/stat");
    std::array<char, 8192> bytes{};
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad() || !input.eof() || input.gcount() <= 0)
        return fail(ErrorCode::IoError, "CoreCLR process identity is unavailable");
    std::string_view fields(bytes.data(), static_cast<std::size_t>(input.gcount()));
    // comm can contain spaces and ')'; the last ')' delimits it. Field 22 is starttime.
    const auto end = fields.rfind(')');
    if (end == std::string_view::npos)
        return fail(ErrorCode::Corrupt, "CoreCLR process identity is malformed");
    fields.remove_prefix(end + 1);
    for (int field = 3; field <= 22; ++field) {
        const auto begin = fields.find_first_not_of(' ');
        if (begin == std::string_view::npos)
            return fail(ErrorCode::Corrupt, "CoreCLR process identity is incomplete");
        fields.remove_prefix(begin);
        const auto boundary = fields.find(' ');
        const auto token = fields.substr(0, boundary);
        if (field == 22) {
            std::uint64_t start = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), start);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || start == 0)
                return fail(ErrorCode::Corrupt, "CoreCLR process start time is invalid");
            return start;
        }
        if (boundary == std::string_view::npos)
            return fail(ErrorCode::Corrupt, "CoreCLR process identity is incomplete");
        fields.remove_prefix(boundary + 1);
    }
    return fail(ErrorCode::Corrupt, "CoreCLR process identity is incomplete");
}
#endif
} // namespace

Result<std::function<Result<void>()>> coreClrProcessAdmission(std::uint32_t pid, std::string executable) {
    if (!pid || !io::isAbsolutePath(executable) || executable.find('\0') != std::string::npos)
        return fail(ErrorCode::InvalidArgument, "CoreCLR attach requires a PID and absolute process image");
#ifdef _WIN32
    auto expected = io::toWide(executable);
    if (!expected)
        return std::unexpected(std::move(expected.error()));
    auto process = std::shared_ptr<void>(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid),
                                         [](void* handle) {
                                             if (handle)
                                                 CloseHandle(handle);
                                         });
    if (!process)
        return fail(ErrorCode::PermissionDenied, "CoreCLR process is unavailable");
    std::function<Result<void>()> admission = [process = std::move(process), expected = std::move(*expected)] {
        if (WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
            return Result<void>(fail(ErrorCode::NotFound, "CoreCLR process exited"));
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length) ||
            CompareStringOrdinal(path.data(), static_cast<int>(length), expected.data(),
                                 static_cast<int>(expected.size()), TRUE) != CSTR_EQUAL)
            return Result<void>(fail(ErrorCode::PermissionDenied, "CoreCLR process image changed"));
        return Result<void>{};
    };
#else
    auto start = processStart(pid, executable);
    if (!start)
        return std::unexpected(std::move(start.error()));
    std::function<Result<void>()> admission = [pid, executable = std::move(executable), start = *start] {
        auto current = processStart(pid, executable);
        if (!current)
            return Result<void>(std::unexpected(std::move(current.error())));
        if (*current != start)
            return Result<void>(fail(ErrorCode::PermissionDenied, "CoreCLR process identity changed"));
        return Result<void>{};
    };
#endif
    if (auto allowed = admission(); !allowed)
        return std::unexpected(std::move(allowed.error()));
    return admission;
}
} // namespace frost::debug
