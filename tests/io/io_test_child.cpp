// SPDX-License-Identifier: Apache-2.0
//
// io_test_child: a tiny helper program that the frost::io process tests launch
// through the real IProcessLauncher. argv[1] selects what it does; output is
// line-based and every string is hex-encoded UTF-8, so the tests can compare
// exact bytes without any quoting or encoding ambiguity.
//
//   argv ARGS...       "crt <n>" then "arg:<hex>" per argument as the C runtime parsed
//                      them; on Windows also "shell <n>" and the CommandLineToArgvW view
//   env NAME           "set:<hex>" or "unset"
//   cwd                "cwd:<hex>"
//   exit N             exits with status N
//   cat                copies stdin to stdout until EOF
//   tee                copies stdin to stdout and to stderr, chunk by chunk
//   flood N            writes N pattern bytes to stdout and N to stderr, interleaved in 4 KiB chunks
//   stream N           writes N pattern bytes (byte i is i % 251) to stdout only, in 64 KiB chunks
//   read-then-exit N   reads N bytes of stdin (fewer at EOF), prints "read <n>", then exits 0 with
//                      stdin still open on the parent's side (the parent's writes then fail)
//   close-stdout-then-sleep MS  closes its stdout, then sleeps MS milliseconds and exits 0
//   print OUT ERR      writes OUT to stdout and ERR to stderr (raw), then exits 0
//   sleep MS           sleeps, then exits 0
//   traceable-sleep MS FILE  Linux: lets any debugger attach (prctl PR_SET_PTRACER_ANY, as a
//                      debuggee does under Yama ptrace_scope 1); then creates FILE (ready to be
//                      attached to), sleeps MS and exits 0 (the attach-configuration test, RUN-5)
//   probe-handle N ID  "inherited" or "not inherited": whether handle/descriptor N (or the
//                      word "stdin" for the standard input) here is the parent's probe
//                      object ID (Windows: the name of an event, compared with
//                      CompareObjectHandles; POSIX: "<st_dev>:<st_ino>" of a file).
//                      Checking identity, not just validity, matters: the child's own
//                      handles can have the same numeric value.
//   grandchild wait|exit  starts a copy of itself as "sleep 60000" that shares these standard
//                      handles, prints "grandchild <pid>", then sleeps 60 s (wait) or exits 0
//                      (exit), leaving the grandchild behind holding stdout open
//   grandchild-own-group wait|exit  the same, but on POSIX the grandchild starts in a process
//                      group of its own (setpgid, as ninja starts every command), so only a
//                      session-wide kill reaches it; on Windows the same as grandchild
//   session            POSIX: "session leader" or "not session leader", then "tty" if
//                      /dev/tty (the controlling terminal) opens, "no tty" for ENXIO
//   fd-write N TEXT    POSIX: writes TEXT to descriptor N, then prints "wrote" ("write error
//                      <errno>" if that fails); for ProcessSpec::posixInheritedFds
//   find-identity ID   POSIX: "found <fd>" for every descriptor 3..1023 whose "<st_dev>:<st_ino>"
//                      is ID, then "done"; shows where (and whether) a probe object arrived
//   lease PATH         POSIX: takes a write lease on PATH, prints "leased", then waits up
//                      to 30 s for the lease-break signal, releases the lease and prints
//                      "released" (or "timed out"); "no lease <errno>" if refused
//   pipe-info          Windows: "dacl:<SDDL>" and "name:<name>" of its stdout pipe
//   image-path         Windows: "image:<hex>", its own image path as GetModuleFileNameW
//                      reports it (so a \\?\ start shows the prefix)
//   desktop            Windows: "desktop:<hex>", "<window station>\<desktop>" of its main
//                      thread, from GetUserObjectInformationW(UOI_NAME)
//   window             Windows: creates a top-level window that is never shown, prints
//                      "window:<handle in decimal>", and keeps it until stdin reaches EOF
//
// Numeric arguments (exit, sleep, probe-handle on POSIX) must be whole decimal ints, or it exits with
// status 65; an unknown mode or a wrong argument count exits with 64.
//
// It deliberately uses only the OS and C runtime, not frost::io, so it is an
// independent oracle for the launcher's quoting, environment and directory handling.

#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <crtdbg.h>
#include <fcntl.h>
#include <io.h>
// GetSecurityInfo; ConvertSecurityDescriptorToStringSecurityDescriptorW. Both need windows.h first.
#include <aclapi.h>
#include <sddl.h>
// CommandLineToArgvW; shellapi.h needs windows.h first.
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <sys/types.h>

#include <climits>

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

// POSIX requires the application to declare environ; <unistd.h> declares it only under _GNU_SOURCE, which
// libstdc++ defines, so clang-tidy sees this declaration as redundant.
extern char** environ; // NOLINT(readability-redundant-declaration): POSIX requires the declaration
#endif

namespace {

std::string hex(const std::string& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        out += kDigits[c >> 4];
        out += kDigits[c & 15];
    }
    return out;
}

void writeRaw(std::FILE* stream, const std::string& bytes) { std::fwrite(bytes.data(), 1, bytes.size(), stream); }

void line(const std::string& text) { writeRaw(stdout, text + "\n"); }

// The decimal int that is all of `text`. Anything else ends the program with status 65 (EX_DATAERR, unlike
// the usage error's 64) and a message, so a malformed argument fails a test instead of reading as 0.
int number(const std::string& text) {
    int value = 0;
    const char* const end = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        std::fprintf(stderr, "io_test_child: not a number: \"%s\"\n", text.c_str());
        std::exit(65);
    }
    return value;
}

// Copies stdin to the given streams until EOF.
void copyStdin(bool alsoStderr) {
    std::vector<char> buffer(16 * 1024);
    for (;;) {
        const std::size_t got = std::fread(buffer.data(), 1, buffer.size(), stdin);
        if (got == 0) {
            break;
        }
        std::fwrite(buffer.data(), 1, got, stdout);
        if (alsoStderr) {
            std::fwrite(buffer.data(), 1, got, stderr);
        }
    }
}

void flood(std::size_t count) {
    std::string chunk(4096, '\0');
    std::size_t written = 0;
    while (written < count) {
        const std::size_t size = count - written < chunk.size() ? count - written : chunk.size();
        for (std::size_t i = 0; i < size; ++i) {
            chunk[i] = static_cast<char>((written + i) % 251);
        }
        std::fwrite(chunk.data(), 1, size, stdout);
        std::fwrite(chunk.data(), 1, size, stderr);
        written += size;
    }
}

#ifdef _WIN32

std::string toUtf8(const wchar_t* wide) {
    const int length = static_cast<int>(std::wcslen(wide));
    if (length == 0) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return "<invalid UTF-16>";
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, length, out.data(), bytes, nullptr, nullptr);
    return out;
}

#endif

// Starts "<this program> sleep 60000" with this process's standard handles and returns
// its process id, or 0 on failure. Deliberately not through frost::io (see the header).
// POSIX `ownGroup`: the copy leads a new process group (posix_spawn sets it before exec).
unsigned long spawnSleepingCopy([[maybe_unused]] bool ownGroup = false) {
#ifdef _WIN32
    std::wstring self(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (length == 0 || length >= self.size()) {
        return 0;
    }
    self.resize(length);
    std::wstring commandLine = L"\"" + self + L"\" sleep 60000";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION info{};
    // bInheritHandles = TRUE: the standard handles this process inherited are still inheritable.
    if (CreateProcessW(self.c_str(), commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                       &info) == FALSE) {
        return 0;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return info.dwProcessId;
#else
    char self[PATH_MAX + 1] = {};
    const ssize_t length = readlink("/proc/self/exe", self, PATH_MAX);
    if (length <= 0) {
        return 0;
    }
    std::string sleepMode = "sleep";
    std::string duration = "60000";
    char* argv[] = {self, sleepMode.data(), duration.data(), nullptr};
    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0) {
        return 0;
    }
    if (ownGroup && (posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) != 0 ||
                     posix_spawnattr_setpgroup(&attributes, 0) != 0)) {
        posix_spawnattr_destroy(&attributes);
        return 0;
    }
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, self, nullptr, &attributes, argv, environ);
    posix_spawnattr_destroy(&attributes);
    if (spawned != 0) {
        return 0;
    }
    return static_cast<unsigned long>(pid);
#endif
}

int run(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::fputs("usage: io_test_child MODE ...\n", stderr);
        return 64;
    }
    const std::string& mode = args[1];
    if (mode == "invalid-utf8") {
        constexpr char bytes[] = {'a', static_cast<char>(0xff), '\n'};
        std::fwrite(bytes, 1, sizeof(bytes), stdout);
        std::fflush(stdout);
        return 0;
    }
    if (mode == "argv") {
        line("crt " + std::to_string(args.size() - 2));
        for (std::size_t i = 2; i < args.size(); ++i) {
            line("arg:" + hex(args[i]));
        }
#ifdef _WIN32
        int count = 0;
        wchar_t** parsed = CommandLineToArgvW(GetCommandLineW(), &count);
        line("shell " + std::to_string(count >= 2 ? count - 2 : 0));
        for (int i = 2; i < count; ++i) {
            line("arg:" + hex(toUtf8(parsed[i])));
        }
        LocalFree(parsed);
#endif
        return 0;
    }
    if (mode == "env" && args.size() == 3) {
#ifdef _WIN32
        const int length = MultiByteToWideChar(CP_UTF8, 0, args[2].c_str(), -1, nullptr, 0);
        std::wstring name(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, args[2].c_str(), -1, name.data(), length);
        std::vector<wchar_t> value(32768);
        SetLastError(ERROR_SUCCESS);
        const DWORD got = GetEnvironmentVariableW(name.c_str(), value.data(), static_cast<DWORD>(value.size()));
        if (got == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            line("unset");
        } else {
            line("set:" + hex(toUtf8(value.data())));
        }
#else
        const char* value = std::getenv(args[2].c_str());
        line(value == nullptr ? std::string("unset") : "set:" + hex(value));
#endif
        return 0;
    }
    if (mode == "cwd") {
#ifdef _WIN32
        std::vector<wchar_t> buffer(32768);
        GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
        line("cwd:" + hex(toUtf8(buffer.data())));
#else
        std::vector<char> buffer(65536);
        line("cwd:" + hex(getcwd(buffer.data(), buffer.size()) != nullptr ? buffer.data() : "?"));
#endif
        return 0;
    }
    if (mode == "exit" && args.size() == 3) {
        return number(args[2]);
    }
    if (mode == "cat" || mode == "tee") {
        copyStdin(mode == "tee");
        return 0;
    }
    if (mode == "flood" && args.size() == 3) {
        flood(static_cast<std::size_t>(std::strtoull(args[2].c_str(), nullptr, 10)));
        return 0;
    }
    if (mode == "stream" && args.size() == 3) {
        const auto count = static_cast<std::size_t>(std::strtoull(args[2].c_str(), nullptr, 10));
        std::string chunk(64 * 1024, '\0');
        for (std::size_t written = 0; written < count;) {
            const std::size_t size = count - written < chunk.size() ? count - written : chunk.size();
            for (std::size_t i = 0; i < size; ++i) {
                chunk[i] = static_cast<char>((written + i) % 251);
            }
            if (std::fwrite(chunk.data(), 1, size, stdout) != size) {
                return 1; // The reader went away.
            }
            written += size;
        }
        return 0;
    }
    if (mode == "read-then-exit" && args.size() == 3) {
        const auto wanted = static_cast<std::size_t>(std::strtoull(args[2].c_str(), nullptr, 10));
        std::vector<char> buffer(16 * 1024);
        std::size_t got = 0;
        while (got < wanted) {
            const std::size_t chunk = wanted - got < buffer.size() ? wanted - got : buffer.size();
            const std::size_t read = std::fread(buffer.data(), 1, chunk, stdin);
            if (read == 0) {
                break;
            }
            got += read;
        }
        line("read " + std::to_string(got));
        std::fflush(stdout);
        // Exits without draining stdin: the parent's pending and later writes fail (broken pipe).
        std::_Exit(0);
    }
    if (mode == "close-stdout-then-sleep" && args.size() == 3) {
        std::fflush(stdout);
#ifdef _WIN32
        // The CRT's descriptor 1 owns the inherited handle; closing it closes the pipe end.
        _close(1);
#else
        close(STDOUT_FILENO);
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(args[2].c_str())));
        std::_Exit(0);
    }
    if (mode == "print" && args.size() == 4) {
        writeRaw(stdout, args[2]);
        std::fflush(stdout);
        writeRaw(stderr, args[3]);
        return 0;
    }
    if (mode == "sleep" && args.size() == 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(number(args[2])));
        return 0;
    }
    if (mode == "traceable-sleep" && args.size() == 4) {
        const int milliseconds = number(args[2]);
#ifdef __linux__
        if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0) {
            return 66;
        }
#endif
#ifdef _WIN32
        std::wstring path(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, args[3].c_str(), -1, nullptr, 0)),
                          L'\0');
        MultiByteToWideChar(CP_UTF8, 0, args[3].c_str(), -1, path.data(), static_cast<int>(path.size()));
        const HANDLE ready =
            CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (ready == INVALID_HANDLE_VALUE) {
            return 66;
        }
        CloseHandle(ready);
#else
        std::FILE* ready = std::fopen(args[3].c_str(), "wb");
        if (ready == nullptr) {
            return 66;
        }
        std::fclose(ready);
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        return 0;
    }
    if (mode == "probe-handle" && args.size() == 4) {
        const bool isStdin = args[2] == "stdin";
#ifdef _WIN32
        const HANDLE handle =
            isStdin
                ? GetStdHandle(STD_INPUT_HANDLE)
                : reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::strtoull(args[2].c_str(), nullptr, 10)));
        const std::wstring name(args[3].begin(), args[3].end()); // The tests use ASCII names.
        const HANDLE own = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
        if (own == nullptr) {
            line("no such event");
            return 3;
        }
        // CompareObjectHandles (Windows 10+) is exported by kernelbase.dll but its import
        // library is only the mincore umbrella, which must not be mixed with kernel32.lib.
        using CompareFn = BOOL(WINAPI*)(HANDLE, HANDLE);
        const FARPROC proc = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CompareObjectHandles");
        if (proc == nullptr) {
            line("no CompareObjectHandles");
            return 3;
        }
        const auto compare = reinterpret_cast<CompareFn>(reinterpret_cast<void*>(proc));
        // OpenEventW returns a free handle value. If that is the probed value N, slot N was
        // empty, so nothing was inherited there; comparing the handle with itself would
        // wrongly report that it was.
        const bool same = own != handle && compare(own, handle) != FALSE;
        CloseHandle(own);
        line(same ? "inherited" : "not inherited");
#else
        struct stat info{};
        const bool same = fstat(isStdin ? 0 : number(args[2]), &info) == 0 &&
                          std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) == args[3];
        line(same ? "inherited" : "not inherited");
#endif
        return 0;
    }
#ifdef _WIN32
    if (mode == "pipe-info" && args.size() == 2) {
        const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        wchar_t* sddl = nullptr;
        if (GetSecurityInfo(out, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr,
                            &descriptor) != ERROR_SUCCESS ||
            ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION,
                                                                 &sddl, nullptr) == FALSE) {
            line("dacl-error:" + std::to_string(GetLastError()));
        } else {
            line("dacl:" + toUtf8(sddl));
        }
        LocalFree(sddl);
        LocalFree(descriptor);
        std::vector<DWORD> buffer(1024); // DWORD-aligned room for FILE_NAME_INFO and a long name.
        auto* info = reinterpret_cast<FILE_NAME_INFO*>(buffer.data());
        if (GetFileInformationByHandleEx(out, FileNameInfo, info, static_cast<DWORD>(buffer.size() * sizeof(DWORD))) ==
            FALSE) {
            line("name-error:" + std::to_string(GetLastError()));
        } else {
            const std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            line("name:" + toUtf8(name.c_str()));
        }
        return 0;
    }
    if (mode == "image-path" && args.size() == 2) {
        std::wstring image(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
        if (length == 0 || length >= image.size()) {
            line("image-error:" + std::to_string(GetLastError()));
            return 3;
        }
        image.resize(length);
        line("image:" + hex(toUtf8(image.c_str())));
        return 0;
    }
    if (mode == "desktop" && args.size() == 2) {
        // The name of a window station or desktop handle, or "" on failure.
        const auto objectName = [](HANDLE object) {
            std::vector<wchar_t> name(1024);
            DWORD needed = 0;
            if (GetUserObjectInformationW(object, UOI_NAME, name.data(),
                                          static_cast<DWORD>(name.size() * sizeof(wchar_t)), &needed) == FALSE) {
                return std::string();
            }
            return toUtf8(name.data());
        };
        const std::string station = objectName(GetProcessWindowStation());
        const std::string desk = objectName(GetThreadDesktop(GetCurrentThreadId()));
        if (station.empty() || desk.empty()) {
            line("desktop-error:" + std::to_string(GetLastError()));
            return 3;
        }
        line("desktop:" + hex(station + "\\" + desk));
        return 0;
    }
    if (mode == "window" && args.size() == 2) {
        // Never shown (no WS_VISIBLE), so even on the user's desktop it would stay invisible.
        HWND window = CreateWindowExW(0, L"STATIC", L"io_test_child", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
        if (window == nullptr) {
            line("window-error:" + std::to_string(GetLastError()));
            return 3;
        }
        line("window:" + std::to_string(reinterpret_cast<std::uintptr_t>(window)));
        std::fflush(stdout);
        std::vector<char> ignored(4096);
        while (std::fread(ignored.data(), 1, ignored.size(), stdin) > 0) {
        }
        DestroyWindow(window);
        return 0;
    }
#else
    if (mode == "session" && args.size() == 2) {
        line(getsid(0) == getpid() ? "session leader" : "not session leader");
        const int tty = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (tty >= 0) {
            close(tty);
            line("tty");
        } else {
            line(errno == ENXIO ? "no tty" : "tty error " + std::to_string(errno));
        }
        return 0;
    }
    if (mode == "fd-write" && args.size() == 4) {
        const int fd = std::atoi(args[2].c_str());
        const ssize_t wrote = write(fd, args[3].data(), args[3].size());
        line(wrote == static_cast<ssize_t>(args[3].size()) ? "wrote" : "write error " + std::to_string(errno));
        return 0;
    }
    if (mode == "find-identity" && args.size() == 3) {
        for (int fd = 3; fd < 1024; ++fd) {
            struct stat info{};
            if (fstat(fd, &info) == 0 && std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) == args[2]) {
                line("found " + std::to_string(fd));
            }
        }
        line("done");
        return 0;
    }
    if (mode == "lease" && args.size() == 3) {
        // The break is announced with SIGIO; blocked here, it waits for sigtimedwait.
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGIO);
        sigprocmask(SIG_BLOCK, &signals, nullptr);
        const int fd = open(args[2].c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0 || fcntl(fd, F_SETLEASE, F_WRLCK) != 0) {
            line("no lease " + std::to_string(errno));
            return 3;
        }
        line("leased");
        std::fflush(stdout);
        const timespec limit{30, 0};
        const int got = sigtimedwait(&signals, nullptr, &limit);
        fcntl(fd, F_SETLEASE, F_UNLCK);
        close(fd);
        line(got == SIGIO ? "released" : "timed out");
        return 0;
    }
#endif
    if ((mode == "grandchild" || mode == "grandchild-own-group") && args.size() == 3 &&
        (args[2] == "wait" || args[2] == "exit")) {
        const unsigned long pid = spawnSleepingCopy(mode == "grandchild-own-group");
        if (pid == 0) {
            line("spawn failed");
            return 3;
        }
        line("grandchild " + std::to_string(pid));
        std::fflush(stdout);
        if (args[2] == "wait") {
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
        return 0;
    }
    std::fputs("io_test_child: unknown mode or wrong argument count\n", stderr);
    return 64;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    // Never a dialog on the desktop: crashes, aborts and Debug CRT assertions go to stderr
    // and end the process, so a failing test reports instead of waiting for a click.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for ([[maybe_unused]] const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) { // Unused in Release.
        static_cast<void>(_CrtSetReportMode(type, _CRTDBG_MODE_FILE));
        static_cast<void>(_CrtSetReportFile(type, _CRTDBG_FILE_STDERR));
    }
    // Binary mode: no CRLF translation, so byte counts and content survive exactly.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        args.push_back(toUtf8(argv[i]));
    }
    const int status = run(args);
    std::fflush(stdout);
    std::fflush(stderr);
    return status;
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    const int status = run(args);
    std::fflush(stdout);
    std::fflush(stderr);
    return status;
}
#endif
