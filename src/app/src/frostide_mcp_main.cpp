// SPDX-License-Identifier: Apache-2.0
// Standalone stdio MCP proxy: current-user private discovery -> ACL pipe/AF_UNIX, never TCP.
#include "agent_private.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/local_ipc.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include "frost/rpc/json.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <vector>
namespace {
using frost::rpc::Json;
frost::Result<Json> discover(std::string path, std::string root) {
    auto& fs = frost::io::realFileSystem();
    if (root.empty()) {
        std::error_code error;
        const auto current = std::filesystem::current_path(error);
        if (error) return frost::fail(frost::ErrorCode::IoError, "cannot identify MCP client working directory");
        root = frost::io::fromStdPath(current);
    }
    const auto physical = fs.physicalPath(root);
    if (!physical) return std::unexpected(physical.error());
    const auto inspect = [&](std::string_view candidate) -> frost::Result<Json> {
        auto bytes = frost::app::readAgentPrivateFile(candidate);
        if (!bytes) return std::unexpected(bytes.error());
        auto record = frost::rpc::parseJson(*bytes);
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (!record || record->stringAt("kind") != "frostide-mcp" || !([&] { const auto prefix = frost::io::normalizePath(record->stringAt("root")); const auto current = frost::io::normalizePath(*physical); if (!prefix || !current) return false; return *current == *prefix || current->starts_with(*prefix +
#ifdef _WIN32
            "\\"
#else
            "/"
#endif
        ); })() ||
            record->integerAt("expires").value_or(0) <= now || record->stringAt("token").size() != 32 || record->stringAt("key").size() != 64)
            return frost::fail(frost::ErrorCode::PermissionDenied, "no consenting current-window MCP discovery matches this working directory");
        return *record;
    };
    if (!path.empty()) return inspect(path);
    std::string base;
    if (const char* configured = std::getenv("CLAUDE_CONFIG_DIR")) base = configured;
    else {
#ifdef _WIN32
        if (const char* home = std::getenv("USERPROFILE")) base = std::string(home) + "/.claude";
#else
        if (const char* home = std::getenv("HOME")) base = std::string(home) + "/.claude";
#endif
    }
    auto directory = frost::io::joinPath(base, "frostide");
    if (!directory) return std::unexpected(directory.error());
    auto files = fs.listDirectory(*directory);
    if (!files) return std::unexpected(files.error());
    std::optional<Json> match;
    for (const auto& file : *files) {
        if (file.type != frost::io::FileType::Regular || !file.name.ends_with(".discovery.json")) continue;
        const auto candidate = frost::io::joinPath(*directory, file.name);
        if (!candidate) continue;
        auto found = inspect(*candidate);
        if (!found) continue;
        if (match) return frost::fail(frost::ErrorCode::Busy, "multiple consenting windows match; configure --discovery explicitly");
        match = std::move(*found);
    }
    if (!match) return frost::fail(frost::ErrorCode::NotFound, "connect AI Agent to this workspace in FrostIDE first");
    return *match;
}
int run(const std::vector<std::string>& args) {
    std::string path, root;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if ((args[i] != "--discovery" && args[i] != "--root") || i + 1 == args.size()) { std::cerr << "frostide-mcp: invalid arguments\n"; return 2; }
        const bool discovery = args[i] == "--discovery";
        ++i;
        (discovery ? path : root) = args[i];
    }
    auto record = discover(path, root);
    if (!record) { std::cerr << "frostide-mcp: " << record.error().message() << '\n'; return 2; }
    std::string line;
    for (;;) {
        char byte = 0;
        if (!std::cin.get(byte)) {
            if (!line.empty()) { std::cerr << "frostide-mcp: incomplete input\n"; return 2; }
            return 0;
        }
        if (byte != '\n') {
            if (line.size() >= 60000) { std::cerr << "frostide-mcp: input exceeds IPC bound\n"; return 2; }
            line.push_back(byte); continue;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto request = frost::rpc::parseJson(line);
        line.clear();
        if (!request || !request->isObject()) { std::cerr << "frostide-mcp: invalid JSON-RPC input\n"; return 2; }
        auto envelope = Json::object().set("token", std::string(record->stringAt("token"))).set("root", std::string(record->stringAt("root")))
            .set("instance", std::string(record->stringAt("instance"))).set("request", *request).dump();
        if (envelope.size() > 65536) { std::cerr << "frostide-mcp: request exceeds IPC bound\n"; return 2; }
        auto response = frost::io::requestLocalIpc(record->stringAt("key"), envelope, std::chrono::seconds(5));
        if (!response) { std::cerr << "frostide-mcp: " << response.error().message() << '\n'; return 2; }
        if (request->find("id")) { std::cout << *response << '\n'; std::cout.flush(); }
    }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) { auto text = frost::io::fromWide(argv[i]); if (!text) return 2; args.push_back(std::move(*text)); }
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
