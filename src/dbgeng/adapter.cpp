// SPDX-License-Identifier: Apache-2.0
// System32-only native DAP backend. One worker creates, uses and releases every DbgEng interface.
// DAP 1.71 framing is shared with the existing client; debugger output is never parsed as values.
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"
#include "frost/rpc/framing.hpp"

#include "internal.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <exception>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

namespace frost::dbgeng {
namespace {
using Json = rpc::Json;
using namespace std::chrono_literals;
constexpr std::size_t kMaxMessage = 4 * 1024 * 1024;
constexpr std::size_t kMaxQueue = 16 * 1024 * 1024;

bool absoluteLocal(std::string_view path, bool commandPath = false) {
    const bool localVerbatim = path.size() >= 7 && path.starts_with("\\\\?\\") && path[5] == ':' && path[6] == '\\';
    const auto separator = [](char value) { return value == '/' || value == '\\'; };
    const bool unc = path.size() >= 2 && separator(path[0]) && separator(path[1]);
    return io::isAbsolutePath(path) && (!unc || localVerbatim) && path.find('\0') == std::string_view::npos &&
           (!commandPath || path.find_first_of("\r\n\";") == std::string_view::npos);
}

// Dump evaluation is deliberately a field/index lookup subset. No assignment, function call,
// debugger extensions, casts or command expressions can turn postmortem inspection into mutation.
bool dumpExpression(std::string_view text) {
    if (text.empty() || text.size() > 4096)
        return false;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':' ||
              c == '.' || c == '-' || c == '>' || c == '[' || c == ']' || c == ' '))
            return false;
    }
    return text.find("--") == std::string_view::npos;
}

class OutputSink final : public IDebugOutputCallbacksWide {
public:
    std::function<void(ULONG, std::wstring_view)> emit;
    STDMETHOD(QueryInterface)(REFIID iid, void** result) override {
        if (result == nullptr)
            return E_POINTER;
        *result = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDebugOutputCallbacksWide)) {
            *result = static_cast<IDebugOutputCallbacksWide*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++m_references; }
    STDMETHOD_(ULONG, Release)() override { return --m_references; }
    STDMETHOD(Output)(ULONG mask, PCWSTR text) override {
        if (emit && text != nullptr)
            emit(mask, std::wstring_view(text, std::min<std::size_t>(wcslen(text), 32768)));
        return S_OK;
    }

private:
    std::atomic<ULONG> m_references{1}; // Lifetime is owned by Engine, not by a callback reference.
};

class Engine {
public:
    explicit Engine(std::function<void(std::string_view, Json)> event) : m_event(std::move(event)) {
        m_output.emit = [this](ULONG mask, std::wstring_view text) {
            if (m_capturingCommand && (mask & DEBUG_OUTPUT_ERROR) != 0)
                m_commandError = true;
            if (m_loadingNatvis && (mask & DEBUG_OUTPUT_ERROR) != 0)
                m_natvisError = true;
            if (auto value = utf8(text); value)
                m_event("output", Json::object().set("category", "console").set("output", std::move(*value)));
        };
    }
    ~Engine() { close(); }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] bool running() const noexcept { return m_running; }
    [[nodiscard]] bool ended() const noexcept { return m_ended; }
    [[nodiscard]] bool readOnly() const noexcept { return m_dump; }

    Result<void> load() {
        if (m_client)
            return {};
        std::array<wchar_t, MAX_PATH> directory{};
        const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        if (length == 0 || length >= directory.size())
            return fail(ErrorCode::IoError, "Windows system directory is unavailable");
        const std::wstring path = std::wstring(directory.data(), length) + L"\\dbgeng.dll";
        m_library = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m_library == nullptr)
            return fail(ErrorCode::Unsupported, "System32 DbgEng is unavailable; no other copy is loaded");
        using Create = HRESULT(STDAPICALLTYPE*)(REFIID, void**);
        const auto create = reinterpret_cast<Create>(GetProcAddress(m_library, "DebugCreate"));
        if (create == nullptr)
            return fail(ErrorCode::Unsupported, "System32 DbgEng has no DebugCreate export");
        HRESULT status = create(__uuidof(IDebugClient5), reinterpret_cast<void**>(m_client.put()));
        if (FAILED(status))
            return std::unexpected(engineError("DebugCreate", status));
        if (FAILED(status = m_client.query(m_control)) || FAILED(status = m_client.query(m_symbols)) ||
            FAILED(status = m_client.query(m_systems)))
            return std::unexpected(engineError("DbgEng interfaces", status));
        if (FAILED(status = m_client->SetOutputCallbacksWide(&m_output)))
            return std::unexpected(engineError("debug output callback", status));
        status = m_client->SetOutputMask(DEBUG_OUTPUT_NORMAL | DEBUG_OUTPUT_ERROR | DEBUG_OUTPUT_WARNING |
                                         DEBUG_OUTPUT_DEBUGGEE);
        if (FAILED(status))
            return std::unexpected(engineError("debug output mask", status));
        m_control->RemoveEngineOptions(DEBUG_ENGOPT_ALLOW_NETWORK_PATHS);
        status = m_control->AddEngineOptions(DEBUG_ENGOPT_INITIAL_BREAK | DEBUG_ENGOPT_DISALLOW_NETWORK_PATHS |
                                             DEBUG_ENGOPT_DISALLOW_SHELL_COMMANDS | DEBUG_ENGOPT_DISABLESQM);
        if (FAILED(status))
            return std::unexpected(engineError("local-only engine options", status));
        m_symbols->AddSymbolOptions(0x10U); // SYMOPT_LOAD_LINES from the documented DbgHelp options.
        status = m_control->SetCodeLevel(DEBUG_LEVEL_SOURCE);
        if (FAILED(status))
            return std::unexpected(engineError("source stepping mode", status));
        if (auto initialized = m_models.initialize(m_client.get()); !initialized)
            return initialized;
        (void)m_client.query(m_memory);
        (void)m_client.query(m_registers);
        return {};
    }

    Result<Json> launch(const Json& arguments) {
        if (m_loaded)
            return fail(ErrorCode::Busy, "this adapter already owns a debug target");
        if (auto loaded = load(); !loaded)
            return std::unexpected(std::move(loaded.error()));
        const std::string program(arguments.stringAt("program"));
        const std::string dump(arguments.stringAt("dumpFile"));
        const std::string cwd(arguments.stringAt("cwd"));
        const bool invalidProgram = arguments.find("processId") ? (!program.empty() && !absoluteLocal(program, true))
                                    : dump.empty()
                                        ? !absoluteLocal(program, true)
                                        : (!absoluteLocal(dump) || (!program.empty() && !absoluteLocal(program, true)));
        if (invalidProgram || (!cwd.empty() && !absoluteLocal(cwd, true)))
            return fail(ErrorCode::InvalidArgument, "program, dump and working directory must be absolute local paths");
        std::wstring symbols;
        if (const Json* paths = arguments.find("symbolPaths"); paths != nullptr) {
            if (!paths->isArray() || paths->size() > 64)
                return fail(ErrorCode::InvalidArgument, "invalid local symbol directory list");
            for (const Json& path : paths->items()) {
                if (!path.asString() || !absoluteLocal(*path.asString(), true))
                    return fail(ErrorCode::InvalidArgument, "symbol servers and network symbol paths are unsupported");
                auto wide = io::toWide(*path.asString());
                if (!wide)
                    return std::unexpected(std::move(wide.error()));
                if (!symbols.empty())
                    symbols += L';';
                symbols += *wide;
            }
        }
        // Explicit local paths only; never inherit _NT_SYMBOL_PATH or source-server settings.
        auto programFolder = io::toWide(io::parentPath(program.empty() ? dump : program));
        if (!programFolder)
            return std::unexpected(std::move(programFolder.error()));
        if (!symbols.empty())
            symbols += L';';
        symbols += *programFolder;
        HRESULT status = m_symbols->SetSymbolPathWide(symbols.c_str());
        if (FAILED(status))
            return std::unexpected(engineError("local symbol paths", status));
        m_symbols->SetSourcePathWide(L"");
        if (const auto* maps = arguments.find("sourceMap")) {
            auto configured = sourceMap(*maps);
            if (!configured)
                return std::unexpected(std::move(configured.error()));
        }
        m_dump = !dump.empty();
        m_attached = arguments.find("processId") != nullptr;
        m_stopOnEntry = arguments.boolAt("stopOnEntry").value_or(false);
        if (m_attached) {
            const auto pid = arguments.unsignedAt("processId");
            if (m_dump || !pid || *pid == 0 || *pid > std::numeric_limits<ULONG>::max() ||
                *pid == GetCurrentProcessId())
                return fail(ErrorCode::InvalidArgument, "Attach requires a distinct live process ID");
            status = m_client->AttachProcess(0, static_cast<ULONG>(*pid), DEBUG_ATTACH_DEFAULT);
        } else if (m_dump) {
            auto path = io::toWide(dump);
            if (!path)
                return std::unexpected(std::move(path.error()));
            status = m_client->OpenDumpFileWide(path->c_str(), 0);
        } else {
            std::vector<std::string> argv;
            if (const Json* args = arguments.find("args"); args != nullptr) {
                if (!args->isArray() || args->size() > 4096)
                    return fail(ErrorCode::InvalidArgument, "invalid debuggee arguments");
                for (const Json& argument : args->items()) {
                    if (!argument.asString() || argument.asString()->find('\0') != std::string::npos)
                        return fail(ErrorCode::InvalidArgument, "invalid debuggee argument");
                    argv.push_back(*argument.asString());
                }
            }
            auto command = io::buildWindowsCommandLine(program, argv);
            if (!command)
                return std::unexpected(std::move(command.error()));
            auto wideCommand = io::toWide(*command);
            auto wideCwd = io::toWide(cwd);
            if (!wideCommand || !wideCwd)
                return fail(ErrorCode::InvalidArgument, "debuggee command is not valid UTF-8");
            auto environment = makeEnvironment(arguments);
            if (!environment)
                return std::unexpected(std::move(environment.error()));
            DEBUG_CREATE_PROCESS_OPTIONS options{};
            // No DEBUG_ECREATE_PROCESS_INHERIT_HANDLES: the target must never inherit the
            // adapter's DAP pipe handles. DEBUG_OUTPUT_DEBUGGEE carries OutputDebugString only.
            options.CreateFlags = DEBUG_ONLY_THIS_PROCESS | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
            status = m_client->CreateProcessAndAttach2Wide(0, wideCommand->data(), &options, sizeof(options),
                                                           cwd.empty() ? nullptr : wideCwd->c_str(),
                                                           environment->data(), 0, 0);
        }
        if (FAILED(status))
            return std::unexpected(engineError(m_dump ? "open minidump" : "create debuggee", status));
        m_loaded = true; // From here every failure must clean up this owned session.
        m_ended = false;
        m_configured = false;
        status = m_control->WaitForEvent(0, 30000);
        if (status != S_OK) {
            close();
            return std::unexpected(engineError("initial debug event", status));
        }
        if (m_dump) {
            status = m_control->ExecuteWide(DEBUG_OUTCTL_THIS_CLIENT, L".ecxr", DEBUG_EXECUTE_NOT_LOGGED);
            if (FAILED(status))
                return std::unexpected(engineError("crash exception context", status));
        }
        if (!m_models.structured())
            m_event("output", Json::object()
                                  .set("category", "console")
                                  .set("output", "System32 DbgEng structured data model unavailable; native symbols "
                                                 "remain available, Natvis unavailable.\n"));
        if (const Json* files = arguments.find("natvis"); files != nullptr && m_models.structured()) {
            if (!files->isArray() || files->size() > 64)
                return fail(ErrorCode::InvalidArgument, "invalid Natvis file list");
            for (const Json& file : files->items()) {
                if (!file.asString() || !absoluteLocal(*file.asString(), true))
                    return fail(ErrorCode::InvalidArgument, "Natvis file must be an absolute local path");
                auto path = io::toWide(*file.asString());
                if (!path)
                    return std::unexpected(std::move(path.error()));
                const DWORD attributes = GetFileAttributesW(path->c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    return fail(ErrorCode::NotFound, "the requested Natvis file is unavailable");
                const std::wstring command = L".nvload \"" + *path + L"\"";
                m_natvisError = false;
                m_loadingNatvis = true;
                status = m_control->ExecuteWide(DEBUG_OUTCTL_THIS_CLIENT, command.c_str(), DEBUG_EXECUTE_NOT_LOGGED);
                m_loadingNatvis = false;
                if (FAILED(status))
                    return std::unexpected(engineError("load Natvis file", status));
                if (m_natvisError)
                    return fail(ErrorCode::Corrupt,
                                "DbgEng reported an error loading the Natvis file; see debugger output");
                if (std::ranges::find(m_natvisLoaded, *file.asString()) == m_natvisLoaded.end())
                    m_natvisLoaded.push_back(*file.asString());
                visualizersLoaded();
            }
        }
        if (!m_dump) {
            ULONG pid = 0;
            if (SUCCEEDED(m_systems->GetCurrentProcessSystemId(&pid)))
                m_event("process", Json::object()
                                       .set("name", program)
                                       .set("systemProcessId", pid)
                                       .set("isLocalProcess", true)
                                       .set("startMethod", m_attached ? "attach" : "launch"));
        }
        m_event("capabilities",
                Json::object().set("capabilities", Json::object()
                                                       .set("supportsTerminateRequest", !m_dump)
                                                       .set("supportsSetVariable", !m_dump)
                                                       .set("supportsDataBreakpoints", !m_dump)
                                                       .set("supportsWriteMemoryRequest", !m_dump && bool(m_memory))
                                                       .set("supportsReadMemoryRequest", bool(m_memory))
                                                       .set("frostSupportsRegisters", bool(m_registers))
                                                       .set("frostStructuredNatvis", m_models.structured())
                                                       .set("frostSupportsNatvisReload", m_models.structured())
                                                       .set("frostSupportsThreadFreeze", !m_dump)));
        return Json::object();
    }

    Result<Json> configure() {
        if (!m_loaded || m_configured)
            return fail(ErrorCode::Busy, "target is absent or configuration already completed");
        m_configured = true;
        if (m_dump || m_stopOnEntry) {
            stopped(m_dump ? "exception" : "entry", m_dump ? "Read-only crash dump" : "Initial process stop");
        } else if (auto resumed = execute(DEBUG_STATUS_GO); !resumed) {
            return std::unexpected(std::move(resumed.error()));
        }
        return Json::object();
    }

    Result<Json> breakpoints(const Json& arguments, bool functions) {
        if (!m_loaded || m_running)
            return fail(ErrorCode::Busy, "breakpoint updates require a stopped target");
        if (m_dump)
            return fail(ErrorCode::Unsupported, "a crash dump is read-only; breakpoints cannot execute");
        std::string key = "functions";
        std::string path;
        if (!functions) {
            const Json* source = arguments.find("source");
            path = source ? std::string(source->stringAt("path")) : std::string();
            if (!absoluteLocal(path))
                return fail(ErrorCode::InvalidArgument, "source breakpoint path must be absolute and local");
            key = path;
        }
        const Json* requested = arguments.find("breakpoints");
        if (requested == nullptr || !requested->isArray() || requested->size() > 4096)
            return fail(ErrorCode::InvalidArgument, "invalid breakpoint list");
        if (!m_breakpoints.contains(key) && m_breakpoints.size() >= 1024)
            return fail(ErrorCode::OutOfRange, "breakpoint source budget exhausted");
        std::size_t total = requested->size();
        for (const auto& [source, values] : m_breakpoints)
            if (source != key)
                total += values.size();
        if (total > 8192)
            return fail(ErrorCode::OutOfRange, "breakpoint budget exhausted");
        auto& previous = m_breakpoints[key];
        for (auto& breakpoint : previous) {
            ULONG id = 0;
            if (SUCCEEDED(breakpoint->GetId(&id)))
                m_policies.erase(id);
            m_control->RemoveBreakpoint2(breakpoint.get());
        }
        previous.clear();
        Json result = Json::array();
        for (const Json& request : requested->items()) {
            Json bound = Json::object().set("verified", false);
            const auto line = request.integerAt("line");
            if (line)
                bound.set("line", *line);
            if (request.stringAt("condition").size() > 4096 ||
                request.stringAt("condition").find('\0') != std::string_view::npos ||
                request.stringAt("logMessage").size() > 16384 ||
                request.stringAt("logMessage").find('\0') != std::string_view::npos ||
                !hitMatches(request.stringAt("hitCondition"), 1)) {
                result.push(std::move(bound).set("message", "Invalid conditional, hit-count or log breakpoint"));
                continue;
            }
            ComPtr<IDebugBreakpoint2> breakpoint;
            HRESULT status = m_control->AddBreakpoint2(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, breakpoint.put());
            if (SUCCEEDED(status)) {
                if (functions) {
                    const std::string name(request.stringAt("name"));
                    auto wide = io::toWide(name);
                    status = name.empty() || name.find('\0') != std::string::npos || !wide
                                 ? E_INVALIDARG
                                 : breakpoint->SetOffsetExpressionWide(wide->c_str());
                } else {
                    auto wide = io::toWide(mappedSource(path, true));
                    ULONG64 offset = 0;
                    status = !line || *line <= 0 || *line > std::numeric_limits<ULONG>::max() || !wide
                                 ? E_INVALIDARG
                                 : m_symbols->GetOffsetByLineWide(static_cast<ULONG>(*line), wide->c_str(), &offset);
                    if (SUCCEEDED(status))
                        status = breakpoint->SetOffset(offset);
                }
            }
            if (SUCCEEDED(status))
                status = breakpoint->AddFlags(DEBUG_BREAKPOINT_ENABLED);
            if (SUCCEEDED(status)) {
                ULONG id = 0;
                status = breakpoint->GetId(&id);
                if (SUCCEEDED(status)) {
                    ULONG flags = 0;
                    status = breakpoint->GetFlags(&flags);
                    if (SUCCEEDED(status)) {
                        const bool resolved = (flags & DEBUG_BREAKPOINT_DEFERRED) == 0;
                        bound.set("id", id).set("verified", resolved);
                        m_policies.insert_or_assign(id,
                                                    BreakpointPolicy{std::string(request.stringAt("condition")),
                                                                     std::string(request.stringAt("hitCondition")),
                                                                     std::string(request.stringAt("logMessage")), 0});
                        if (!resolved)
                            bound.set("message", "the debugger has not resolved this breakpoint");
                    }
                }
                if (SUCCEEDED(status)) {
                    previous.push_back(std::move(breakpoint));
                }
            }
            if (FAILED(status)) {
                if (breakpoint)
                    m_control->RemoveBreakpoint2(breakpoint.get());
                bound.set("message", engineError("bind breakpoint", status).message());
            }
            result.push(std::move(bound));
        }
        return Json::object().set("breakpoints", std::move(result));
    }

    Result<Json> threads() {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        ULONG count = 0;
        HRESULT status = m_systems->GetNumberThreads(&count);
        if (FAILED(status) || count > 65536)
            return std::unexpected(engineError("thread count", FAILED(status) ? status : E_BOUNDS));
        std::vector<ULONG> ids(count), systemIds(count);
        status = m_systems->GetThreadIdsByIndex(0, count, ids.data(), systemIds.data());
        if (FAILED(status))
            return std::unexpected(engineError("thread identifiers", status));
        m_threads.clear();
        Json result = Json::array();
        for (std::size_t i = 0; i < count; ++i) {
            m_threads.emplace(systemIds[i], ids[i]);
            result.push(Json::object().set("id", systemIds[i]).set("name", std::format("Thread {}", systemIds[i])));
        }
        return Json::object().set("threads", std::move(result));
    }

    Result<Json> stack(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        const auto thread = arguments.unsignedAt("threadId");
        const auto start = arguments.unsignedAt("startFrame").value_or(0);
        const auto levels = arguments.unsignedAt("levels").value_or(20);
        if (!thread || *thread > std::numeric_limits<ULONG>::max() || start > 4096 || levels > 4096 ||
            (arguments.find("startFrame") && !arguments.unsignedAt("startFrame")) ||
            (arguments.find("levels") && !arguments.unsignedAt("levels")))
            return fail(ErrorCode::InvalidArgument, "invalid stack page");
        if (auto selected = selectThread(static_cast<ULONG>(*thread)); !selected)
            return std::unexpected(std::move(selected.error()));
        std::array<DEBUG_STACK_FRAME, 4096> frames{};
        ULONG count = 0;
        HRESULT status = m_control->GetStackTrace(0, 0, 0, frames.data(), static_cast<ULONG>(frames.size()), &count);
        if (FAILED(status))
            return std::unexpected(engineError("stack trace", status));
        ULONG nativeThread = 0;
        if (FAILED(status = m_systems->GetCurrentThreadId(&nativeThread)))
            return std::unexpected(engineError("selected thread", status));
        Json result = Json::array();
        const auto end = std::min<std::uint64_t>(count, start + (levels == 0 ? 4096 : levels));
        for (std::uint64_t index = start; index < end; ++index) {
            if (m_frames.size() >= 16384 || m_nextFrame == std::numeric_limits<std::int64_t>::max())
                return fail(ErrorCode::OutOfRange, "frame handle budget exhausted");
            const std::int64_t id = m_nextFrame++;
            m_frames.emplace(id, FrameContext{nativeThread, static_cast<ULONG>(index)});
            std::array<wchar_t, 4096> name{}, file{};
            ULONG64 displacement = 0;
            const auto offset = frames[static_cast<std::size_t>(index)].InstructionOffset;
            const HRESULT named = m_symbols->GetNameByOffsetWide(offset, name.data(), static_cast<ULONG>(name.size()),
                                                                 nullptr, &displacement);
            auto frame = Json::object().set("id", id).set("instructionPointerReference", std::format("{:#x}", offset));
            auto converted = SUCCEEDED(named) ? utf8(name.data()) : Result<std::string>(std::format("{:#x}", offset));
            if (!converted)
                return std::unexpected(std::move(converted.error()));
            frame.set("name", std::move(*converted));
            ULONG line = 0;
            if (SUCCEEDED(m_symbols->GetLineByOffsetWide(offset, &line, file.data(), static_cast<ULONG>(file.size()),
                                                         nullptr, &displacement))) {
                auto convertedFile = utf8(file.data());
                if (!convertedFile)
                    return std::unexpected(std::move(convertedFile.error()));
                frame.set("source", Json::object().set("path", mappedSource(*convertedFile)));
                frame.set("line", line).set("column", 1);
            } else {
                frame.set("line", 0).set("column", 0);
            }
            result.push(std::move(frame));
        }
        return Json::object().set("stackFrames", std::move(result)).set("totalFrames", count);
    }

    Result<Json> scopes(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        const auto id = arguments.integerAt("frameId");
        if (!id || *id <= 0)
            return fail(ErrorCode::InvalidArgument, "scopes requires a current frame id");
        auto context = frame(id);
        if (!context)
            return std::unexpected(std::move(context.error()));
        auto locals = m_models.scope(*context, false);
        auto parameters = m_models.scope(*context, true);
        if (!locals)
            return std::unexpected(std::move(locals.error()));
        if (!parameters)
            return std::unexpected(std::move(parameters.error()));
        auto result = Json::array();
        result.push(Json::object()
                        .set("name", "Locals")
                        .set("presentationHint", "locals")
                        .set("expensive", false)
                        .set("variablesReference", *locals));
        result.push(Json::object()
                        .set("name", "Arguments")
                        .set("presentationHint", "arguments")
                        .set("expensive", false)
                        .set("variablesReference", *parameters));
        return Json::object().set("scopes", std::move(result));
    }

    Result<Json> variables(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        const auto reference = arguments.integerAt("variablesReference");
        const auto start = arguments.unsignedAt("start").value_or(0);
        const auto count = arguments.unsignedAt("count").value_or(0);
        if (!reference || *reference <= 0 || start > 65536 || count > 4096 ||
            (arguments.find("start") && !arguments.unsignedAt("start")) ||
            (arguments.find("count") && !arguments.unsignedAt("count")))
            return fail(ErrorCode::InvalidArgument, "invalid variable page");
        return m_models.children(
            *reference, static_cast<std::size_t>(start), static_cast<std::size_t>(count), arguments.stringAt("filter"),
            arguments.find("format") && arguments.find("format")->boolAt("frostRaw").value_or(false));
    }

    Result<Json> evaluate(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        const auto expression = arguments.stringAt("expression");
        if (expression.empty() || expression.size() > 16384 || expression.find('\0') != std::string_view::npos)
            return fail(ErrorCode::InvalidArgument, "invalid debugger expression");
        if (m_dump && !dumpExpression(expression))
            return fail(ErrorCode::Unsupported, "dump evaluation permits only read-only field and index lookups");
        if (arguments.find("frameId") && (!arguments.integerAt("frameId") || *arguments.integerAt("frameId") <= 0))
            return fail(ErrorCode::InvalidArgument, "invalid evaluate frame id");
        auto context = frame(arguments.integerAt("frameId"));
        if (!context)
            return std::unexpected(std::move(context.error()));
        auto result = m_models.evaluate(expression, *context);
        if (result && result->stringAt("memoryReference").empty()) {
            auto location = nativeInteger("&(" + std::string(expression) + ")");
            if (location)
                result->set("memoryReference", std::format("{:#x}", *location));
        }
        return result;
    }

    Result<Json> exceptions(const Json& arguments) {
        if (!m_loaded || m_running || m_dump)
            return fail(ErrorCode::Unsupported, "Exception filters require a stopped live target");
        const auto* filters = arguments.find("filters");
        if (!filters || !filters->isArray() || filters->size() > 3)
            return fail(ErrorCode::InvalidArgument, "Invalid exception filters");
        bool cpp = false, win32 = false;
        for (const auto& filter : filters->items()) {
            if (!filter.asString())
                return fail(ErrorCode::InvalidArgument, "Exception filter must be a string");
            if (*filter.asString() == "cpp_throw")
                cpp = true;
            else if (*filter.asString() == "win32")
                win32 = true;
            else
                return fail(ErrorCode::Unsupported, "Unknown native exception filter");
        }
        ULONG events = 0, specific = 0, arbitrary = 0;
        auto status = m_control->GetNumberEventFilters(&events, &specific, &arbitrary);
        if (FAILED(status) || specific + arbitrary > 4096)
            return fail(ErrorCode::OutOfRange, "Native exception filter budget exceeded");
        std::vector<DEBUG_EXCEPTION_FILTER_PARAMETERS> parameters(specific + arbitrary);
        if (parameters.empty())
            return fail(ErrorCode::Unsupported, "System32 engine has no exception filters");
        status = m_control->GetExceptionFilterParameters(static_cast<ULONG>(parameters.size()), nullptr, events,
                                                         parameters.data());
        if (FAILED(status))
            return std::unexpected(engineError("read native exception filters", status));
        bool cppPresent = false;
        for (auto& parameter : parameters) {
            if (parameter.ExceptionCode == EXCEPTION_BREAKPOINT || parameter.ExceptionCode == EXCEPTION_SINGLE_STEP)
                continue;
            const bool cppException = parameter.ExceptionCode == 0xe06d7363;
            cppPresent |= cppException;
            parameter.ExecutionOption =
                (cppException ? cpp : win32) ? DEBUG_FILTER_BREAK : DEBUG_FILTER_SECOND_CHANCE_BREAK;
        }
        if (!cppPresent) {
            DEBUG_EXCEPTION_FILTER_PARAMETERS parameter{};
            parameter.ExceptionCode = 0xe06d7363;
            parameter.ExecutionOption = cpp ? DEBUG_FILTER_BREAK : DEBUG_FILTER_SECOND_CHANCE_BREAK;
            parameter.ContinueOption = DEBUG_FILTER_GO_NOT_HANDLED;
            parameters.push_back(parameter);
        }
        status = m_control->SetExceptionFilterParameters(static_cast<ULONG>(parameters.size()), parameters.data());
        if (FAILED(status))
            return std::unexpected(engineError("native exception filters", status));
        return Json::object();
    }
    Result<Json> exceptionInfo() {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        ULONG type = 0, process = 0, thread = 0;
        DEBUG_LAST_EVENT_INFO_EXCEPTION exception{};
        std::array<wchar_t, 4096> description{};
        auto status =
            m_control->GetLastEventInformationWide(&type, &process, &thread, &exception, sizeof(exception), nullptr,
                                                   description.data(), static_cast<ULONG>(description.size()), nullptr);
        if (FAILED(status) || type != DEBUG_EVENT_EXCEPTION)
            return fail(ErrorCode::NotFound, "Current stop is not an exception");
        auto text = utf8(description.data());
        if (!text)
            return std::unexpected(std::move(text.error()));
        return Json::object()
            .set("exceptionId", std::format("{:#x}", exception.ExceptionRecord.ExceptionCode))
            .set("description", *text)
            .set("breakMode", exception.FirstChance ? "always" : "unhandled");
    }
    Result<Json> dataBreakpointInfo(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        if (m_dump)
            return fail(ErrorCode::Unsupported, "Crash dumps cannot install watchpoints");
        const auto name = arguments.stringAt("name");
        if (name.empty() || name.size() > 4096 || name.find('\0') != name.npos)
            return fail(ErrorCode::InvalidArgument, "Invalid data expression");
        // Resolve a real native lvalue and sizeof in the current scope; never guess object layout.
        Result<ULONG64> a = fail(ErrorCode::NotFound, "No data location"),
                        size = fail(ErrorCode::NotFound, "No data size");
        if (arguments.find("variablesReference") && !arguments.integerAt("variablesReference"))
            return fail(ErrorCode::InvalidArgument, "Invalid data parent reference");
        if (const auto reference = arguments.integerAt("variablesReference")) {
            auto location = m_models.location(*reference, name);
            if (!location)
                return std::unexpected(std::move(location.error()));
            a = location->first;
            size = location->second;
        } else {
            a = nativeInteger("&(" + std::string(name) + ")");
            size = nativeInteger("sizeof(" + std::string(name) + ")");
        }
        if (!a || !size || (*size != 1 && *size != 2 && *size != 4 && *size != 8) || *a % *size != 0)
            return Json::object()
                .set("dataId", nullptr)
                .set("description", "Native hardware watchpoints require an aligned 1, 2, 4 or 8 byte lvalue");
        if (m_dataIds.size() >= 4096)
            return fail(ErrorCode::OutOfRange, "Data reference budget exhausted");
        const auto id = std::format("{}:{}:{}", m_dataGeneration, *a, *size);
        m_dataIds.insert_or_assign(id, std::pair<ULONG64, ULONG>{*a, static_cast<ULONG>(*size)});
        return Json::object()
            .set("dataId", id)
            .set("description", std::string(name))
            .set("accessTypes", Json::array().push("write").push("readWrite"))
            .set("canPersist", false);
    }
    Result<Json> dataBreakpoints(const Json& arguments) {
        if (!m_loaded || m_running || m_dump)
            return fail(ErrorCode::Unsupported, "Data breakpoints require a stopped live target");
        const auto* requested = arguments.find("breakpoints");
        if (!requested || !requested->isArray() || requested->size() > 4)
            return fail(ErrorCode::InvalidArgument, "At most four hardware data breakpoints are supported");
        for (auto& previous : m_dataBreakpoints)
            (void)m_control->RemoveBreakpoint2(previous.get());
        m_dataBreakpoints.clear();
        Json result = Json::array();
        for (const auto& request : requested->items()) {
            auto item = Json::object().set("verified", false);
            auto found = m_dataIds.find(std::string(request.stringAt("dataId")));
            const auto access = request.stringAt("accessType");
            if (found == m_dataIds.end() || (access != "write" && access != "readWrite" && !access.empty()) ||
                !request.stringAt("condition").empty() || !request.stringAt("hitCondition").empty()) {
                result.push(std::move(item).set("message", "Expired/unsupported hardware data breakpoint"));
                continue;
            }
            ComPtr<IDebugBreakpoint2> breakpoint;
            auto status = m_control->AddBreakpoint2(DEBUG_BREAKPOINT_DATA, DEBUG_ANY_ID, breakpoint.put());
            if (SUCCEEDED(status))
                status = breakpoint->SetOffset(found->second.first);
            if (SUCCEEDED(status))
                status = breakpoint->SetDataParameters(found->second.second, access == "readWrite"
                                                                                 ? DEBUG_BREAK_READ | DEBUG_BREAK_WRITE
                                                                                 : DEBUG_BREAK_WRITE);
            if (SUCCEEDED(status))
                status = breakpoint->AddFlags(DEBUG_BREAKPOINT_ENABLED);
            ULONG id = 0;
            if (SUCCEEDED(status))
                status = breakpoint->GetId(&id);
            if (SUCCEEDED(status)) {
                item.set("verified", true).set("id", id);
                m_dataBreakpoints.push_back(std::move(breakpoint));
            } else {
                if (breakpoint)
                    (void)m_control->RemoveBreakpoint2(breakpoint.get());
                item.set("message", engineError("install hardware data breakpoint", status).message());
            }
            result.push(std::move(item));
        }
        return Json::object().set("breakpoints", std::move(result));
    }
    Result<Json> reloadNatvis(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        if (!m_models.structured())
            return fail(ErrorCode::Unsupported,
                        "System32 structured Natvis is unavailable; raw native symbols remain available");
        const auto* files = arguments.find("natvis");
        if (!files || !files->isArray() || files->size() > 64)
            return fail(ErrorCode::InvalidArgument, "Invalid Natvis list");
        for (const auto& file : files->items()) {
            if (!file.asString() || !absoluteLocal(*file.asString(), true))
                return fail(ErrorCode::InvalidArgument, "Natvis requires absolute local paths");
            auto path = io::toWide(*file.asString());
            if (!path)
                return std::unexpected(std::move(path.error()));
            auto attributes = GetFileAttributesW(path->c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
                return fail(ErrorCode::NotFound, "Natvis file unavailable");
            m_natvisError = false;
            m_loadingNatvis = true;
            const auto command = L".nvload \"" + *path + L"\"";
            auto status = m_control->ExecuteWide(DEBUG_OUTCTL_THIS_CLIENT, command.c_str(), DEBUG_EXECUTE_NOT_LOGGED);
            m_loadingNatvis = false;
            if (FAILED(status) || m_natvisError)
                return std::unexpected(engineError("load Natvis", FAILED(status) ? status : E_FAIL));
            if (std::ranges::find(m_natvisLoaded, *file.asString()) == m_natvisLoaded.end())
                m_natvisLoaded.push_back(*file.asString());
            visualizersLoaded();
        }
        m_models.clear();

        return Json::object().set("loaded", true).set("files", *files);
    }
    Result<Json> writeValue(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        if (m_dump)
            return fail(ErrorCode::Unsupported, "Crash dump values are read-only");
        const auto reference = arguments.integerAt("variablesReference");
        if (!reference || *reference <= 0)
            return fail(ErrorCode::InvalidArgument, "A current variable reference is required");
        return m_models.write(*reference, arguments.stringAt("name"), arguments.stringAt("value"));
    }
    Result<Json> memory(const Json& arguments, bool write) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        if (!m_memory)
            return fail(ErrorCode::Unsupported, "System32 DbgEng memory API unavailable");
        if (write && m_dump)
            return fail(ErrorCode::Unsupported, "Crash dump memory is read-only");
        if (arguments.find("offset") && !arguments.integerAt("offset"))
            return fail(ErrorCode::InvalidArgument, "Invalid memory offset");
        auto location = address(arguments.stringAt("memoryReference"), arguments.integerAt("offset").value_or(0));
        if (!location)
            return std::unexpected(std::move(location.error()));
        if (write) {
            auto bytes = unbase64(arguments.stringAt("data"));
            if (!bytes)
                return std::unexpected(std::move(bytes.error()));
            if (bytes->size() > 1024 * 1024)
                return fail(ErrorCode::OutOfRange, "Memory write exceeds 1 MiB");
            if (bytes->empty())
                return Json::object().set("offset", 0).set("bytesWritten", 0);
            ULONG written = 0;
            auto status = m_memory->WriteVirtual(*location, bytes->data(), static_cast<ULONG>(bytes->size()), &written);
            // A partial native write is never rolled back or reported as an all-or-nothing write.
            if (FAILED(status) && written == 0)
                return std::unexpected(engineError("write memory", status));
            if (written != bytes->size() && !arguments.boolAt("allowPartial").value_or(false))
                return fail(ErrorCode::IoError,
                            std::format("Native memory write changed {} of {} bytes", written, bytes->size()));
            return Json::object().set("offset", 0).set("bytesWritten", written);
        }
        const auto count = arguments.unsignedAt("count");
        if (!count || *count > 1024 * 1024)
            return fail(ErrorCode::InvalidArgument, "Memory count must be at most 1 MiB");
        if (*count == 0)
            return Json::object()
                .set("address", std::format("{:#x}", *location))
                .set("data", "")
                .set("unreadableBytes", 0);
        std::string bytes(static_cast<std::size_t>(*count), '\0');
        ULONG read = 0;
        auto status = m_memory->ReadVirtual(*location, bytes.data(), static_cast<ULONG>(bytes.size()), &read);
        if (FAILED(status) && read == 0)
            return std::unexpected(engineError("read memory", status));
        bytes.resize(read);
        return Json::object()
            .set("address", std::format("{:#x}", *location))
            .set("data", base64(bytes))
            .set("unreadableBytes", *count - read);
    }
    Result<Json> disassemble(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        auto location = address(arguments.stringAt("memoryReference"), arguments.integerAt("offset").value_or(0));
        if (!location)
            return std::unexpected(std::move(location.error()));
        const auto count = arguments.unsignedAt("instructionCount");
        const auto skip = arguments.integerAt("instructionOffset").value_or(0);
        if (!count || *count > 4096 || skip > 4096 || skip < -4096)
            return fail(ErrorCode::InvalidArgument, "Invalid disassembly page");
        ULONG64 current = *location;
        if (skip) {
            auto status = m_control->GetNearInstruction(current, static_cast<LONG>(skip), &current);
            if (FAILED(status))
                return std::unexpected(engineError("instruction offset", status));
        }
        Json instructions = Json::array();
        for (std::uint64_t index = 0; index < *count; ++index) {
            std::array<wchar_t, 4096> text{};
            ULONG64 next = 0;
            auto status =
                m_control->DisassembleWide(current, 0, text.data(), static_cast<ULONG>(text.size()), nullptr, &next);
            if (FAILED(status) || next <= current)
                return std::unexpected(engineError("disassemble instruction", FAILED(status) ? status : E_FAIL));
            auto converted = utf8(text.data());
            if (!converted)
                return std::unexpected(std::move(converted.error()));
            auto item = Json::object().set("address", std::format("{:#x}", current)).set("instruction", *converted);
            std::array<wchar_t, 4096> name{}, file{};
            ULONG64 displacement = 0;
            ULONG line = 0;
            if (SUCCEEDED(m_symbols->GetNameByOffsetWide(current, name.data(), static_cast<ULONG>(name.size()), nullptr,
                                                         &displacement)))
                if (auto value = utf8(name.data()); value)
                    item.set("symbol", *value);
            if (SUCCEEDED(m_symbols->GetLineByOffsetWide(current, &line, file.data(), static_cast<ULONG>(file.size()),
                                                         nullptr, &displacement)))
                if (auto value = utf8(file.data()); value)
                    item.set("location", Json::object().set("path", mappedSource(*value)))
                        .set("line", line)
                        .set("column", 1);
            instructions.push(std::move(item));
            current = next;
        }
        return Json::object().set("instructions", std::move(instructions));
    }
    Result<Json> registers(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        if (!m_registers)
            return fail(ErrorCode::Unsupported, "Native registers unavailable");
        if (arguments.find("threadId")) {
            const auto thread = arguments.unsignedAt("threadId");
            if (!thread || *thread > std::numeric_limits<ULONG>::max())
                return fail(ErrorCode::InvalidArgument, "Invalid thread ID");
            if (auto selected = selectThread(static_cast<ULONG>(*thread)); !selected)
                return std::unexpected(std::move(selected.error()));
        }
        ULONG count = 0;
        auto status = m_registers->GetNumberRegisters(&count);
        if (FAILED(status) || count > 4096)
            return std::unexpected(engineError("register count", FAILED(status) ? status : E_BOUNDS));
        Json values = Json::array();
        for (ULONG index = 0; index < count; ++index) {
            std::array<wchar_t, 256> name{};
            DEBUG_REGISTER_DESCRIPTION description{};
            DEBUG_VALUE value{};
            status = m_registers->GetDescriptionWide(index, name.data(), static_cast<ULONG>(name.size()), nullptr,
                                                     &description);
            if (SUCCEEDED(status))
                status = m_registers->GetValue(index, &value);
            if (FAILED(status))
                return std::unexpected(engineError("native register", status));
            auto converted = utf8(name.data());
            if (!converted)
                return std::unexpected(std::move(converted.error()));
            std::string text;
            switch (value.Type) {
            case DEBUG_VALUE_INT8: text = std::format("{:#x}", value.I8); break;
            case DEBUG_VALUE_INT16: text = std::format("{:#x}", value.I16); break;
            case DEBUG_VALUE_INT32: text = std::format("{:#x}", value.I32); break;
            case DEBUG_VALUE_INT64: text = std::format("{:#x}", value.I64); break;
            case DEBUG_VALUE_FLOAT32: text = std::format("{}", value.F32); break;
            case DEBUG_VALUE_FLOAT64: text = std::format("{}", value.F64); break;
            default: {
                constexpr char digits[] = "0123456789abcdef";
                const auto width = value.Type == DEBUG_VALUE_FLOAT80                                           ? 10U
                                   : value.Type == DEBUG_VALUE_FLOAT82                                         ? 11U
                                   : value.Type == DEBUG_VALUE_FLOAT128 || value.Type == DEBUG_VALUE_VECTOR128 ? 16U
                                   : value.Type == DEBUG_VALUE_VECTOR64                                        ? 8U
                                                                                                               : 0U;
                if (width == 0)
                    return fail(ErrorCode::Unsupported, "Unknown native register value width");
                text = "0x";
                for (std::size_t index = width; index > 0; --index) {
                    const auto byte = value.RawBytes[index - 1];
                    text += digits[byte >> 4];
                    text += digits[byte & 15];
                }
                break;
            }
            }
            values.push(Json::object()
                            .set("name", *converted)
                            .set("value", text)
                            .set("type", value.Type)
                            .set("typeName", value.Type == DEBUG_VALUE_FLOAT32   ? "float32"
                                             : value.Type == DEBUG_VALUE_FLOAT64 ? "float64"
                                             : value.Type <= DEBUG_VALUE_INT64   ? "integer"
                                                                                 : "native-vector")
                            .set("index", index));
        }
        return Json::object().set("registers", std::move(values));
    }
    Result<Json> modules(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        ULONG loaded = 0, unloaded = 0;
        auto status = m_symbols->GetNumberModules(&loaded, &unloaded);
        if (FAILED(status) || loaded > 65536)
            return std::unexpected(engineError("module count", FAILED(status) ? status : E_BOUNDS));
        const auto start = arguments.unsignedAt("startModule").value_or(0),
                   count = arguments.unsignedAt("moduleCount").value_or(0);
        if (start > 65536 || count > 4096)
            return fail(ErrorCode::InvalidArgument, "Invalid module page");
        Json result = Json::array();
        for (auto index = start; index < std::min<std::uint64_t>(loaded, start + (count ? count : 256)); ++index) {
            ULONG64 base = 0;
            status = m_symbols->GetModuleByIndex(static_cast<ULONG>(index), &base);
            if (FAILED(status))
                return std::unexpected(engineError("module base", status));
            DEBUG_MODULE_PARAMETERS parameter{};
            status = m_symbols->GetModuleParameters(1, &base, 0, &parameter);
            if (FAILED(status))
                return std::unexpected(engineError("module parameters", status));
            std::array<wchar_t, 32768> name{}, image{}, symbols{};
            status = m_symbols->GetModuleNameStringWide(DEBUG_MODNAME_MODULE, DEBUG_ANY_ID, base, name.data(),
                                                        static_cast<ULONG>(name.size()), nullptr);
            if (FAILED(status))
                return std::unexpected(engineError("module name", status));
            (void)m_symbols->GetModuleNameStringWide(DEBUG_MODNAME_IMAGE, DEBUG_ANY_ID, base, image.data(),
                                                     static_cast<ULONG>(image.size()), nullptr);
            (void)m_symbols->GetModuleNameStringWide(DEBUG_MODNAME_SYMBOL_FILE, DEBUG_ANY_ID, base, symbols.data(),
                                                     static_cast<ULONG>(symbols.size()), nullptr);
            auto n = utf8(name.data()), p = utf8(image.data()), sym = utf8(symbols.data());
            if (!n || !p || !sym)
                return fail(ErrorCode::Corrupt, "Invalid native module names");
            const auto symbolStatus = parameter.SymbolType == DEBUG_SYMTYPE_DEFERRED ? "Deferred"
                                      : parameter.SymbolType == DEBUG_SYMTYPE_NONE   ? "Not loaded"
                                      : parameter.SymbolType == DEBUG_SYMTYPE_EXPORT ? "Exports only"
                                                                                     : "Loaded";
            result.push(Json::object()
                            .set("id", std::format("{:#x}", base))
                            .set("name", *n)
                            .set("path", *p)
                            .set("addressRange", std::format("{:#x}-{:#x}", base, base + parameter.Size))
                            .set("symbolStatus", symbolStatus)
                            .set("symbolFilePath", *sym));
        }
        return Json::object().set("modules", std::move(result)).set("totalModules", loaded);
    }
    Result<Json> freeze(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        if (m_dump)
            return fail(ErrorCode::Unsupported, "Crash dump threads cannot be frozen");
        const auto thread = arguments.unsignedAt("threadId");
        const auto frozen = arguments.boolAt("frozen");
        if (!thread || *thread > std::numeric_limits<ULONG>::max() || !frozen)
            return fail(ErrorCode::InvalidArgument, "Freeze requires a thread and boolean operation");
        if (auto selected = selectThread(static_cast<ULONG>(*thread)); !selected)
            return std::unexpected(std::move(selected.error()));
        const auto id = m_threads.at(static_cast<ULONG>(*thread));
        const auto command = std::format(L"~{}{}", id, *frozen ? L'f' : L'u');
        m_commandError = false;
        m_capturingCommand = true;
        auto status = m_control->ExecuteWide(DEBUG_OUTCTL_THIS_CLIENT, command.c_str(), DEBUG_EXECUTE_NOT_LOGGED);
        m_capturingCommand = false;
        if (FAILED(status) || m_commandError)
            return std::unexpected(engineError("native thread freeze/thaw", FAILED(status) ? status : E_FAIL));
        return Json::object().set("threadId", *thread).set("commandSucceeded", true).set("operationApplied", true);
    }
    Result<Json> symbolPaths(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        const auto* paths = arguments.find("symbolPaths");
        if (!paths || !paths->isArray() || paths->size() > 64)
            return fail(ErrorCode::InvalidArgument, "Invalid symbol path list");
        const bool network = arguments.boolAt("allowSymbolServer").value_or(false);
        std::wstring joined;
        for (const auto& path : paths->items()) {
            if (!path.asString())
                return fail(ErrorCode::InvalidArgument, "Symbol path must be a string");
            const std::string_view value = *path.asString();
            // Symbol-server grammar is explicit, never inherited from the adapter environment.
            if (!absoluteLocal(value, true)) {
                if (!network || !value.starts_with("srv*") ||
                    (value.find_first_of("\r\n\";") != value.npos || value.find('\0') != value.npos))
                    return fail(ErrorCode::InvalidArgument, "Symbol server requires explicit opt-in");
                const auto separator = value.find('*', 4);
                if (separator == value.npos || !absoluteLocal(value.substr(4, separator - 4), true) ||
                    !value.substr(separator + 1).starts_with("https://") ||
                    value.find('*', separator + 1) != value.npos)
                    return fail(ErrorCode::InvalidArgument, "Use srv*absolute-local-cache*https://server");
            }
            auto wide = io::toWide(value);
            if (!wide)
                return std::unexpected(std::move(wide.error()));
            if (!joined.empty())
                joined += L';';
            joined += *wide;
        }
        auto status = network ? m_control->RemoveEngineOptions(DEBUG_ENGOPT_DISALLOW_NETWORK_PATHS)
                              : m_control->AddEngineOptions(DEBUG_ENGOPT_DISALLOW_NETWORK_PATHS);
        if (SUCCEEDED(status))
            status = network ? m_control->AddEngineOptions(DEBUG_ENGOPT_ALLOW_NETWORK_PATHS)
                             : m_control->RemoveEngineOptions(DEBUG_ENGOPT_ALLOW_NETWORK_PATHS);
        if (SUCCEEDED(status))
            status = m_symbols->SetSymbolPathWide(joined.c_str());
        if (FAILED(status))
            return std::unexpected(engineError("symbol path configuration", status));
        return Json::object().set("configured", true).set("symbolServerEnabled", network);
    }
    Result<Json> reloadSymbols(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        const auto module = arguments.stringAt("module");
        if (module.empty() || module.size() > 1024 ||
            module.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") !=
                module.npos)
            return fail(ErrorCode::InvalidArgument, "Reload requires one literal module name");
        auto wide = io::toWide(module);
        if (!wide)
            return std::unexpected(std::move(wide.error()));
        const auto status = m_symbols->ReloadWide(wide->c_str());
        if (FAILED(status))
            return std::unexpected(engineError("reload symbols", status));
        return Json::object().set("reloaded", true);
    }
    Result<Json> sourceMapRequest(const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        const auto* maps = arguments.find("sourceMap");
        if (!maps)
            return fail(ErrorCode::InvalidArgument, "sourceMap is required");
        return sourceMap(*maps);
    }
    Result<Json> sourceMap(const Json& maps) {
        if (!maps.isObject() || maps.size() > 64)
            return fail(ErrorCode::InvalidArgument, "Source map must be a bounded path object");
        std::vector<std::pair<std::string, std::string>> validated;
        for (std::size_t index = 0; index < maps.size(); ++index) {
            const auto* destination = maps.items()[index].asString();
            if (!destination || !absoluteLocal(maps.keys()[index]) || !absoluteLocal(*destination))
                return fail(ErrorCode::InvalidArgument, "Source mappings require absolute local roots");
            auto from = io::normalizePath(maps.keys()[index]), to = io::normalizePath(*destination);
            if (!from || !to)
                return fail(ErrorCode::InvalidArgument, "Invalid source mapping roots");
            validated.emplace_back(*from, *to);
        }
        std::ranges::sort(validated, [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
        m_sourceMap = std::move(validated);
        return Json::object().set("configured", true);
    }
    Result<Json> resume(std::string_view command, const Json& arguments) {
        if (auto ready = requireStop(); !ready)
            return std::unexpected(std::move(ready.error()));
        if (m_dump)
            return fail(ErrorCode::Unsupported, "a crash dump is read-only and cannot execute");
        if (const auto thread = arguments.unsignedAt("threadId"); thread) {
            if (*thread > std::numeric_limits<ULONG>::max())
                return fail(ErrorCode::InvalidArgument, "invalid thread id");
            if (auto selected = selectThread(static_cast<ULONG>(*thread)); !selected)
                return std::unexpected(std::move(selected.error()));
        }
        const auto granularity = arguments.stringAt("granularity");
        if (!granularity.empty() && granularity != "instruction" && granularity != "line" && granularity != "statement")
            return fail(ErrorCode::InvalidArgument, "Unknown stepping granularity");
        const auto level =
            m_control->SetCodeLevel(granularity == "instruction" ? DEBUG_LEVEL_ASSEMBLY : DEBUG_LEVEL_SOURCE);
        if (FAILED(level))
            return std::unexpected(engineError("stepping code level", level));
        ULONG execution = DEBUG_STATUS_GO;
        if (command == "next")
            execution = DEBUG_STATUS_STEP_OVER;
        if (command == "stepIn")
            execution = DEBUG_STATUS_STEP_INTO;
        if (command == "stepOut") {
            // gu uses the engine's actual return address/stack, not a predicted source line.
            m_models.clear();
            m_frames.clear();
            const HRESULT status = m_control->ExecuteWide(DEBUG_OUTCTL_THIS_CLIENT, L"gu", DEBUG_EXECUTE_NOT_LOGGED);
            if (FAILED(status))
                return std::unexpected(engineError("step out", status));
            m_running = true;
            observeExecution();
        } else if (auto executed = execute(execution); !executed) {
            return std::unexpected(std::move(executed.error()));
        }
        m_lastCommand = std::string(command);
        return Json::object().set("allThreadsContinued", true);
    }

    Result<Json> pause() {
        if (!m_loaded || !m_running || m_dump)
            return fail(ErrorCode::Unsupported, "pause requires a running live target");
        const HRESULT status = m_control->SetInterrupt(DEBUG_INTERRUPT_ACTIVE);
        if (FAILED(status))
            return std::unexpected(engineError("pause", status));
        m_lastCommand = "pause";
        return Json::object();
    }

    void poll() {
        if (!m_running)
            return;
        observeExecution();
        const HRESULT waited = m_control->WaitForEvent(0, 10);
        if (waited == S_FALSE)
            return;
        ULONG status = 0;
        const HRESULT execution = m_control->GetExecutionStatus(&status);
        // The last process can disappear while WaitForEvent returns an end-of-session status.
        // The actual engine state and GetExitCode decide whether this is an ordinary exit.
        if (SUCCEEDED(execution) && (status & DEBUG_STATUS_MASK) == DEBUG_STATUS_NO_DEBUGGEE) {
            ULONG exit = 0;
            if (SUCCEEDED(m_client->GetExitCode(&exit)))
                m_event("exited", Json::object().set("exitCode", exit));
            m_running = false;
            m_ended = true;
            m_event("terminated", Json::object());
        } else if (FAILED(waited) || FAILED(execution)) {
            m_event("output", Json::object()
                                  .set("category", "stderr")
                                  .set("output", engineError("wait for target event", waited).message() + "\n"));
            close();
            m_event("terminated", Json::object());
            return;
        } else if ((status & DEBUG_STATUS_MASK) == DEBUG_STATUS_BREAK) {
            ULONG type = 0, process = 0, thread = 0;
            DEBUG_LAST_EVENT_INFO_EXCEPTION exception{};
            std::array<wchar_t, 4096> description{};
            m_control->GetLastEventInformationWide(&type, &process, &thread, &exception, sizeof(exception), nullptr,
                                                   description.data(), static_cast<ULONG>(description.size()), nullptr);
            std::string reason = type == DEBUG_EVENT_BREAKPOINT ? "breakpoint" : "exception";
            if (type == DEBUG_EVENT_EXCEPTION && exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP)
                reason = "step";
            else if (m_lastCommand == "pause" && type == DEBUG_EVENT_EXCEPTION &&
                     exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT)
                reason = "pause";
            m_stopBreakpoint.reset();
            if (type == DEBUG_EVENT_BREAKPOINT) {
                DEBUG_LAST_EVENT_INFO_BREAKPOINT breakpoint{};
                if (SUCCEEDED(m_control->GetLastEventInformationWide(
                        &type, &process, &thread, &breakpoint, sizeof(breakpoint), nullptr, nullptr, 0, nullptr))) {
                    m_stopBreakpoint = breakpoint.Id;
                    ComPtr<IDebugBreakpoint2> native;
                    ULONG breakpointType = 0, processor = 0;
                    if (SUCCEEDED(m_control->GetBreakpointById2(breakpoint.Id, native.put())) &&
                        SUCCEEDED(native->GetType(&breakpointType, &processor)) &&
                        breakpointType == DEBUG_BREAKPOINT_DATA)
                        reason = "data breakpoint";
                    auto stop = breakpointStops(breakpoint.Id);
                    if (stop && !*stop) {
                        auto continued = execute(DEBUG_STATUS_GO);
                        if (continued)
                            return;
                    } else if (!stop)
                        m_event("output",
                                Json::object().set("category", "stderr").set("output", stop.error().message() + "\n"));
                }
            }
            auto text = utf8(description.data());
            stopped(reason, text ? *text : "");
        }
    }

    void finish(bool terminate) noexcept {
        m_terminateAttached = terminate;
        close();
    }
    void close() noexcept {
        m_natvisLoaded.clear();
        m_dataBreakpoints.clear();
        m_dataIds.clear();
        m_policies.clear();
        m_models.shutdown();
        m_frames.clear();
        m_threads.clear();
        m_breakpoints.clear();
        if (m_client) {
            if (m_loaded) {
                if (!m_dump)
                    if (!m_attached || m_terminateAttached)
                        (void)m_client->TerminateProcesses();
                (void)m_client->EndSession(m_dump                               ? DEBUG_END_PASSIVE
                                           : m_attached && !m_terminateAttached ? DEBUG_END_ACTIVE_DETACH
                                                                                : DEBUG_END_ACTIVE_TERMINATE);
            }
            (void)m_client->SetOutputCallbacksWide(nullptr);
        }
        m_memory.reset();
        m_registers.reset();
        m_systems.reset();
        m_symbols.reset();
        m_control.reset();
        m_client.reset();
        if (HMODULE library = std::exchange(m_library, nullptr))
            FreeLibrary(library);
        m_loaded = false;
        m_running = false;
        m_ended = true;
    }

private:
    void visualizersLoaded() {
        Json paths = Json::array();
        for (const auto& path : m_natvisLoaded)
            paths.push(path);
        m_event("frost.visualizersLoaded", Json::object().set("paths", std::move(paths)));
    }
    bool m_observedRunning = false;
    void observeExecution() {
        ULONG status = 0;
        if (SUCCEEDED(m_control->GetExecutionStatus(&status))) {
            const auto value = status & DEBUG_STATUS_MASK;
            if (value != DEBUG_STATUS_BREAK && value != DEBUG_STATUS_NO_DEBUGGEE && !m_observedRunning) {
                m_observedRunning = true;
                ULONG thread = 0;
                Json body = Json::object().set("allThreadsContinued", true);
                if (SUCCEEDED(m_systems->GetCurrentThreadSystemId(&thread)))
                    body.set("threadId", thread);
                m_event("continued", std::move(body));
            }
        }
    }
    struct BreakpointPolicy {
        std::string condition, hit, log;
        std::uint64_t count = 0;
    };
    std::map<ULONG, BreakpointPolicy> m_policies;
    std::map<std::string, std::pair<ULONG64, ULONG>> m_dataIds;
    std::vector<ComPtr<IDebugBreakpoint2>> m_dataBreakpoints;
    std::uint64_t m_dataGeneration = 0;
    Result<ULONG64> nativeInteger(std::string_view expression) {
        auto wide = io::toWide(expression);
        if (!wide)
            return std::unexpected(std::move(wide.error()));
        const auto syntax = m_control->SetExpressionSyntax(DEBUG_EXPR_CPLUSPLUS);
        if (FAILED(syntax))
            return std::unexpected(engineError("C++ expression syntax", syntax));
        DEBUG_VALUE value{};
        ULONG remainder = 0;
        auto status = m_control->EvaluateWide(wide->c_str(), DEBUG_VALUE_INT64, &value, &remainder);
        if (FAILED(status) || remainder != wide->size())
            return std::unexpected(engineError("native C++ expression", FAILED(status) ? status : E_INVALIDARG));
        return value.I64;
    }
    Result<bool> breakpointStops(ULONG id) {
        auto found = m_policies.find(id);
        if (found == m_policies.end())
            return true;
        auto& policy = found->second;
        if (policy.count != std::numeric_limits<std::uint64_t>::max())
            ++policy.count;
        auto hit = hitMatches(policy.hit, policy.count);
        if (!hit || !*hit)
            return hit;
        if (!policy.condition.empty()) {
            auto value = nativeInteger(policy.condition);
            if (!value)
                return std::unexpected(std::move(value.error()));
            if (!*value)
                return false;
        }
        if (policy.log.empty())
            return true;
        std::string message;
        for (std::size_t index = 0; index < policy.log.size();) {
            if (policy.log[index] != '{') {
                message += policy.log[index++];
                continue;
            }
            if (index + 1 < policy.log.size() && policy.log[index + 1] == '{') {
                message += '{';
                index += 2;
                continue;
            }
            const auto end = policy.log.find('}', index + 1);
            if (end == std::string::npos)
                return fail(ErrorCode::InvalidArgument, "Unclosed logpoint expression");
            auto value = m_models.evaluate(std::string_view(policy.log).substr(index + 1, end - index - 1),
                                           FrameContext{currentThread(), 0});
            if (!value)
                return std::unexpected(std::move(value.error()));
            message += value->stringAt("result");
            index = end + 1;
        }
        m_event("output", Json::object().set("category", "console").set("output", message + "\n"));
        return false;
    }
    ULONG currentThread() {
        ULONG id = 0;
        (void)m_systems->GetCurrentThreadId(&id);
        return id;
    }
    std::string mappedSource(std::string_view input, bool reverse = false) const {
        auto normalized = io::normalizePath(input);
        const std::string path = normalized ? *normalized : std::string(input);
        for (const auto& pair : m_sourceMap) {
            const auto& from = reverse ? pair.second : pair.first;
            const auto& to = reverse ? pair.first : pair.second;
            if (path.size() >= from.size() && _strnicmp(path.data(), from.c_str(), from.size()) == 0 &&
                (path.size() == from.size() || path[from.size()] == '/' || path[from.size()] == '\\'))
                return to + std::string(path.substr(from.size()));
        }
        return std::string(path);
    }
    std::optional<ULONG> m_stopBreakpoint;
    bool m_capturingCommand = false;
    bool m_commandError = false;
    struct EnvironmentLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        }
    };
    Result<std::wstring> makeEnvironment(const Json& arguments) {
        std::map<std::wstring, std::wstring, EnvironmentLess> values;
        if (arguments.boolAt("inheritEnvironment").value_or(true)) {
            wchar_t* environment = GetEnvironmentStringsW();
            if (environment == nullptr)
                return fail(ErrorCode::IoError, "debuggee environment is unavailable");
            for (const wchar_t* current = environment; *current != L'\0'; current += wcslen(current) + 1) {
                const std::wstring_view item(current);
                const auto equal = item.find(L'=', item.starts_with(L'=') ? 1 : 0);
                if (equal != std::wstring_view::npos)
                    values.emplace(std::wstring(item.substr(0, equal)), std::wstring(item.substr(equal + 1)));
            }
            FreeEnvironmentStringsW(environment);
        }
        if (const Json* overrides = arguments.find("env"); overrides != nullptr) {
            if (!overrides->isObject() || overrides->size() > 4096)
                return fail(ErrorCode::InvalidArgument, "invalid debuggee environment");
            for (std::size_t i = 0; i < overrides->size(); ++i) {
                const auto& key = overrides->keys()[i];
                const auto* value = overrides->items()[i].asString();
                if (key.empty() || key.find('=') != std::string::npos || key.find('\0') != std::string::npos ||
                    value == nullptr || value->find('\0') != std::string::npos)
                    return fail(ErrorCode::InvalidArgument, "invalid debuggee environment entry");
                auto wideKey = io::toWide(key), wideValue = io::toWide(*value);
                if (!wideKey || !wideValue)
                    return fail(ErrorCode::InvalidArgument, "invalid UTF-8 environment entry");
                values.insert_or_assign(std::move(*wideKey), std::move(*wideValue));
            }
        }
        std::wstring block;
        for (const auto& [key, value] : values) {
            if (block.size() + key.size() + value.size() + 2 > 1024 * 1024)
                return fail(ErrorCode::OutOfRange, "debuggee environment exceeds the bounded adapter limit");
            block += key + L'=' + value;
            block.push_back(L'\0');
        }
        block.push_back(L'\0');
        if (block.size() == 1)
            block.push_back(L'\0');
        return block;
    }

    Result<void> requireStop() const {
        if (!m_loaded || !m_configured || m_running || m_ended)
            return fail(ErrorCode::Busy, "inspection requires a current stopped target");
        return {};
    }
    Result<void> selectThread(ULONG systemId) {
        if (!m_threads.contains(systemId)) {
            auto found = threads();
            if (!found)
                return std::unexpected(std::move(found.error()));
        }
        const auto thread = m_threads.find(systemId);
        if (thread == m_threads.end())
            return fail(ErrorCode::NotFound, "the requested OS thread is not in this stop");
        const HRESULT status = m_systems->SetCurrentThreadId(thread->second);
        if (FAILED(status))
            return std::unexpected(engineError("select native thread", status));
        return {};
    }
    Result<FrameContext> frame(std::optional<std::int64_t> id) {
        if (id) {
            const auto found = m_frames.find(*id);
            if (found == m_frames.end())
                return fail(ErrorCode::Cancelled, "frame reference belongs to an expired stop");
            return found->second;
        }
        ULONG thread = 0, index = 0;
        HRESULT status = m_systems->GetCurrentThreadId(&thread);
        if (SUCCEEDED(status))
            status = m_symbols->GetCurrentScopeFrameIndex(&index);
        if (FAILED(status))
            return std::unexpected(engineError("current frame", status));
        return FrameContext{thread, index};
    }
    Result<void> execute(ULONG status) {
        m_models.clear();
        m_frames.clear();
        m_threads.clear();
        const HRESULT result = m_control->SetExecutionStatus(status);
        if (FAILED(result))
            return std::unexpected(engineError("resume target", result));
        m_running = true;
        observeExecution();
        return {};
    }
    void stopped(std::string_view reason, std::string_view description) {
        m_running = false;
        m_observedRunning = false;
        m_dataIds.clear();
        ++m_dataGeneration;
        m_lastCommand.clear();
        m_models.clear();
        m_frames.clear();
        m_threads.clear();
        ULONG systemThread = 0;
        Json body = Json::object().set("reason", reason).set("allThreadsStopped", true);
        if (m_stopBreakpoint)
            body.set("hitBreakpointIds", Json::array().push(*m_stopBreakpoint));
        if (!description.empty())
            body.set("description", description);
        if (SUCCEEDED(m_systems->GetCurrentThreadSystemId(&systemThread)))
            body.set("threadId", systemThread);
        m_event("stopped", std::move(body));
    }

    std::function<void(std::string_view, Json)> m_event;
    OutputSink m_output;
    HMODULE m_library = nullptr;
    ComPtr<IDebugClient5> m_client;
    ComPtr<IDebugControl4> m_control;
    ComPtr<IDebugSymbols3> m_symbols;
    ComPtr<IDebugSystemObjects4> m_systems;
    ComPtr<IDebugDataSpaces> m_memory;
    ComPtr<IDebugRegisters2> m_registers;
    bool m_attached = false;
    bool m_terminateAttached = false;
    std::vector<std::pair<std::string, std::string>> m_sourceMap;
    std::vector<std::string> m_natvisLoaded;
    ModelStore m_models;
    std::map<std::string, std::vector<ComPtr<IDebugBreakpoint2>>> m_breakpoints;
    std::map<ULONG, ULONG> m_threads;
    std::map<std::int64_t, FrameContext> m_frames;
    std::int64_t m_nextFrame = 1;
    bool m_loaded = false;
    bool m_configured = false;
    bool m_running = false;
    bool m_dump = false;
    bool m_stopOnEntry = false;
    bool m_ended = false;
    bool m_loadingNatvis = false;
    bool m_natvisError = false;
    std::string m_lastCommand;
};

class Server {
public:
    int run() {
        std::thread input([this] {
            readRequests();
            m_inputDone = true;
            m_shutdownChanged.notify_all();
        });
        std::thread output([this] {
            writeOutput();
            m_outputDone = true;
            m_shutdownChanged.notify_all();
        });
        std::thread owner([this] { work(); });
        {
            std::unique_lock lock(m_shutdownMutex);
            m_shutdownChanged.wait(lock, [this] { return m_closed.load(); });
        }
        // DbgEng cleanup never waits for an output consumer. No COM pointer crosses to an I/O
        // thread; the owner ends/terminates its target before the adapter can finish shutdown.
        (void)CancelSynchronousIo(input.native_handle());
        (void)CancelSynchronousIo(output.native_handle());
        owner.join();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!m_inputDone.load() || !m_outputDone.load()) {
            // Repeat cancellation to close the race where shutdown preceded ReadFile/WriteFile
            // entering the kernel. Synchronous pipe I/O is cancellable on supported Windows 11.
            if (!m_inputDone.load())
                (void)CancelSynchronousIo(input.native_handle());
            if (!m_outputDone.load())
                (void)CancelSynchronousIo(output.native_handle());
            if (std::chrono::steady_clock::now() >= deadline) {
                // The engine owner has already joined and released every target/interface. An
                // unresponsive OS pipe must not keep this separate adapter process alive forever.
                (void)TerminateProcess(GetCurrentProcess(), 2);
                std::terminate();
            }
            std::unique_lock lock(m_shutdownMutex);
            m_shutdownChanged.wait_for(lock, 5ms, [this] { return m_inputDone.load() && m_outputDone.load(); });
        }
        input.join();
        output.join();
        return m_failed.load() ? 2 : 0;
    }

private:
    void stop(bool failure) {
        {
            // Every wait's shutdown predicate changes under its own mutex. A notification
            // cannot slip between a false predicate check and the corresponding wait.
            std::scoped_lock lock(m_mutex, m_outputMutex, m_shutdownMutex);
            if (failure)
                m_failed = true;
            m_closed = true;
        }
        m_changed.notify_all();
        m_outputChanged.notify_all();
        m_shutdownChanged.notify_all();
    }
    void readRequests() {
        rpc::FrameDecoder decoder(kMaxMessage);
        std::array<char, 65536> buffer{};
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        bool okay = true;
        while (!m_closed.load()) {
            DWORD read = 0;
            if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                okay = m_closed.load() || GetLastError() == ERROR_BROKEN_PIPE;
                break;
            }
            if (read == 0)
                break;
            decoder.feed(std::string_view(buffer.data(), read));
            while (auto frame = decoder.next()) {
                if (!*frame) {
                    okay = false;
                    break;
                }
                auto message = rpc::parseJson(**frame);
                if (!message || !message->isObject() || message->stringAt("type") != "request" ||
                    !message->integerAt("seq") || *message->integerAt("seq") <= 0 ||
                    message->stringAt("command").empty()) {
                    okay = false;
                    break;
                }
                {
                    std::scoped_lock lock(m_mutex);
                    if (m_requests.size() >= 256 || m_queuedBytes + frame->value().size() > kMaxQueue) {
                        okay = false;
                        break;
                    }
                    m_queuedBytes += frame->value().size();
                    m_requests.emplace_back(std::move(*message), frame->value().size());
                }
                m_changed.notify_one();
            }
            if (!okay)
                break;
        }
        stop(!okay);
    }
    void writeOutput() {
        for (;;) {
            std::string bytes;
            {
                std::unique_lock lock(m_outputMutex);
                m_outputChanged.wait(lock, [this] { return m_closed.load() || !m_output.empty(); });
                if (m_closed.load())
                    return;
                bytes = std::move(m_output.front());
                m_output.pop_front();
                m_outputBytes -= bytes.size();
            }
            // Only this thread can block on DAP stdout. At most one bounded active frame plus
            // kMaxQueue/256 queued frames are retained. EOF/failure cancels this synchronous I/O.
            std::size_t offset = 0;
            while (offset < bytes.size() && !m_closed.load()) {
                DWORD written = 0;
                if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data() + offset,
                               static_cast<DWORD>(bytes.size() - offset), &written, nullptr) ||
                    written == 0) {
                    stop(!m_closed.load());
                    return;
                }
                offset += written;
            }
        }
    }
    void write(Json message) {
        bool overflow = false;
        {
            std::scoped_lock lock(m_outputMutex);
            if (m_closed.load())
                return;
            if (m_sequence == std::numeric_limits<std::int64_t>::max()) {
                overflow = true;
            } else {
                message.set("seq", ++m_sequence);
                std::string bytes = rpc::encodeFrame(message.dump());
                if (bytes.size() > kMaxMessage || m_output.size() >= 256 || m_outputBytes + bytes.size() > kMaxQueue) {
                    overflow = true;
                } else {
                    m_outputBytes += bytes.size();
                    m_output.push_back(std::move(bytes));
                }
            }
        }
        if (overflow)
            stop(true); // Never silently drop a response on a still-open connection.
        else
            m_outputChanged.notify_one();
    }
    void event(std::string_view name, Json body) {
        write(Json::object().set("type", "event").set("event", name).set("body", std::move(body)));
    }
    void respond(const Json& request, Result<Json> result) {
        Json message = Json::object()
                           .set("type", "response")
                           .set("request_seq", *request.integerAt("seq"))
                           .set("command", request.stringAt("command"))
                           .set("success", result.has_value());
        if (result)
            message.set("body", std::move(*result));
        else
            message.set("message", result.error().message());
        write(std::move(message));
    }
    void work() {
        Engine engine([this](std::string_view name, Json body) { event(name, std::move(body)); });
        bool initialized = false;
        while (!m_closed.load()) {
            std::optional<Json> request;
            {
                std::unique_lock lock(m_mutex);
                if (m_requests.empty() && !engine.running())
                    m_changed.wait(lock, [this] { return m_closed.load() || !m_requests.empty(); });
                if (m_closed.load())
                    break;
                if (!m_requests.empty()) {
                    request = std::move(m_requests.front().first);
                    m_queuedBytes -= m_requests.front().second;
                    m_requests.pop_front();
                }
            }
            if (request) {
                const auto command = request->stringAt("command");
                const Json empty = Json::object();
                const Json* arguments = request->find("arguments");
                if (arguments == nullptr)
                    arguments = &empty;
                if (!arguments->isObject()) {
                    respond(*request, fail(ErrorCode::InvalidArgument, "DAP arguments must be an object"));
                } else if (command == "initialize" && !initialized) {
                    initialized = true;
                    respond(*request, Json::object()
                                          .set("supportsConfigurationDoneRequest", true)
                                          .set("supportsFunctionBreakpoints", true)
                                          .set("supportsEvaluateForHovers", true)
                                          .set("supportsTerminateRequest", true)
                                          .set("supportsDelayedStackTraceLoading", true)
                                          .set("supportsSetVariable", true)
                                          .set("supportsConditionalBreakpoints", true)
                                          .set("supportsHitConditionalBreakpoints", true)
                                          .set("supportsLogPoints", true)
                                          .set("supportsDataBreakpoints", true)
                                          .set("supportsReadMemoryRequest", true)
                                          .set("supportsWriteMemoryRequest", true)
                                          .set("supportsDisassembleRequest", true)
                                          .set("supportsSteppingGranularity", true)
                                          .set("supportsModulesRequest", true)
                                          .set("supportsExceptionInfoRequest", true)
                                          .set("frostSupportsRegisters", true)
                                          .set("frostSupportsSymbolControl", true)
                                          .set("frostSupportsSourceMap", true)
                                          .set("frostSupportsRawView", true)
                                          .set("frostSupportsThreadFreeze", true)
                                          .set("exceptionBreakpointFilters",
                                               Json::array()
                                                   .push(Json::object()
                                                             .set("filter", "cpp_throw")
                                                             .set("label", "C++ Throw")
                                                             .set("default", false))
                                                   .push(Json::object()
                                                             .set("filter", "win32")
                                                             .set("label", "Win32 first-chance exceptions")
                                                             .set("default", false)))
                                          .set("supportsRestartRequest", false));
                } else if (!initialized) {
                    respond(*request, fail(ErrorCode::InvalidArgument, "initialize must precede target requests"));
                } else if (command == "launch" || command == "attach") {
                    auto launched = engine.launch(*arguments);
                    const bool success = launched.has_value();
                    respond(*request, std::move(launched));
                    if (success)
                        event("initialized", Json::object());
                    else
                        engine.close();
                } else if (command == "configurationDone")
                    respond(*request, engine.configure());
                else if (command == "setBreakpoints")
                    respond(*request, engine.breakpoints(*arguments, false));
                else if (command == "setFunctionBreakpoints")
                    respond(*request, engine.breakpoints(*arguments, true));
                else if (command == "setExceptionBreakpoints")
                    respond(*request, engine.exceptions(*arguments));
                else if (command == "exceptionInfo")
                    respond(*request, engine.exceptionInfo());
                else if (command == "setVariable")
                    respond(*request, engine.writeValue(*arguments));
                else if (command == "readMemory" || command == "writeMemory")
                    respond(*request, engine.memory(*arguments, command == "writeMemory"));
                else if (command == "disassemble")
                    respond(*request, engine.disassemble(*arguments));
                else if (command == "modules")
                    respond(*request, engine.modules(*arguments));
                else if (command == "frost.registers")
                    respond(*request, engine.registers(*arguments));
                else if (command == "frost.setThreadFrozen")
                    respond(*request, engine.freeze(*arguments));
                else if (command == "frost.setSymbolPaths")
                    respond(*request, engine.symbolPaths(*arguments));
                else if (command == "frost.reloadSymbols")
                    respond(*request, engine.reloadSymbols(*arguments));
                else if (command == "frost.reloadNatvis")
                    respond(*request, engine.reloadNatvis(*arguments));
                else if (command == "frost.setSourceMap")
                    respond(*request, engine.sourceMapRequest(*arguments));
                else if (command == "dataBreakpointInfo")
                    respond(*request, engine.dataBreakpointInfo(*arguments));
                else if (command == "setDataBreakpoints")
                    respond(*request, engine.dataBreakpoints(*arguments));
                else if (command == "threads")
                    respond(*request, engine.threads());
                else if (command == "stackTrace")
                    respond(*request, engine.stack(*arguments));
                else if (command == "scopes")
                    respond(*request, engine.scopes(*arguments));
                else if (command == "variables")
                    respond(*request, engine.variables(*arguments));
                else if (command == "evaluate")
                    respond(*request, engine.evaluate(*arguments));
                else if (command == "pause")
                    respond(*request, engine.pause());
                else if (command == "continue" || command == "next" || command == "stepIn" || command == "stepOut")
                    respond(*request, engine.resume(command, *arguments));
                else if (command == "disconnect" || command == "terminate") {
                    if (command == "terminate" && engine.readOnly())
                        respond(*request, fail(ErrorCode::Unsupported, "a crash dump has no process to terminate"));
                    else {
                        engine.finish(command == "terminate" || arguments->boolAt("terminateDebuggee").value_or(false));
                        respond(*request, Json::object());
                        event("terminated", Json::object());
                    }
                } else
                    respond(*request, fail(ErrorCode::Unsupported, "request is unavailable on this DbgEng backend"));
            }
            engine.poll();
        }
        engine.close();
    }

    std::atomic<bool> m_closed{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_inputDone{false};
    std::atomic<bool> m_outputDone{false};
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::pair<Json, std::size_t>> m_requests;
    std::size_t m_queuedBytes = 0;
    std::mutex m_outputMutex;
    std::condition_variable m_outputChanged;
    std::deque<std::string> m_output;
    std::size_t m_outputBytes = 0;
    std::int64_t m_sequence = 0;
    std::mutex m_shutdownMutex;
    std::condition_variable m_shutdownChanged;
};
} // namespace

int runAdapter() { return Server().run(); }

} // namespace frost::dbgeng
