// SPDX-License-Identifier: Apache-2.0
//
// The Windows pseudo terminal (frost/term/pty.hpp): bundled ConPTY, with system fallback. CreatePseudoConsole gets two
// anonymous pipes (our ends: the input's write end and the output's read end), the child starts on it
// through io::IProcessLauncher (ProcessSpec::pseudoTerminal), and a dedicated thread drains the output.
/*
 * Closing (Microsoft's "Creating a Pseudoconsole session" and the ClosePseudoConsole reference):
 * ClosePseudoConsole ends the console host and the clients still attached to it, and may wait until
 * the host's final output has been read, so the reader thread keeps draining while it runs. The host's
 * exit breaks both pipes: the reader then sees ERROR_BROKEN_PIPE (end of output) and a writer blocked
 * on a full input pipe fails. The shell's own tree is in the launcher's Job Object, which close()
 * terminates first.
 *
 * The bundled DLL is loaded by its absolute executable-relative path. The in-box entry points are
 * used when that DLL is unavailable, with a warning in the log.
 */

#include "frost/core/log.hpp"
#include "frost/io/os_error.hpp"
#include "frost/term/pty.hpp"

#include "pty_buffer.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <windows.h>

namespace frost::term {

namespace {

using Hpcon = void*;
using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, Hpcon*);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(Hpcon, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(Hpcon);

// The ConPTY entry points, or nulls when this Windows has none (before Windows 10 1809).
struct ConPtyApi {
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;
};

const ConPtyApi& conPtyApi() {
    // Trivially destructible; initialised once, thread-safe.
    static const ConPtyApi api = [] {
        ConPtyApi found;
        // Load only the explicitly deployed DLL next to this executable, never a search-path DLL.
        std::array<wchar_t, 32768> executable{};
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length > 0 && length < executable.size()) {
            std::wstring path(executable.data(), length);
            const auto separator = path.find_last_of(L"\\/");
            if (separator != std::wstring::npos) {
                path.resize(separator + 1);
                path += L"conpty.dll";
                HMODULE bundled = LoadLibraryExW(path.c_str(), nullptr,
                                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (bundled != nullptr) {
                    found.create =
                        reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(bundled, "ConptyCreatePseudoConsole"));
                    found.resize =
                        reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(bundled, "ConptyResizePseudoConsole"));
                    found.close =
                        reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(bundled, "ConptyClosePseudoConsole"));
                    if (found.create && found.resize && found.close) {
                        log::info("terminal", "Using bundled ConPTY 1.25.260930003");
                        return found;
                    }
                    FreeLibrary(bundled);
                    found = {};
                }
            }
        }
        log::warn("terminal", "Bundled ConPTY unavailable; using system ConPTY");
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (kernel == nullptr) {
            return found;
        }
        found.create = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(kernel, "CreatePseudoConsole"));
        found.resize = reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(kernel, "ResizePseudoConsole"));
        found.close = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(kernel, "ClosePseudoConsole"));
        if (found.create == nullptr || found.resize == nullptr || found.close == nullptr) {
            return ConPtyApi{};
        }
        return found;
    }();
    return api;
}

// An owned HANDLE (INVALID_HANDLE_VALUE or null when empty).
class Handle {
public:
    Handle() noexcept = default;
    explicit Handle(HANDLE handle) noexcept : m_handle(handle) {}
    ~Handle() { reset(); }
    Handle(Handle&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            m_handle = std::exchange(other.m_handle, nullptr);
        }
        return *this;
    }
    Handle(const Handle&) = delete ("a handle has one owner");
    Handle& operator=(const Handle&) = delete ("a handle has one owner");

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] bool valid() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
    void reset() noexcept {
        if (valid()) {
            CloseHandle(m_handle);
        }
        m_handle = nullptr;
    }

private:
    HANDLE m_handle = nullptr;
};

COORD toCoord(PtySize size) noexcept { return COORD{static_cast<SHORT>(size.columns), static_cast<SHORT>(size.rows)}; }

class ConPty final : public IPty {
public:
    ConPty(Hpcon console, Handle input, Handle output, std::unique_ptr<io::IProcess> process)
        : m_console(console), m_input(std::move(input)), m_output(std::move(output)), m_process(std::move(process)) {
        m_reader = std::thread([this] { drain(); });
    }
    ~ConPty() override { close(); }
    ConPty(const ConPty&) = delete ("a terminal has exactly one owner");
    ConPty& operator=(const ConPty&) = delete ("a terminal has exactly one owner");

    [[nodiscard]] io::ProcessId pid() const noexcept override { return m_process->pid(); }

    Result<void> write(std::string_view bytes) override {
        LockGuard lock(m_writeMutex);
        while (!bytes.empty()) {
            if (!m_input.valid() || m_closing.load(std::memory_order_acquire)) {
                return fail(ErrorCode::IoError, "the terminal is closed");
            }
            DWORD written = 0;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 64 * 1024));
            if (WriteFile(m_input.get(), bytes.data(), chunk, &written, nullptr) == FALSE) {
                return std::unexpected(io::lastWin32Error("WriteFile", "terminal input"));
            }
            bytes.remove_prefix(written);
        }
        return {};
    }

    Result<std::size_t> read(std::span<char> buffer) override { return m_buffer.take(buffer); }

    Result<void> resize(PtySize size) override {
        if (size.columns == 0 || size.rows == 0) {
            return fail(ErrorCode::InvalidArgument, "a terminal needs at least one row and one column");
        }
        LockGuard lock(m_consoleMutex);
        if (m_console == nullptr) {
            return fail(ErrorCode::IoError, "the terminal is closed");
        }
        const HRESULT result = conPtyApi().resize(m_console, toCoord(size));
        if (FAILED(result)) {
            return std::unexpected(io::win32Error(static_cast<DWORD>(result), "ResizePseudoConsole"));
        }
        return {};
    }

    Result<io::ExitStatus> wait(std::optional<IClock::Duration> timeout) override { return m_process->wait(timeout); }

    Result<std::vector<io::ContainedProcess>> containedProcesses() override { return m_process->containedProcesses(); }

    void close() noexcept override {
        if (m_closing.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        m_buffer.abandon();
        // The shell's tree first (the Job Object), then the console host; the reader drains meanwhile.
        static_cast<void>(m_process->terminate());
        {
            LockGuard lock(m_consoleMutex);
            if (m_console != nullptr) {
                conPtyApi().close(m_console);
                m_console = nullptr;
            }
        }
        if (m_reader.joinable()) {
            m_reader.join();
        }
        // A writer blocked on the input pipe failed when the host exited, so the lock is free now.
        LockGuard lock(m_writeMutex);
        m_input.reset();
    }

private:
    // The reader thread: copies the output into m_buffer until the pipe breaks (the host exited).
    void drain() noexcept {
        std::array<char, 64 * 1024> chunk{};
        for (;;) {
            DWORD count = 0;
            if (ReadFile(m_output.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &count, nullptr) == FALSE ||
                count == 0) {
                break;
            }
            // After close() the bytes are dropped, but reading goes on so the host can exit.
            static_cast<void>(m_buffer.append(std::string_view(chunk.data(), count)));
        }
        m_buffer.finish();
    }

    Mutex m_consoleMutex;
    Hpcon m_console FROST_GUARDED_BY(m_consoleMutex);
    Mutex m_writeMutex;
    Handle m_input FROST_GUARDED_BY(m_writeMutex);
    Handle m_output;
    std::unique_ptr<io::IProcess> m_process;
    detail::OutputBuffer m_buffer;
    std::atomic<bool> m_closing{false};
    std::thread m_reader;
};

} // namespace

Result<std::unique_ptr<IPty>> RealPtyLauncher::open(const io::ProcessSpec& spec, PtySize size) {
    if (size.columns == 0 || size.rows == 0) {
        return fail(ErrorCode::InvalidArgument, "a terminal needs at least one row and one column");
    }
    if (spec.pseudoTerminal) {
        return fail(ErrorCode::InvalidArgument, "the PTY launcher sets ProcessSpec::pseudoTerminal itself");
    }
    const ConPtyApi& api = conPtyApi();
    if (api.create == nullptr) {
        return fail(ErrorCode::Unsupported, "this Windows has no pseudo console (ConPTY needs Windows 10 1809)");
    }
    // Our ends: inputWrite and outputRead. The console host gets (and duplicates) the other two.
    HANDLE inputRead = nullptr;
    HANDLE inputWrite = nullptr;
    if (CreatePipe(&inputRead, &inputWrite, nullptr, 0) == FALSE) {
        return std::unexpected(io::lastWin32Error("CreatePipe", "terminal input"));
    }
    Handle inRead(inputRead);
    Handle inWrite(inputWrite);
    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    if (CreatePipe(&outputRead, &outputWrite, nullptr, 0) == FALSE) {
        return std::unexpected(io::lastWin32Error("CreatePipe", "terminal output"));
    }
    Handle outRead(outputRead);
    Handle outWrite(outputWrite);

    Hpcon console = nullptr;
    const HRESULT created = api.create(toCoord(size), inRead.get(), outWrite.get(), 0, &console);
    if (FAILED(created)) {
        return std::unexpected(io::win32Error(static_cast<DWORD>(created), "CreatePseudoConsole"));
    }
    // The host holds its own copies; closing ours lets the pipes break when it exits.
    inRead.reset();
    outWrite.reset();

    /*
     * Children inherit "ignore Ctrl+C" (SetConsoleCtrlHandler(NULL, TRUE), also set by
     * CREATE_NEW_PROCESS_GROUP) from the process that creates them, and a console host does not reset
     * it: if FrostIDE was started that way, Ctrl+C would never interrupt anything in its terminals.
     * Restoring normal processing in this process is harmless (a GUI FrostIDE has no console to get
     * Ctrl+C from) and is what the shell's tree then inherits.
     */
    static_cast<void>(SetConsoleCtrlHandler(nullptr, FALSE));
    io::ProcessSpec attached = spec;
    attached.pseudoTerminal = io::PseudoTerminal{.windowsPseudoConsole = console, .posixTerminalPath = {}};
    auto process = m_launcher->launch(attached);
    if (!process) {
        // Nothing attached: the host exits at once. The reader end is still ours, so drain it on a
        // short-lived thread in case the close waits for its last frame.
        std::thread drainer([&outRead] {
            std::array<char, 4096> sink{};
            DWORD count = 0;
            while (ReadFile(outRead.get(), sink.data(), static_cast<DWORD>(sink.size()), &count, nullptr) != FALSE &&
                   count > 0) {
            }
        });
        api.close(console);
        drainer.join();
        return std::unexpected(std::move(process.error()));
    }
    return std::make_unique<ConPty>(console, std::move(inWrite), std::move(outRead), std::move(*process));
}

} // namespace frost::term
