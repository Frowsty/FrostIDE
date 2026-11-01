// SPDX-License-Identifier: Apache-2.0
//
// The real IProcessLauncher on Windows: CreateProcessW with an explicit
// inherited-handle list, an optional kill-on-close Job Object, and overlapped
// named pipes for the parent's ends of the child's standard streams.
//
/*
 * Why named pipes: anonymous pipes (CreatePipe) cannot do overlapped I/O, so
 * draining stdout and stderr while feeding stdin would need one thread per
 * stream. A uniquely named, single-instance pipe whose server (parent) end is
 * opened with FILE_FLAG_OVERLAPPED lets communicate() multiplex all three on
 * one thread with WaitForMultipleObjects, and is what the M1 IOCP reactor
 * needs anyway. The client (child) end is an ordinary synchronous handle, as
 * console programs expect. The same construction is used by Rust's standard
 * library (library/std/src/sys/pal/windows/pipe.rs, MIT/Apache-2.0); this is
 * an independent implementation of the idea. Each name carries 128 random bits
 * from the system RNG, the pipe's DACL admits only the current user,
 * PIPE_REJECT_REMOTE_CLIENTS keeps other machines out, and
 * FILE_FLAG_FIRST_PIPE_INSTANCE defeats name squatters; the client end is
 * opened immediately, so the single instance is taken before anyone else
 * could connect.
 */

#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"

#include "internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <vector>

#include <windows.h>

#include <bcrypt.h>
#include <sddl.h>

namespace frost::io {
namespace {

using detail::UniqueHandle;

// Pipe buffer size in bytes, each direction; also the communicate() read chunk.
constexpr DWORD kPipeBufferBytes = 64 * 1024;
// How long communicate() waits for a terminated process to disappear before returning Timeout.
constexpr DWORD kReapGraceMs = 5000;

// Milliseconds for Wait* APIs: rounded up, clamped below INFINITE.
DWORD toWaitMs(IClock::Duration duration) noexcept {
    if (duration <= IClock::Duration::zero()) {
        return 0;
    }
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(duration).count();
    return static_cast<DWORD>(std::min<long long>(ms, INFINITE - 1));
}

bool isEndOfPipe(DWORD error) noexcept {
    return error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF || error == ERROR_NO_DATA ||
           error == ERROR_PIPE_NOT_CONNECTED;
}

// The parent's overlapped end and the child's synchronous, inheritable end of one pipe.
struct PipePair {
    UniqueHandle parent;
    UniqueHandle child;
};

// Frees memory that Win32 allocated with LocalAlloc (SDDL conversions).
struct LocalFreeDeleter {
    void operator()(void* memory) const noexcept { LocalFree(memory); }
};

/*
 * A security descriptor whose DACL grants full access to the current user (the
 * process token's user SID) and to nobody else, so other users' processes cannot
 * open a pipe even if they learn its name. "D:P" is a protected DACL (nothing is
 * inherited from the pipe file system's default).
 */
Result<std::unique_ptr<void, LocalFreeDeleter>> currentUserOnlyDescriptor() {
    HANDLE rawToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken) == FALSE) {
        return std::unexpected(lastWin32Error("OpenProcessToken"));
    }
    const UniqueHandle token(rawToken);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (size == 0 || GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) == FALSE) {
        return std::unexpected(lastWin32Error("GetTokenInformation(TokenUser)"));
    }
    wchar_t* rawSid = nullptr;
    if (ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid, &rawSid) == FALSE) {
        return std::unexpected(lastWin32Error("ConvertSidToStringSidW"));
    }
    const std::unique_ptr<wchar_t, LocalFreeDeleter> sid(rawSid);
    const std::wstring sddl = std::format(L"D:P(A;;GA;;;{})", sid.get());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) ==
        FALSE) {
        return std::unexpected(lastWin32Error("ConvertStringSecurityDescriptorToSecurityDescriptorW"));
    }
    return std::unique_ptr<void, LocalFreeDeleter>(descriptor);
}

// 128 random bits as hex, from the system RNG, so pipe names cannot be predicted.
Result<std::wstring> randomHex() {
    std::array<unsigned char, 16> bytes{};
    if (const NTSTATUS status =
            BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        status < 0) {
        return fail(ErrorCode::IoError,
                    std::format("BCryptGenRandom failed (NTSTATUS {:#x})", static_cast<std::uint32_t>(status)));
    }
    std::wstring out;
    for (const unsigned char b : bytes) {
        out += std::format(L"{:02x}", b);
    }
    return out;
}

// `parentReads` selects the direction: true for the child's stdout/stderr, false for its stdin.
Result<PipePair> createPipe(bool parentReads) {
    auto descriptor = currentUserOnlyDescriptor();
    if (!descriptor) {
        return std::unexpected(std::move(descriptor.error()));
    }
    SECURITY_ATTRIBUTES serverSecurity{sizeof(SECURITY_ATTRIBUTES), descriptor->get(), FALSE};
    for (int attempt = 0;; ++attempt) {
        auto suffix = randomHex();
        if (!suffix) {
            return std::unexpected(std::move(suffix.error()));
        }
        const std::wstring name = std::format(L"\\\\.\\pipe\\frost-io-{}-{}", detail::currentProcessId(), *suffix);
        const DWORD openMode = (parentReads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED |
                               FILE_FLAG_FIRST_PIPE_INSTANCE;
        UniqueHandle server(CreateNamedPipeW(
            name.c_str(), openMode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
            kPipeBufferBytes, kPipeBufferBytes, 0, &serverSecurity));
        if (!server.valid()) {
            const DWORD error = GetLastError();
            // FIRST_PIPE_INSTANCE reports a taken name as ERROR_ACCESS_DENIED; try another name.
            if (error == ERROR_ACCESS_DENIED && attempt < 8) {
                continue;
            }
            return std::unexpected(win32Error(error, "CreateNamedPipeW"));
        }
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        // FILE_READ/WRITE_ATTRIBUTES let the child query or switch the pipe's mode, as with CreatePipe handles.
        const DWORD access =
            parentReads ? (GENERIC_WRITE | FILE_READ_ATTRIBUTES) : (GENERIC_READ | FILE_WRITE_ATTRIBUTES);
        UniqueHandle client(CreateFileW(name.c_str(), access, 0, &inherit, OPEN_EXISTING, 0, nullptr));
        if (!client.valid()) {
            return std::unexpected(lastWin32Error("open pipe client"));
        }
        return PipePair{std::move(server), std::move(client)};
    }
}

// An inheritable handle to the null device.
Result<UniqueHandle> openNull() {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    UniqueHandle handle(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                    OPEN_EXISTING, 0, nullptr));
    if (!handle.valid()) {
        return std::unexpected(lastWin32Error("open NUL"));
    }
    return handle;
}

// An inheritable duplicate of the parent's standard handle, or the null device if there is none.
Result<UniqueHandle> inheritStd(DWORD which) {
    const HANDLE own = GetStdHandle(which);
    if (own == nullptr || own == INVALID_HANDLE_VALUE) {
        return openNull();
    }
    HANDLE duplicate = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), own, GetCurrentProcess(), &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS) ==
        FALSE) {
        return std::unexpected(lastWin32Error("DuplicateHandle(std handle)"));
    }
    return UniqueHandle(duplicate);
}

// One child stream: the handle the child gets, and (for pipes) the parent's end.
struct StreamSetup {
    UniqueHandle child;
    UniqueHandle parent;
};

Result<StreamSetup> setupStream(Stdio mode, DWORD stdHandle, bool parentReads) {
    switch (mode) {
    case Stdio::Pipe: {
        auto pipe = createPipe(parentReads);
        if (!pipe) {
            return std::unexpected(std::move(pipe.error()));
        }
        return StreamSetup{std::move(pipe->child), std::move(pipe->parent)};
    }
    case Stdio::Null: {
        auto null = openNull();
        if (!null) {
            return std::unexpected(std::move(null.error()));
        }
        return StreamSetup{std::move(*null), {}};
    }
    case Stdio::Inherit: {
        auto inherited = inheritStd(stdHandle);
        if (!inherited) {
            return std::unexpected(std::move(inherited.error()));
        }
        return StreamSetup{std::move(*inherited), {}};
    }
    case Stdio::MergeIntoStdout: break; // Handled by the caller (stderr only).
    }
    return fail(ErrorCode::Internal, "unexpected Stdio mode");
}

// The variable name of an environment entry "NAME=value". Names may start with '='
// (the per-drive current directories, "=C:=C:\dir"), so the search starts at 1.
std::wstring_view envName(std::wstring_view entry) noexcept {
    const std::size_t equals = entry.find(L'=', 1);
    return equals == std::wstring_view::npos ? entry : entry.substr(0, equals);
}

// Windows compares environment names case-insensitively, by ordinal (locale-independent) rules.
int compareNames(std::wstring_view a, std::wstring_view b) noexcept {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE);
}

// The CREATE_UNICODE_ENVIRONMENT block for `spec`: the parent's environment (unless
// cleanEnvironment) with the overrides applied, sorted case-insensitively by name as
// CreateProcessW documents, each entry NUL-terminated, plus a final NUL.
Result<std::wstring> buildEnvironmentBlock(const ProcessSpec& spec) {
    std::vector<std::wstring> entries;
    if (!spec.cleanEnvironment) {
        wchar_t* block = GetEnvironmentStringsW();
        if (block == nullptr) {
            return std::unexpected(lastWin32Error("GetEnvironmentStringsW"));
        }
        for (const wchar_t* p = block; *p != L'\0';) {
            const std::wstring_view entry(p);
            entries.emplace_back(entry);
            p += entry.size() + 1;
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : spec.environment) {
        auto wideName = toWide(name);
        if (!wideName) {
            return std::unexpected(std::move(wideName.error()).withContext("environment variable name"));
        }
        std::erase_if(entries,
                      [&](const std::wstring& entry) { return compareNames(envName(entry), *wideName) == CSTR_EQUAL; });
        if (value) {
            auto wideValue = toWide(*value);
            if (!wideValue) {
                return std::unexpected(std::move(wideValue.error()).withContext("environment variable " + name));
            }
            entries.push_back(*wideName + L"=" + *wideValue);
        }
    }
    std::ranges::sort(entries, [](const std::wstring& a, const std::wstring& b) {
        return compareNames(envName(a), envName(b)) == CSTR_LESS_THAN;
    });
    std::wstring out;
    for (const std::wstring& entry : entries) {
        out += entry;
        out += L'\0';
    }
    if (entries.empty()) {
        out += L'\0'; // An empty block is two NULs.
    }
    out += L'\0';
    return out;
}

// Plain form below MAX_PATH (what children expect to see as their image path and current
// directory), extended-length form beyond it.
Result<std::wstring> launchPath(const std::string& normalized) {
    auto wide = toWide(normalized);
    if (!wide) {
        return wide;
    }
    if (wide->size() < MAX_PATH) {
        return wide;
    }
    return toWin32Path(normalized);
}

/*
 * Explains CreateProcessW's ERROR_DIRECTORY ("the directory name is invalid") for the
 * working directory `normalized`: NotFound if it does not exist, InvalidArgument if it is
 * not a directory or is too long. Windows keeps a process's current directory in a
 * MAX_PATH buffer unless long paths are enabled system-wide, so with LongPathsEnabled = 0
 * (observed 2026-10-31) a directory beyond it cannot be a child's current directory even
 * though frost::io can create and use it.
 * TODO(M1): fall back to the directory's 8.3 short name (GetShortPathNameW) when it has one.
 */
Error workingDirectoryError(const ProcessSpec& spec, const std::string& normalized) {
    auto extended = toWin32Path(normalized);
    const DWORD attributes = extended ? GetFileAttributesW(extended->c_str()) : INVALID_FILE_ATTRIBUTES;
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return Error(ErrorCode::NotFound,
                     std::format("launch '{}': working directory '{}' does not exist", spec.executable, normalized));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return Error(ErrorCode::InvalidArgument, std::format("launch '{}': working directory '{}' is not a directory",
                                                             spec.executable, normalized));
    }
    const auto wide = toWide(normalized);
    const std::size_t units = wide ? wide->size() : normalized.size();
    if (units >= MAX_PATH - 12) {
        return Error(ErrorCode::InvalidArgument,
                     std::format("launch '{}': working directory '{}' is {} UTF-16 units long; Windows limits a "
                                 "process's current directory to MAX_PATH ({}) unless long paths are enabled",
                                 spec.executable, normalized, units, MAX_PATH));
    }
    return win32Error(ERROR_DIRECTORY, "launch", spec.executable).withContext("working directory '" + normalized + "'");
}

/*
 * Defence in depth for the batch-file refusal (see process.hpp): CreateProcessW
 * resolves a non-verbatim application name the way GetFullPathNameW does
 * (resolving "." and "..", dropping trailing dots and spaces) and decides on
 * cmd.exe from the result, so the check runs on exactly that string. A \\?\ name
 * is used literally and was already checked by validateProcessSpec.
 */
Result<void> checkNotBatch(const std::wstring& applicationName, const std::string& executable) {
    std::wstring resolved = applicationName;
    if (!applicationName.starts_with(L"\\\\?\\")) {
        DWORD size = MAX_PATH;
        for (;;) {
            resolved.assign(size, L'\0');
            const DWORD length = GetFullPathNameW(applicationName.c_str(), size, resolved.data(), nullptr);
            if (length == 0) {
                return std::unexpected(lastWin32Error("GetFullPathNameW", executable));
            }
            if (length < size) {
                resolved.resize(length);
                break;
            }
            size = length; // Too small: `length` includes the terminator.
        }
    }
    auto utf8 = fromWide(resolved);
    if (!utf8) {
        return std::unexpected(std::move(utf8.error()).withContext("executable"));
    }
    if (detail::isBatchFileName(*utf8)) {
        return fail(ErrorCode::Unsupported,
                    std::format("'{}' resolves to the batch file '{}'; batch files are not launched directly",
                                executable, *utf8));
    }
    return {};
}

class WindowsProcess final : public IProcess {
public:
    WindowsProcess(UniqueHandle process, UniqueHandle job, ProcessId pid, UniqueHandle in, UniqueHandle out,
                   UniqueHandle err)
        : m_process(std::move(process)), m_job(std::move(job)), m_pid(pid), m_in(std::move(in)), m_out(std::move(out)),
          m_err(std::move(err)) {}

    // Members close in reverse order: pipes first, then the job (which ends the tree when
    // killOnClose was set), then the process handle.
    ~WindowsProcess() override = default;

    ProcessId pid() const noexcept override { return m_pid; }

    Result<void> writeStdin(std::string_view bytes) override {
        if (!m_in.pipe.valid()) {
            return fail(ErrorCode::InvalidArgument, "stdin is not an open pipe");
        }
        while (!bytes.empty()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), kPipeBufferBytes));
            auto done = blockingIo(m_in, /*read=*/false, const_cast<char*>(bytes.data()), chunk);
            if (!done) {
                if (isEndOfPipe(done.error())) {
                    return fail(ErrorCode::IoError, "writeStdin: the child closed its stdin");
                }
                return std::unexpected(win32Error(done.error(), "writeStdin"));
            }
            bytes.remove_prefix(*done);
        }
        return {};
    }

    void closeStdin() noexcept override { m_in.pipe.reset(); }

    Result<std::size_t> readStdout(std::span<char> buffer) override {
        return blockingRead(m_out, buffer, "readStdout");
    }
    Result<std::size_t> readStderr(std::span<char> buffer) override {
        return blockingRead(m_err, buffer, "readStderr");
    }

    Result<ExitStatus> wait(std::optional<IClock::Duration> timeout) override {
        const DWORD ms = timeout ? toWaitMs(*timeout) : INFINITE;
        switch (WaitForSingleObject(m_process.get(), ms)) {
        case WAIT_OBJECT_0: return exitStatus();
        case WAIT_TIMEOUT: return fail(ErrorCode::Timeout, std::format("process {} is still running", m_pid));
        default: return std::unexpected(lastWin32Error("WaitForSingleObject(process)"));
        }
    }

    Result<void> terminate() override {
        // The job form also ends descendants that outlived the child itself.
        if (m_job.valid()) {
            if (TerminateJobObject(m_job.get(), static_cast<UINT>(kTerminatedExitCode)) == FALSE) {
                return std::unexpected(lastWin32Error("TerminateJobObject"));
            }
            return {};
        }
        if (WaitForSingleObject(m_process.get(), 0) == WAIT_OBJECT_0) {
            return {};
        }
        if (TerminateProcess(m_process.get(), static_cast<UINT>(kTerminatedExitCode)) == FALSE) {
            const DWORD error = GetLastError();
            // ERROR_ACCESS_DENIED also means "already exiting"; only report it if the process lives on.
            if (WaitForSingleObject(m_process.get(), 0) == WAIT_OBJECT_0) {
                return {};
            }
            return std::unexpected(win32Error(error, "TerminateProcess"));
        }
        return {};
    }

    Result<ProcessResult> communicate(std::string_view stdinData, IClock::Duration timeout) override;

private:
    // A parent-side pipe end with the event its overlapped operations signal.
    struct Stream {
        explicit Stream(UniqueHandle handle)
            : pipe(std::move(handle)), event(pipe.valid() ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr) {}
        UniqueHandle pipe;
        UniqueHandle event;
        bool eof = false;
    };

    // One overlapped ReadFile/WriteFile, waited for to completion. Returns the byte count or
    // the Win32 error code.
    static std::expected<DWORD, DWORD> blockingIo(Stream& stream, bool read, char* data, DWORD size) noexcept {
        if (!stream.event.valid()) {
            return std::unexpected(static_cast<DWORD>(ERROR_INVALID_HANDLE));
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = stream.event.get();
        const BOOL started = read ? ReadFile(stream.pipe.get(), data, size, nullptr, &overlapped)
                                  : WriteFile(stream.pipe.get(), data, size, nullptr, &overlapped);
        if (started == FALSE && GetLastError() != ERROR_IO_PENDING) {
            return std::unexpected(GetLastError());
        }
        DWORD done = 0;
        if (GetOverlappedResult(stream.pipe.get(), &overlapped, &done, TRUE) == FALSE) {
            return std::unexpected(GetLastError());
        }
        return done;
    }

    static Result<std::size_t> blockingRead(Stream& stream, std::span<char> buffer, std::string_view operation) {
        if (!stream.pipe.valid()) {
            return fail(ErrorCode::InvalidArgument, std::format("{}: the stream is not a pipe", operation));
        }
        while (!stream.eof && !buffer.empty()) {
            const DWORD size = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), kPipeBufferBytes));
            auto got = blockingIo(stream, /*read=*/true, buffer.data(), size);
            if (!got) {
                if (isEndOfPipe(got.error())) {
                    stream.eof = true;
                    break;
                }
                return std::unexpected(win32Error(got.error(), operation));
            }
            // A zero-byte read only means the child wrote zero bytes; EOF is ERROR_BROKEN_PIPE.
            if (*got > 0) {
                return std::size_t{*got};
            }
        }
        return std::size_t{0};
    }

    Result<ExitStatus> exitStatus() {
        DWORD code = 0;
        if (GetExitCodeProcess(m_process.get(), &code) == FALSE) {
            return std::unexpected(lastWin32Error("GetExitCodeProcess"));
        }
        return ExitStatus{static_cast<int>(code), 0};
    }

    UniqueHandle m_process;
    UniqueHandle m_job;
    ProcessId m_pid;
    Stream m_in;
    Stream m_out;
    Stream m_err;
};

/*
 * communicate() keeps at most one overlapped operation in flight per stream and
 * waits on their events together with WaitForMultipleObjects, so a child that
 * fills stdout while we are still feeding stdin (or fills both output pipes)
 * cannot deadlock us. Each OVERLAPPED lives in a Channel on this stack frame;
 * before returning, every pending operation is cancelled and waited for, so
 * the kernel never writes into a dead frame.
 */
Result<ProcessResult> WindowsProcess::communicate(std::string_view stdinData, IClock::Duration timeout) {
    if (!stdinData.empty() && !m_in.pipe.valid()) {
        return fail(ErrorCode::InvalidArgument, "communicate: stdin data given but stdin is not an open pipe");
    }
    const auto deadline = detail::deadlineAfter(timeout);

    struct Channel {
        Stream* stream = nullptr;
        bool read = true;
        bool active = false;
        bool pending = false;
        OVERLAPPED overlapped{};
        std::vector<char> buffer;
        std::string* sink = nullptr;
    };
    ProcessResult result;
    std::array<Channel, 3> channels;
    Channel& in = channels[0];
    Channel& out = channels[1];
    Channel& err = channels[2];
    in.stream = &m_in;
    in.read = false;
    in.active = m_in.pipe.valid() && !stdinData.empty();
    out.stream = &m_out;
    out.active = m_out.pipe.valid() && !m_out.eof;
    out.sink = &result.stdoutBytes;
    err.stream = &m_err;
    err.active = m_err.pipe.valid() && !m_err.eof;
    err.sink = &result.stderrBytes;
    if (!in.active) {
        closeStdin();
    }
    std::size_t inOffset = 0;

    // Cancels whatever is still in flight and waits for the cancellations to land.
    const auto cancelPending = [&] {
        for (Channel& channel : channels) {
            if (channel.pending) {
                CancelIoEx(channel.stream->pipe.get(), &channel.overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(channel.stream->pipe.get(), &channel.overlapped, &ignored, TRUE);
                channel.pending = false;
            }
        }
    };
    const auto failWith = [&](Error error) -> Result<ProcessResult> {
        cancelPending();
        return std::unexpected(std::move(error));
    };
    const auto timedOut = [&]() -> Result<ProcessResult> {
        cancelPending();
        static_cast<void>(terminate());
        WaitForSingleObject(m_process.get(), kReapGraceMs);
        return fail(ErrorCode::Timeout,
                    std::format("process {} did not finish within {} ms and was terminated", m_pid,
                                std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()));
    };
    // Ends the stdin channel: everything written, or the child stopped reading.
    const auto finishStdin = [&] {
        in.active = false;
        closeStdin();
    };

    while (in.active || out.active || err.active) {
        // Start an operation on every idle channel.
        for (Channel& channel : channels) {
            if (!channel.active || channel.pending) {
                continue;
            }
            channel.overlapped = OVERLAPPED{};
            channel.overlapped.hEvent = channel.stream->event.get();
            BOOL started = FALSE;
            if (channel.read) {
                channel.buffer.resize(kPipeBufferBytes);
                started = ReadFile(channel.stream->pipe.get(), channel.buffer.data(), kPipeBufferBytes, nullptr,
                                   &channel.overlapped);
            } else {
                const DWORD chunk =
                    static_cast<DWORD>(std::min<std::size_t>(stdinData.size() - inOffset, kPipeBufferBytes));
                started = WriteFile(channel.stream->pipe.get(), stdinData.data() + inOffset, chunk, nullptr,
                                    &channel.overlapped);
            }
            // Synchronous completions also signal the event, so both cases are collected below.
            if (started != FALSE || GetLastError() == ERROR_IO_PENDING) {
                channel.pending = true;
                continue;
            }
            const DWORD error = GetLastError();
            if (!isEndOfPipe(error)) {
                return failWith(win32Error(error, channel.read ? "communicate: read" : "communicate: write"));
            }
            if (channel.read) {
                channel.active = false;
                channel.stream->eof = true;
            } else {
                finishStdin();
            }
        }

        std::array<HANDLE, 3> events{};
        std::array<Channel*, 3> waiting{};
        DWORD count = 0;
        for (Channel& channel : channels) {
            if (channel.pending) {
                events[count] = channel.overlapped.hEvent;
                waiting[count] = &channel;
                ++count;
            }
        }
        if (count == 0) {
            continue;
        }
        const auto left = deadline - std::chrono::steady_clock::now();
        if (left <= IClock::Duration::zero()) {
            return timedOut();
        }
        const DWORD signalled = WaitForMultipleObjects(count, events.data(), FALSE, toWaitMs(left));
        if (signalled == WAIT_TIMEOUT) {
            return timedOut();
        }
        if (signalled >= WAIT_OBJECT_0 + count) {
            return failWith(lastWin32Error("communicate: WaitForMultipleObjects"));
        }
        Channel& channel = *waiting[signalled - WAIT_OBJECT_0];
        DWORD done = 0;
        const BOOL ok = GetOverlappedResult(channel.stream->pipe.get(), &channel.overlapped, &done, FALSE);
        channel.pending = false;
        if (ok == FALSE) {
            const DWORD error = GetLastError();
            if (!isEndOfPipe(error)) {
                return failWith(win32Error(error, channel.read ? "communicate: read" : "communicate: write"));
            }
            if (channel.read) {
                channel.active = false;
                channel.stream->eof = true;
            } else {
                finishStdin(); // The child closed stdin early; the rest of the data is dropped.
            }
            continue;
        }
        if (channel.read) {
            channel.sink->append(channel.buffer.data(), done);
        } else {
            inOffset += done;
            if (inOffset >= stdinData.size()) {
                finishStdin();
            }
        }
    }

    const auto left = deadline - std::chrono::steady_clock::now();
    if (WaitForSingleObject(m_process.get(), toWaitMs(left)) != WAIT_OBJECT_0) {
        return timedOut();
    }
    auto status = exitStatus();
    if (!status) {
        return std::unexpected(std::move(status.error()));
    }
    result.exitCode = status->exitCode;
    result.signal = status->signal;
    return result;
}

class WindowsProcessLauncher final : public IProcessLauncher {
public:
    constexpr WindowsProcessLauncher() noexcept = default;

    Result<std::unique_ptr<IProcess>> launch(const ProcessSpec& spec) override {
        if (auto valid = validateProcessSpec(spec, /*windowsRules=*/true); !valid) {
            return std::unexpected(std::move(valid.error()));
        }
        auto executable = normalizePath(spec.executable, PathStyle::Windows);
        if (!executable) {
            return std::unexpected(std::move(executable.error()));
        }
        auto applicationName = launchPath(*executable);
        if (!applicationName) {
            return std::unexpected(std::move(applicationName.error()));
        }
        if (auto notBatch = checkNotBatch(*applicationName, spec.executable); !notBatch) {
            return std::unexpected(std::move(notBatch.error()));
        }
        // validateProcessSpec has checked that the command line builds and fits the limit.
        auto utf8CommandLine = buildWindowsCommandLine(*executable, spec.arguments);
        if (!utf8CommandLine) {
            return std::unexpected(std::move(utf8CommandLine.error()));
        }
        auto commandLine = toWide(*utf8CommandLine);
        if (!commandLine) {
            return std::unexpected(std::move(commandLine.error()).withContext("command line"));
        }
        std::string normalizedDirectory;
        std::wstring workingDirectory;
        if (!spec.workingDirectory.empty()) {
            auto normalized = normalizePath(spec.workingDirectory, PathStyle::Windows);
            auto wide =
                normalized ? launchPath(*normalized) : Result<std::wstring>(std::unexpected(normalized.error()));
            if (!wide) {
                return std::unexpected(std::move(wide.error()));
            }
            normalizedDirectory = std::move(*normalized);
            workingDirectory = std::move(*wide);
        }
        std::wstring environment;
        const bool customEnvironment = spec.cleanEnvironment || !spec.environment.empty();
        if (customEnvironment) {
            auto block = buildEnvironmentBlock(spec);
            if (!block) {
                return std::unexpected(std::move(block.error()));
            }
            environment = std::move(*block);
        }

        auto in = setupStream(spec.stdinMode, STD_INPUT_HANDLE, /*parentReads=*/false);
        if (!in) {
            return std::unexpected(std::move(in.error()));
        }
        auto out = setupStream(spec.stdoutMode, STD_OUTPUT_HANDLE, /*parentReads=*/true);
        if (!out) {
            return std::unexpected(std::move(out.error()));
        }
        StreamSetup err;
        if (spec.stderrMode != Stdio::MergeIntoStdout) {
            auto setup = setupStream(spec.stderrMode, STD_ERROR_HANDLE, /*parentReads=*/true);
            if (!setup) {
                return std::unexpected(std::move(setup.error()));
            }
            err = std::move(*setup);
        }
        const HANDLE childErr = spec.stderrMode == Stdio::MergeIntoStdout ? out->child.get() : err.child.get();

        UniqueHandle job;
        if (spec.killOnClose) {
            job.reset(CreateJobObjectW(nullptr, nullptr));
            if (!job.valid()) {
                return std::unexpected(lastWin32Error("CreateJobObjectW"));
            }
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ==
                FALSE) {
                return std::unexpected(lastWin32Error("SetInformationJobObject"));
            }
        }

        // Exactly these handles are inherited (each once; duplicates are an error for the API).
        std::vector<HANDLE> inherited{in->child.get(), out->child.get()};
        if (std::ranges::find(inherited, childErr) == inherited.end()) {
            inherited.push_back(childErr);
        }
        const DWORD attributeCount = job.valid() ? 2 : 1;
        SIZE_T listSize = 0;
        InitializeProcThreadAttributeList(nullptr, attributeCount, 0, &listSize);
        std::vector<std::byte> listStorage(listSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(listStorage.data());
        if (InitializeProcThreadAttributeList(attributes, attributeCount, 0, &listSize) == FALSE) {
            return std::unexpected(lastWin32Error("InitializeProcThreadAttributeList"));
        }
        struct ListGuard {
            LPPROC_THREAD_ATTRIBUTE_LIST list;
            ~ListGuard() { DeleteProcThreadAttributeList(list); }
        } listGuard{attributes};
        if (UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                      inherited.size() * sizeof(HANDLE), nullptr, nullptr) == FALSE) {
            return std::unexpected(lastWin32Error("UpdateProcThreadAttribute(HANDLE_LIST)"));
        }
        // The job is attached at creation, so not even the child's first instruction runs outside it.
        HANDLE jobHandle = job.get();
        if (job.valid() && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &jobHandle,
                                                     sizeof(HANDLE), nullptr, nullptr) == FALSE) {
            return std::unexpected(lastWin32Error("UpdateProcThreadAttribute(JOB_LIST)"));
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = in->child.get();
        startup.StartupInfo.hStdOutput = out->child.get();
        startup.StartupInfo.hStdError = childErr;
        startup.lpAttributeList = attributes;
        DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
        // A GUI parent has no console; without this flag every console tool would open a window.
        // A console parent shares its console, so inherited console handles stay valid.
        if (GetConsoleWindow() == nullptr) {
            flags |= CREATE_NO_WINDOW;
        }
        // TODO(M1): log applicationName, the working directory and the command line (PLAN.md 3.5).
        PROCESS_INFORMATION info{};
        if (CreateProcessW(applicationName->c_str(), commandLine->data(), nullptr, nullptr, TRUE, flags,
                           customEnvironment ? environment.data() : nullptr,
                           workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup.StartupInfo,
                           &info) == FALSE) {
            const DWORD error = GetLastError();
            if (error == ERROR_DIRECTORY && !spec.workingDirectory.empty()) {
                return std::unexpected(workingDirectoryError(spec, normalizedDirectory));
            }
            return std::unexpected(win32Error(error, "launch", spec.executable));
        }
        CloseHandle(info.hThread);
        // The child ends (in/out/err .child) close when this function returns, so EOF arrives
        // as soon as the child and its descendants close theirs.
        return std::make_unique<WindowsProcess>(UniqueHandle(info.hProcess), std::move(job),
                                                static_cast<ProcessId>(info.dwProcessId), std::move(in->parent),
                                                std::move(out->parent), std::move(err.parent));
    }
};

// constinit and trivially destructible: no code runs before main or after exit.
constinit WindowsProcessLauncher g_realLauncher;

} // namespace

IProcessLauncher& realProcessLauncher() noexcept { return g_realLauncher; }

} // namespace frost::io
