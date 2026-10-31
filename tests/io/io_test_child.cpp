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
//   print OUT ERR      writes OUT to stdout and ERR to stderr (raw), then exits 0
//   sleep MS           sleeps, then exits 0
//   probe-handle N ID  "inherited" or "not inherited": whether handle/descriptor N (or the
//                      word "stdin" for the standard input) here is the parent's probe
//                      object ID (Windows: the name of an event, compared with
//                      CompareObjectHandles; POSIX: "<st_dev>:<st_ino>" of a file).
//                      Checking identity, not just validity, matters: the child's own
//                      handles can have the same numeric value.
//
// It deliberately uses only the OS and C runtime, not frost::io, so it is an
// independent oracle for the launcher's quoting, environment and directory handling.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <fcntl.h>
#include <io.h>
// CommandLineToArgvW; shellapi.h needs windows.h first.
#include <shellapi.h>
#else
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>
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

int run(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::fputs("usage: io_test_child MODE ...\n", stderr);
        return 64;
    }
    const std::string& mode = args[1];
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
        return std::atoi(args[2].c_str());
    }
    if (mode == "cat" || mode == "tee") {
        copyStdin(mode == "tee");
        return 0;
    }
    if (mode == "flood" && args.size() == 3) {
        flood(static_cast<std::size_t>(std::strtoull(args[2].c_str(), nullptr, 10)));
        return 0;
    }
    if (mode == "print" && args.size() == 4) {
        writeRaw(stdout, args[2]);
        std::fflush(stdout);
        writeRaw(stderr, args[3]);
        return 0;
    }
    if (mode == "sleep" && args.size() == 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(args[2].c_str())));
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
        const bool same = compare(own, handle) != FALSE;
        CloseHandle(own);
        line(same ? "inherited" : "not inherited");
#else
        struct stat info{};
        const bool same = fstat(isStdin ? 0 : std::atoi(args[2].c_str()), &info) == 0 &&
                          std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) == args[3];
        line(same ? "inherited" : "not inherited");
#endif
        return 0;
    }
    std::fputs("io_test_child: unknown mode or wrong argument count\n", stderr);
    return 64;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
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
