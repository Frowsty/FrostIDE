// SPDX-License-Identifier: Apache-2.0
// Prepares debugger launch arguments and an adapter for standalone Lua.
#include "frost/debug/standalone_lua_dap.hpp"

#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

namespace frost::debug {
namespace {
bool samePath(std::string_view proposed, std::string_view expected) {
    const auto a = io::normalizePath(proposed);
    const auto b = io::normalizePath(expected);
    return a && b && *a == *b;
}

rpc::Json luaEnvironment(const StandaloneLuaTarget& target) {
    auto environment = rpc::Json::object();
    for (const auto& [name, value] : target.environment)
        environment.set(name, value);
    return environment;
}

// The upstream descriptor's Windows and POSIX spelling. Validate the proposal, then build
// our own Lua literal from the admitted local script path and the held adapter's PID.
bool matchesBootstrap(std::string_view proposed, std::string_view bootstrap, io::ProcessId pid) {
#ifdef _WIN32
    constexpr std::string_view prefix = "dofile[[", separator = "]] DBG[[", suffix = "]]";
#else
    constexpr std::string_view prefix = "dofile\"", separator = "\" DBG\"", suffix = "\"";
#endif
    if (!proposed.starts_with(prefix) || !proposed.ends_with(suffix))
        return false;
    proposed.remove_prefix(prefix.size());
    proposed.remove_suffix(suffix.size());
    const auto split = proposed.find(separator);
    return split != std::string_view::npos && samePath(proposed.substr(0, split), bootstrap) &&
           proposed.substr(split + separator.size()) == std::to_string(pid);
}

bool matchesTerminal(const rpc::Json& request, const StandaloneLuaTarget& target, std::string_view bootstrap,
                     io::ProcessId pid) {
    const auto* args = request.find("args");
    const auto* env = request.find("env");
    const auto environment = luaEnvironment(target);
    const auto* shell = request.find("argsCanBeInterpretedByShell");
    if (request.stringAt("kind") != "integrated" || (shell && shell->asBool() != false) ||
        !samePath(request.stringAt("cwd"), target.workingDirectory) || !env || !env->isObject() ||
        env->size() != environment.size() || !args || !args->isArray() ||
        args->size() != 4 + target.interpreterArguments.size() + target.arguments.size())
        return false;
    for (const auto& [name, value] : target.environment) {
        const auto* entry = env->find(name);
        if (!entry || !entry->asString() || *entry->asString() != value)
            return false;
    }
    const auto& items = args->items();
    const auto isString = [](const rpc::Json& item, std::string_view expected) {
        const auto* text = item.asString();
        return text && *text == expected;
    };
    if (!items[0].asString() || !samePath(*items[0].asString(), target.interpreter) || !isString(items[1], "-e") ||
        !items[2].asString() || !matchesBootstrap(*items[2].asString(), bootstrap, pid))
        return false;
    std::size_t index = 3;
    for (const auto& value : target.interpreterArguments) {
        if (!isString(items[index++], value))
            return false;
    }
    const auto program = items[index++].asString();
    if (!program || !samePath(*program, target.script))
        return false;
    for (const auto& value : target.arguments) {
        if (!isString(items[index++], value))
            return false;
    }
    return true;
}

class OwnedLuaProcess {
public:
    OwnedLuaProcess(io::IProcessLauncher& launcher, StandaloneLuaTarget target, std::string bootstrap,
                    io::ProcessId adapterPid, const DapHandlers& handlers)
        : m_launcher(launcher), m_target(std::move(target)), m_bootstrap(std::move(bootstrap)),
          m_adapterPid(adapterPid), m_admit(handlers.beforeWrite), m_event(handlers.onEvent) {}

    Result<void> beforeWrite(std::string_view command) {
        if (m_admit) {
            if (auto admitted = m_admit(command); !admitted)
                return admitted;
        }
        if (command == "launch")
            m_launchSent = true;
        return {};
    }

    Result<rpc::Json> reverse(std::string_view command, const rpc::Json& arguments) {
        if (command != "runInTerminal" || !m_launchSent || m_started ||
            !matchesTerminal(arguments, m_target, m_bootstrap, m_adapterPid))
            return fail(ErrorCode::PermissionDenied, "Lua terminal request differs from the admitted debug profile");
        if (m_admit) {
            if (auto admitted = m_admit("launch"); !admitted)
                return std::unexpected(admitted.error());
        }

        io::ProcessSpec spec;
        spec.executable = m_target.interpreter;
        spec.workingDirectory = m_target.workingDirectory;
        spec.arguments = {"-e", "dofile[=[" + m_bootstrap + "]=] DBG[=[" + std::to_string(m_adapterPid) + "]=]"};
        spec.arguments.insert(spec.arguments.end(), m_target.interpreterArguments.begin(),
                              m_target.interpreterArguments.end());
        spec.arguments.push_back(m_target.script);
        spec.arguments.insert(spec.arguments.end(), m_target.arguments.begin(), m_target.arguments.end());
        for (const auto& [name, value] : m_target.environment)
            spec.environment.emplace_back(name, value);
        spec.stdoutMode = io::Stdio::Pipe;
        spec.stderrMode = io::Stdio::Pipe;
        spec.killOnClose = true;

        LockGuard lock(m_mutex);
        if (m_cancelled)
            return fail(ErrorCode::Cancelled, "Lua debug launch was cancelled");
        m_started = true;
        auto launched = m_launcher.launch(spec);
        if (!launched)
            return std::unexpected(launched.error());
        m_process = std::move(*launched);
        auto* process = m_process.get();
        m_stdout = std::jthread([this, process] { drain(*process, false); });
        m_stderr = std::jthread([this, process] { drain(*process, true); });
        return rpc::Json::object().set("processId", static_cast<std::int64_t>(process->pid()));
    }

    std::optional<int> finish() {
        io::IProcess* process = nullptr;
        {
            LockGuard lock(m_mutex);
            process = m_process.get();
        }
        if (!process)
            return std::nullopt;
        auto status = process->wait(std::chrono::seconds(2));
        const bool naturalExit = status && !m_cancelled;
        // Reap the owned container's remaining descendants so none can hold the pipes open.
        if (auto ended = process->terminate(); !ended)
            process->abandonOutput();
        m_stdout.join();
        m_stderr.join();
        return naturalExit ? std::optional<int>(status->exitCode) : std::nullopt;
    }

    void cancel() noexcept {
        m_cancelled = true;
        LockGuard lock(m_mutex);
        if (m_process) {
            static_cast<void>(m_process->terminate());
            m_process->abandonOutput();
        }
    }

    // Set and read only on the DAP reader during adapter EOF handling.
    bool cleanAdapterClose = false;

private:
    void drain(io::IProcess& process, bool stderrStream) const {
        std::array<char, 4096> buffer{};
        for (;;) {
            auto read = stderrStream ? process.readStderr(buffer) : process.readStdout(buffer);
            if (!read || *read == 0)
                break;
            if (m_event)
                m_event("output", rpc::Json::object()
                                      .set("category", stderrStream ? "stderr" : "stdout")
                                      .set("output", std::string(buffer.data(), *read)));
        }
    }

    io::IProcessLauncher& m_launcher;
    const StandaloneLuaTarget m_target;
    const std::string m_bootstrap;
    const io::ProcessId m_adapterPid;
    const std::function<Result<void>(std::string_view)> m_admit;
    const std::function<void(std::string_view, const rpc::Json&)> m_event;
    std::atomic<bool> m_launchSent = false;
    std::atomic<bool> m_cancelled = false;
    bool m_started = false; // Only the DAP reader consumes the one-shot reverse request.
    Mutex m_mutex;
    std::unique_ptr<io::IProcess> m_process FROST_GUARDED_BY(m_mutex);
    std::jthread m_stdout;
    std::jthread m_stderr;
};

class OwnedLuaTransport final : public IDapTransport {
public:
    OwnedLuaTransport(std::unique_ptr<IDapTransport> adapter, std::shared_ptr<OwnedLuaProcess> target)
        : m_adapter(std::move(adapter)), m_target(std::move(target)) {}
    Result<std::size_t> read(std::span<char> buffer) override {
        auto result = m_adapter->read(buffer);
        m_eof = result && *result == 0;
        return result;
    }
    Result<void> write(std::string_view bytes) override { return m_adapter->write(bytes); }
    void close() noexcept override {
        m_target->cancel();
        m_adapter->close();
    }
    std::optional<int> exitCode() noexcept override {
        const auto status = m_adapter->exitCode();
        m_target->cleanAdapterClose = m_eof && status == 0;
        return status;
    }

private:
    std::unique_ptr<IDapTransport> m_adapter;
    std::shared_ptr<OwnedLuaProcess> m_target;
    bool m_eof = false; // Only the reader calls read and exitCode.
};
} // namespace

Result<rpc::Json> standaloneLuaLaunchArguments(const StandaloneLuaTarget& target) {
    const auto invalidPath = [](const std::string& path) {
        return !io::isAbsolutePath(path) || path.find('\0') != std::string::npos;
    };
    if (invalidPath(target.interpreter) || invalidPath(target.script) || invalidPath(target.workingDirectory))
        return fail(ErrorCode::InvalidArgument, "Lua interpreter, script and working directory must be absolute");
    rpc::Json arg0 = rpc::Json::array(), arg = rpc::Json::array();
    for (const auto& value : target.interpreterArguments) {
        if (value.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "NUL in interpreter argument");
        arg0.push(value);
    }
    for (const auto& value : target.arguments) {
        if (value.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "NUL in script argument");
        arg.push(value);
    }
    for (const auto& [name, value] : target.environment) {
        if (name.empty() || name.find_first_of("=\0", 0, 2) != std::string::npos ||
            value.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "invalid Lua environment entry");
    }
    return rpc::Json::object()
        .set("type", "lua")
        .set("request", "launch")
        .set("luaexe", target.interpreter)
        .set("luaVersion", "lua54")
        .set("program", target.script)
        .set("arg0", std::move(arg0))
        .set("arg", std::move(arg))
        .set("cwd", target.workingDirectory)
        .set("env", luaEnvironment(target))
        .set("console", "integratedTerminal")
        .set("sourceCoding", "utf8")
        .set("useWSL", false)
        .set("stopOnEntry", target.stopOnEntry)
        .set("outputCapture", rpc::Json::array());
}
AdapterFactory standaloneLuaAdapterFactory(io::IProcessLauncher& launcher, std::string executable,
                                           StandaloneLuaTarget target) {
    return [&launcher, executable = std::move(executable),
            target = std::move(target)](DapHandlers handlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        auto normalized = io::normalizePath(executable);
        if (!normalized)
            return fail(ErrorCode::InvalidArgument, "configure an absolute installed lua-debug executable");
        const auto bin = io::parentPath(*normalized);
        auto bootstrap = io::joinPath(io::parentPath(bin), "script/launch.lua");
        if (io::fileName(bin) != "bin" || !bootstrap || bootstrap->find("]=]") != std::string::npos ||
            bootstrap->find_first_of("\r\n") != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "configure the installed lua-debug extension's bin executable");
        io::ProcessSpec spec;
        spec.executable = executable;
        spec.workingDirectory = std::string(bin);
        spec.stdinMode = io::Stdio::Pipe;
#ifdef __linux__
        // lua-debug uses bee.subprocess.peek on stdin, which requires a socket on Linux.
        spec.posixSocketStdin = true;
#endif
        spec.stdoutMode = io::Stdio::Pipe;
        spec.stderrMode = io::Stdio::Pipe;
        spec.killOnClose = true;
        auto child = launcher.launch(spec);
        if (!child)
            return std::unexpected(child.error().withContext("starting standalone lua-debug"));
        DapProcessOutput output;
        output.privateOutput = handlers.privateSession;
        auto owned =
            std::make_shared<OwnedLuaProcess>(launcher, target, std::move(*bootstrap), (*child)->pid(), handlers);
        handlers.supportsOwnedRunInTerminal = true;
        handlers.beforeWrite = [owned](std::string_view command) { return owned->beforeWrite(command); };
        handlers.onReverseRequest = [owned](std::string_view command, const rpc::Json& args) {
            return owned->reverse(command, args);
        };
        handlers.onEvent = [event = std::move(handlers.onEvent)](std::string_view name, const rpc::Json& body) {
            // The held process is the sole source for target output and completion evidence.
            const auto category = body.stringAt("category");
            if (event && name != "exited" && name != "terminated" &&
                (name != "output" || (category != "stdout" && category != "stderr")))
                event(name, body);
        };
        handlers.onClosed = [owned, closed = std::move(handlers.onClosed),
                             exited = std::move(handlers.onOwnedTargetExit)](const Error& why) {
            if (auto status = owned->finish(); status && exited)
                exited(*status, owned->cleanAdapterClose);
            if (closed)
                closed(why);
        };
        auto transport = processTransport(std::move(*child), std::move(output));
        return makeDapClient(std::make_unique<OwnedLuaTransport>(std::move(transport), std::move(owned)),
                             std::move(handlers));
    };
}
} // namespace frost::debug
