// SPDX-License-Identifier: Apache-2.0
//
// DBG-0 debugger backend spike (docs/adr/0081-windows-debugger-backend.md): measures the time to the
// first breakpoint and the debugger's peak memory for one program under two backends, the same way:
//   dbg0_spike dbgeng <program> <function> [<command>...]
//       DbgEng in this process, loaded only from %SystemRoot%\System32\dbgeng.dll (the copy that is
//       part of Windows; nothing is redistributed). Creates <program>, sets `bp <module>!<function>`
//       at the initial break (which loads the module's PDB), runs to it, then executes each
//       <command> (".nvload <file>", ".frame 1", "dx numbers", ...) and prints its output.
//   dbg0_spike lldb <lldb-dap> <program> <function>
//       The pinned lldb-dap as a child over stdio (DAP): initialize, launch, a function breakpoint on
//       <function>, configurationDone, the stop.
/*
 * Output: one "DBG0 key=value" line per measurement, which tools/dbg0/run-dbg0.ps1 collects:
 *   backend, program, ttfbMs (process creation to the breakpoint event), symbolsMs (DbgEng: the bp
 *   command, which loads the PDB; lldb: the setFunctionBreakpoints answer), peakWorkingSetMB and
 *   peakPrivateMB (the debugger process: this one for DbgEng, lldb-dap for lldb; Windows'
 *   PROCESS_MEMORY_COUNTERS PeakWorkingSetSize and PeakPagefileUsage).
 * The debuggee is terminated at the breakpoint (DbgEng: TerminateProcesses; lldb: disconnect with
 * terminateDebuggee, then the adapter's exit), so no program runs past main and no window opens.
 *
 * A spike tool, not product code: Windows only, not part of the CMake build or the release; it uses
 * CreateProcessW directly (tools/ are not bound by the IProcessLauncher rule of src/). Build with the
 * pinned clang-cl (run-dbg0.ps1 does).
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: the debugger headers need its types.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <dbgeng.h>
#include <psapi.h>

namespace {

using Clock = std::chrono::steady_clock;

// DbgHelp's SYMOPT_LOAD_LINES (dbghelp.h), which DbgEng's symbol options take.
constexpr ULONG kSymoptLoadLines = 0x00000010;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

void printMemory(HANDLE process) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        std::printf("DBG0 peakWorkingSetMB=%.0f\n", static_cast<double>(counters.PeakWorkingSetSize) / (1024 * 1024));
        std::printf("DBG0 peakPrivateMB=%.0f\n", static_cast<double>(counters.PeakPagefileUsage) / (1024 * 1024));
    }
}

std::string_view stem(std::string_view path) {
    const std::size_t slash = path.find_last_of("\\/");
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(0, dot);
}

// --- DbgEng ---------------------------------------------------------------------------------------

// Collects the engine's output for the commands the spike runs.
class OutputSink final : public IDebugOutputCallbacksWide {
public:
    STDMETHOD(QueryInterface)(REFIID iid, PVOID* out) override {
        if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(IDebugOutputCallbacksWide))) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
    STDMETHOD(Output)(ULONG, PCWSTR text) override {
        if (capture) {
            captured += text;
        }
        return S_OK;
    }

    bool capture = false;
    std::wstring captured;
};

int runDbgEng(const std::string& program, const std::string& function, const std::vector<std::string>& commands) {
    wchar_t system[MAX_PATH]{};
    if (GetSystemDirectoryW(system, MAX_PATH) == 0) {
        std::fprintf(stderr, "no system directory\n");
        return 2;
    }
    const std::wstring dll = std::wstring(system) + L"\\dbgeng.dll";
    HMODULE engine = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (engine == nullptr) {
        std::fprintf(stderr, "cannot load %s\n", narrow(dll).c_str());
        return 2;
    }
    using Create = HRESULT(STDAPICALLTYPE*)(REFIID, PVOID*);
    const auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(engine, "DebugCreate")));
    IDebugClient5* client = nullptr;
    if (create == nullptr || FAILED(create(__uuidof(IDebugClient5), reinterpret_cast<void**>(&client)))) {
        std::fprintf(stderr, "DebugCreate failed\n");
        return 2;
    }
    IDebugControl4* control = nullptr;
    IDebugSymbols3* symbols = nullptr;
    client->QueryInterface(__uuidof(IDebugControl4), reinterpret_cast<void**>(&control));
    client->QueryInterface(__uuidof(IDebugSymbols3), reinterpret_cast<void**>(&symbols));
    if (control == nullptr || symbols == nullptr) {
        std::fprintf(stderr, "the engine has no IDebugControl4/IDebugSymbols3\n");
        return 2;
    }
    OutputSink output;
    client->SetOutputCallbacksWide(&output);
    {
        char version[64]{};
        DWORD handle = 0;
        const DWORD size = GetFileVersionInfoSizeW(dll.c_str(), &handle);
        std::vector<char> info(size);
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT length = 0;
        if (size != 0 && GetFileVersionInfoW(dll.c_str(), 0, size, info.data()) &&
            VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &length)) {
            std::snprintf(version, sizeof(version), "%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS),
                          LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS),
                          LOWORD(fixed->dwFileVersionLS));
        }
        std::printf("DBG0 engine=%s %s\n", narrow(dll).c_str(), version);
    }
    // Symbols from the program's folder only: no symbol server, no _NT_SYMBOL_PATH.
    const std::string folder = program.substr(0, program.find_last_of("\\/"));
    symbols->SetSymbolPathWide(widen(folder).c_str());
    symbols->AddSymbolOptions(kSymoptLoadLines);
    // Stop at the loader breakpoint, where the program's module is mapped and a bp can bind.
    control->AddEngineOptions(DEBUG_ENGOPT_INITIAL_BREAK);

    std::printf("DBG0 backend=dbgeng\nDBG0 program=%s\n", program.c_str());
    const auto start = Clock::now();
    std::wstring command = L"\"" + widen(program) + L"\"";
    if (FAILED(client->CreateProcessWide(0, command.data(), DEBUG_ONLY_THIS_PROCESS))) {
        std::fprintf(stderr, "CreateProcessWide failed\n");
        return 2;
    }
    if (const HRESULT waited = control->WaitForEvent(0, INFINITE); FAILED(waited)) { // The initial break.
        std::fprintf(stderr, "no initial break (0x%08lx)\n", static_cast<unsigned long>(waited));
        client->TerminateProcesses();
        return 2;
    }
    const auto symbolsStart = Clock::now();
    const std::wstring bp = L"bp " + widen(stem(program)) + L"!" + widen(function);
    output.capture = true;
    control->ExecuteWide(DEBUG_OUTCTL_THIS_CLIENT, bp.c_str(), DEBUG_EXECUTE_NOT_LOGGED);
    output.capture = false;
    std::printf("DBG0 symbolsMs=%.0f\n", msSince(symbolsStart));
    if (!output.captured.empty()) {
        std::printf("DBG0 bpOutput=%s\n", narrow(output.captured).c_str());
    }
    ULONG count = 0;
    control->GetNumberBreakpoints(&count);
    if (count == 0) {
        std::fprintf(stderr, "the breakpoint on %s was not set\n", function.c_str());
        client->TerminateProcesses();
        return 2;
    }
    control->SetExecutionStatus(DEBUG_STATUS_GO);
    if (FAILED(control->WaitForEvent(0, INFINITE))) {
        std::fprintf(stderr, "no breakpoint event\n");
        return 2;
    }
    ULONG type = 0;
    ULONG processId = 0;
    ULONG threadId = 0;
    control->GetLastEventInformation(&type, &processId, &threadId, nullptr, 0, nullptr, nullptr, 0, nullptr);
    std::printf("DBG0 ttfbMs=%.0f\n", msSince(start));
    std::printf("DBG0 stoppedOnBreakpoint=%s\n", type == DEBUG_EVENT_BREAKPOINT ? "yes" : "no");
    for (const std::string& extra : commands) {
        output.captured.clear();
        output.capture = true;
        const HRESULT result =
            control->ExecuteWide(DEBUG_OUTCTL_THIS_CLIENT, widen(extra).c_str(), DEBUG_EXECUTE_NOT_LOGGED);
        output.capture = false;
        std::printf("DBG0 command=%s hr=0x%08lx\n%s\n", extra.c_str(), static_cast<unsigned long>(result),
                    narrow(output.captured).c_str());
    }
    printMemory(GetCurrentProcess());
    client->TerminateProcesses();
    client->EndSession(DEBUG_END_ACTIVE_TERMINATE);
    client->SetOutputCallbacksWide(nullptr);
    symbols->Release();
    control->Release();
    client->Release();
    return 0;
}

// --- lldb-dap -------------------------------------------------------------------------------------

class Adapter {
public:
    bool start(const std::string& executable) {
        SECURITY_ATTRIBUTES inherit{
            .nLength = sizeof(inherit), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
        HANDLE childIn = nullptr;
        HANDLE childOut = nullptr;
        if (!CreatePipe(&childIn, &m_in, &inherit, 0) || !CreatePipe(&m_out, &childOut, &inherit, 0)) {
            return false;
        }
        SetHandleInformation(m_in, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(m_out, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childIn;
        startup.hStdOutput = childOut;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::wstring command = L"\"" + widen(executable) + L"\"";
        PROCESS_INFORMATION info{};
        const BOOL created = CreateProcessW(widen(executable).c_str(), command.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
        CloseHandle(childIn);
        CloseHandle(childOut);
        if (!created) {
            return false;
        }
        CloseHandle(info.hThread);
        m_process = info.hProcess;
        return true;
    }

    void send(std::string_view command, std::string_view arguments) {
        const std::string body = "{\"seq\":" + std::to_string(m_seq++) + ",\"type\":\"request\",\"command\":\"" +
                                 std::string(command) + "\",\"arguments\":" + std::string(arguments) + "}";
        const std::string frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        DWORD written = 0;
        WriteFile(m_in, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr);
    }

    // The next message; "" at the end of the stream.
    std::string read() {
        while (true) {
            const std::size_t header = m_buffer.find("\r\n\r\n");
            if (header != std::string::npos) {
                const std::size_t length = std::strtoull(m_buffer.c_str() + m_buffer.find(':') + 1, nullptr, 10);
                if (m_buffer.size() >= header + 4 + length) {
                    std::string message = m_buffer.substr(header + 4, length);
                    m_buffer.erase(0, header + 4 + length);
                    return message;
                }
            }
            char chunk[65536];
            DWORD got = 0;
            if (!ReadFile(m_out, chunk, sizeof(chunk), &got, nullptr) || got == 0) {
                return {};
            }
            m_buffer.append(chunk, got);
        }
    }

    // Reads until a message containing `needle`; "" at the end of the stream.
    std::string waitFor(std::string_view needle) {
        while (true) {
            std::string message = read();
            if (message.empty() || message.find(needle) != std::string::npos) {
                return message;
            }
        }
    }

    [[nodiscard]] HANDLE process() const noexcept { return m_process; }

    void finish() {
        CloseHandle(m_in);
        if (WaitForSingleObject(m_process, 60000) != WAIT_OBJECT_0) {
            TerminateProcess(m_process, 1);
        }
        CloseHandle(m_out);
        CloseHandle(m_process);
    }

private:
    HANDLE m_in = nullptr;
    HANDLE m_out = nullptr;
    HANDLE m_process = nullptr;
    int m_seq = 1;
    std::string m_buffer;
};

std::string jsonString(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out + "\"";
}

int runLldb(const std::string& lldbDap, const std::string& program, const std::string& function) {
    Adapter adapter;
    std::printf("DBG0 backend=lldb\nDBG0 engine=%s\nDBG0 program=%s\n", lldbDap.c_str(), program.c_str());
    if (!adapter.start(lldbDap)) {
        std::fprintf(stderr, "cannot start %s\n", lldbDap.c_str());
        return 2;
    }
    const auto start = Clock::now();
    adapter.send("initialize",
                 R"({"adapterID":"lldb-dap","linesStartAt1":true,"columnsStartAt1":true,"pathFormat":"path"})");
    adapter.waitFor("\"command\":\"initialize\"");
    const std::string folder = program.substr(0, program.find_last_of("\\/"));
    adapter.send("launch", "{\"program\":" + jsonString(program) + ",\"cwd\":" + jsonString(folder) + "}");
    adapter.waitFor("\"event\":\"initialized\"");
    const auto symbolsStart = Clock::now();
    adapter.send("setFunctionBreakpoints", "{\"breakpoints\":[{\"name\":" + jsonString(function) + "}]}");
    const std::string answer = adapter.waitFor("\"command\":\"setFunctionBreakpoints\"");
    std::printf("DBG0 symbolsMs=%.0f\n", msSince(symbolsStart));
    std::printf("DBG0 bpVerified=%s\n", answer.find("\"verified\":true") != std::string::npos ? "yes" : "no");
    adapter.send("configurationDone", "{}");
    const std::string stopped = adapter.waitFor("\"event\":\"stopped\"");
    std::printf("DBG0 ttfbMs=%.0f\n", msSince(start));
    std::printf("DBG0 stoppedOnBreakpoint=%s\n", stopped.find("breakpoint") != std::string::npos ? "yes" : "no");
    printMemory(adapter.process());
    adapter.send("disconnect", R"({"terminateDebuggee":true})");
    adapter.waitFor("\"command\":\"disconnect\"");
    adapter.finish();
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(narrow(argv[i]));
    }
    if (args.size() >= 3 && args[0] == "dbgeng") {
        return runDbgEng(args[1], args[2], std::vector<std::string>(args.begin() + 3, args.end()));
    }
    if (args.size() >= 4 && args[0] == "lldb") {
        return runLldb(args[1], args[2], args[3]);
    }
    std::fprintf(stderr, "usage: dbg0_spike dbgeng <program> <function> [<command>...]\n"
                         "       dbg0_spike lldb <lldb-dap> <program> <function> [<variable>...]\n");
    return 2;
}
