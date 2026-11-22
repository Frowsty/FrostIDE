// SPDX-License-Identifier: Apache-2.0
// Standalone stdio MCP proxy: current-user private discovery -> ACL pipe/AF_UNIX, never TCP.
#include "frost/io/file_system.hpp"
#include "frost/io/local_ipc.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include "frost/rpc/json.hpp"

#include "agent_private.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
namespace {
using frost::rpc::Json;
#ifdef _WIN32
std::optional<std::string> environmentVariable(const wchar_t* name) {
    wchar_t* value = nullptr;
    const auto status = _wdupenv_s(&value, nullptr, name);
    const std::unique_ptr<wchar_t, decltype(&std::free)> owned(value, &std::free);
    if (status != 0 || !owned)
        return std::nullopt;
    if (auto text = frost::io::fromWide(owned.get()))
        return std::move(*text);
    return std::nullopt;
}
#else
std::optional<std::string> environmentVariable(const char* name) {
    if (const char* value = std::getenv(name))
        return std::string(value);
    return std::nullopt;
}
#endif

frost::Result<Json> discover(const std::string& path, std::string root) {
    auto& fs = frost::io::realFileSystem();
    if (root.empty()) {
        std::error_code error;
        const auto current = std::filesystem::current_path(error);
        if (error)
            return frost::fail(frost::ErrorCode::IoError, "cannot identify MCP client working directory");
        auto converted = frost::io::fromStdPath(current);
        if (!converted)
            return std::unexpected(converted.error());
        root = std::move(*converted);
    }
    const auto physical = fs.physicalPath(root);
    if (!physical)
        return std::unexpected(physical.error());
    const auto inspect = [&](std::string_view candidate) -> frost::Result<Json> {
        if (!frost::io::fileName(candidate).ends_with(".discovery.json"))
            return frost::fail(frost::ErrorCode::InvalidArgument, "MCP discovery must name a discovery record");
        auto bytes = frost::app::readAgentPrivateFile(candidate);
        if (!bytes)
            return std::unexpected(bytes.error());
        auto record = frost::rpc::parseJson(*bytes);
        const auto now =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        if (!record || record->stringAt("kind") != "frostide-mcp" || !([&] {
                const auto prefix = frost::io::normalizePath(record->stringAt("root"));
                const auto current = frost::io::normalizePath(*physical);
                if (!prefix || !current)
                    return false;
                return *current == *prefix || current->starts_with(*prefix +
#ifdef _WIN32
                                                                   "\\"
#else
                                                                   "/"
#endif
                                              );
            })() ||
            record->integerAt("expires").value_or(0) <= now || record->stringAt("token").size() != 32 ||
            record->stringAt("key").size() != 64)
            return frost::fail(frost::ErrorCode::PermissionDenied,
                               "no consenting current-window MCP discovery matches this working directory");
        return *record;
    };
    if (!path.empty())
        return inspect(path);
    std::string base;
#ifdef _WIN32
    const auto configured = environmentVariable(L"CLAUDE_CONFIG_DIR");
    const auto home = configured ? std::nullopt : environmentVariable(L"USERPROFILE");
#else
    const auto configured = environmentVariable("CLAUDE_CONFIG_DIR");
    const auto home = configured ? std::nullopt : environmentVariable("HOME");
#endif
    if (configured)
        base = *configured;
    else if (home)
        base = *home + "/.claude";
    auto directory = frost::io::joinPath(base, "frostide");
    if (!directory)
        return std::unexpected(directory.error());
    auto files = fs.listDirectory(*directory);
    if (!files)
        return std::unexpected(files.error());
    std::optional<Json> match;
    for (const auto& file : *files) {
        if (file.type != frost::io::FileType::Regular || !file.name.ends_with(".discovery.json"))
            continue;
        const auto candidate = frost::io::joinPath(*directory, file.name);
        if (!candidate)
            continue;
        auto found = inspect(*candidate);
        if (!found)
            continue;
        if (match)
            return frost::fail(frost::ErrorCode::Busy,
                               "multiple consenting windows match; configure --discovery explicitly");
        match = std::move(*found);
    }
    if (!match)
        return frost::fail(frost::ErrorCode::NotFound, "connect AI Agent to this workspace in FrostIDE first");
    return *match;
}
int run(const std::vector<std::string>& args) {
    std::string path, root;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if ((args[i] != "--discovery" && args[i] != "--root") || i + 1 == args.size()) {
            std::cerr << "frostide-mcp: invalid arguments\n";
            return 2;
        }
        const bool discovery = args[i] == "--discovery";
        ++i;
        (discovery ? path : root) = args[i];
    }
    auto record = discover(path, root);
    if (!record) {
        std::cerr << "frostide-mcp: " << record.error().message() << '\n';
        return 2;
    }
    std::mutex output;
    std::atomic<bool> initialized{false};
    std::jthread notifications([&](const std::stop_token& stop) {
        std::int64_t revision = 0;
        while (!stop.stop_requested()) {
            if (initialized.load()) {
                const auto request = Json::object()
                                         .set("jsonrpc", "2.0")
                                         .set("id", "frostide-notification-poll")
                                         .set("method", "notifications/poll")
                                         .set("params", Json::object().set("after", revision));
                const auto envelope = Json::object()
                                          .set("token", std::string(record->stringAt("token")))
                                          .set("root", std::string(record->stringAt("root")))
                                          .set("instance", std::string(record->stringAt("instance")))
                                          .set("request", request)
                                          .dump();
                const auto response =
                    frost::io::requestLocalIpc(record->stringAt("key"), envelope, std::chrono::milliseconds(200));
                if (response)
                    if (auto parsed = frost::rpc::parseJson(*response); parsed)
                        if (const auto* result = parsed->find("result")) {
                            revision = result->integerAt("revision").value_or(revision);
                            if (const auto* events = result->find("notifications"); events && events->isArray()) {
                                std::lock_guard lock(output);
                                for (const auto& event : events->items())
                                    std::cout << event.dump() << '\n';
                                std::cout.flush();
                            }
                        }
            }
            for (int tick = 0; tick < 10 && !stop.stop_requested(); ++tick)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    std::string line;
    for (;;) {
        char byte = 0;
        if (!std::cin.get(byte)) {
            if (!line.empty()) {
                std::cerr << "frostide-mcp: incomplete input\n";
                return 2;
            }
            return 0;
        }
        if (byte != '\n') {
            if (line.size() >= 60000) {
                std::cerr << "frostide-mcp: input exceeds IPC bound\n";
                return 2;
            }
            line.push_back(byte);
            continue;
        }
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto request = frost::rpc::parseJson(line);
        line.clear();
        if (!request || !request->isObject()) {
            std::cerr << "frostide-mcp: invalid JSON-RPC input\n";
            return 2;
        }
        auto envelope = Json::object()
                            .set("token", std::string(record->stringAt("token")))
                            .set("root", std::string(record->stringAt("root")))
                            .set("instance", std::string(record->stringAt("instance")))
                            .set("request", *request)
                            .dump();
        if (envelope.size() > 65536) {
            std::cerr << "frostide-mcp: request exceeds IPC bound\n";
            return 2;
        }
        auto response = frost::io::requestLocalIpc(record->stringAt("key"), envelope, std::chrono::seconds(5));
        if (!response) {
            std::cerr << "frostide-mcp: " << response.error().message() << '\n';
            return 2;
        }

        if (request->find("id")) {
            std::lock_guard lock(output);
            std::cout << *response << '\n';
            std::cout.flush();
        }
        if (request->stringAt("method") == "initialize") {
            const auto parsed = frost::rpc::parseJson(*response);
            initialized.store(parsed && parsed->find("result"));
        }
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        auto text = frost::io::fromWide(argv[i]);
        if (!text)
            return 2;
        args.push_back(std::move(*text));
    }
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
