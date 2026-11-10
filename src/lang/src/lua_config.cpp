// SPDX-License-Identifier: Apache-2.0
// LuaLS discovery (the bundled and pinned SDK installs first, ADR 0079), its launch arguments and the
// client's own default configuration. Hosts (FrostEngine) add theirs through lua_host.hpp (LUA-13).
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/diagnostics.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/document.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <format>
#include <tuple>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace frost::lang {
namespace {
#ifdef _WIN32
constexpr std::string_view kName = "lua-language-server.exe";
constexpr char kSeparator = ';';
#else
constexpr std::string_view kName = "lua-language-server";
constexpr char kSeparator = ':';
#endif
bool regular(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    return status && status->type == io::FileType::Regular;
}
std::optional<std::string> candidate(io::IFileSystem& fs, std::string_view dir) {
    if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"')
        dir = dir.substr(1, dir.size() - 2);
    if (!io::isAbsolutePath(dir))
        return std::nullopt;
    auto path = io::joinPath(dir, kName);
    if (!path)
        return std::nullopt;
    auto normalized = io::normalizePath(*path);
    if (normalized && regular(fs, *normalized))
        return std::move(*normalized);
    return std::nullopt;
}
// The value of an environment variable (UTF-8), or "" when unset.
std::string environment(const char* name) {
#ifdef _WIN32
    auto wide = io::toWide(name);
    if (!wide)
        return {};
    const DWORD needed = GetEnvironmentVariableW(wide->c_str(), nullptr, 0);
    if (needed == 0)
        return {};
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(wide->c_str(), value.data(), needed);
    if (length == 0 || length >= needed)
        return {};
    value.resize(length);
    auto utf8 = io::fromWide(value);
    return utf8 ? std::move(*utf8) : std::string();
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}
// The directory of this process's executable, or "" when it cannot be read.
std::string executableDirectory() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size())
        return {};
    buffer.resize(len);
    auto exePath = io::fromWide(buffer);
    return exePath ? std::string(io::parentPath(*exePath)) : std::string();
#elif defined(__linux__)
    std::string buffer(4096, '\0');
    const ssize_t len = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (len <= 0)
        return {};
    buffer.resize(static_cast<std::size_t>(len));
    return std::string(io::parentPath(buffer));
#else
    return {};
#endif
}
// <root>/luals-<pinned version>/bin, the layout of bootstrap's SDK install, tools/stage.ps1 and tools/stage.sh.
void addPinnedBin(std::vector<std::string>& out, std::string_view root) {
    if (root.empty())
        return;
    if (auto bin = io::joinPath(root, std::format("luals-{}/bin", pinnedLuaLanguageServerVersion())))
        out.push_back(std::move(*bin));
}
} // namespace
std::string_view pinnedLuaLanguageServerVersion() noexcept { return FROST_PINNED_LUALS_VERSION; }
Result<std::string> findLuaLanguageServer(io::IFileSystem& fs, const LuaSearch& search) {
    if (!search.configuredPath.empty()) {
        if (!io::isAbsolutePath(search.configuredPath))
            return fail(ErrorCode::InvalidArgument, "LuaLS path must be absolute");
        auto path = io::normalizePath(search.configuredPath);
        if (!path || !regular(fs, *path))
            return fail(ErrorCode::NotFound, "configured LuaLS executable does not exist");
        return *path;
    }
    for (const auto& dir : search.directories)
        if (auto path = candidate(fs, dir))
            return *path;
    std::string_view rest = search.pathVariable;
    while (!rest.empty()) {
        auto pos = rest.find(kSeparator);
        auto dir = rest.substr(0, pos);
        if (auto path = candidate(fs, dir))
            return *path;
        if (pos == std::string_view::npos)
            break;
        rest.remove_prefix(pos + 1);
    }
    return fail(ErrorCode::NotFound, std::format("LuaLS {} was not found in the bundled folder, the pinned SDK or PATH",
                                                 pinnedLuaLanguageServerVersion()));
}
LuaSearch defaultLuaSearch(std::string configuredPath) {
    LuaSearch search;
    search.configuredPath = std::move(configuredPath);
    // A staged FrostIDE bundles the pinned LuaLS beside the executable (tools/stage.ps1, tools/stage.sh).
    if (const std::string exeDir = executableDirectory(); !exeDir.empty()) {
        addPinnedBin(search.directories, exeDir);
        if (auto sub = io::joinPath(exeDir, "lua-language-server/bin")) // Layout of earlier previews.
            search.directories.push_back(std::move(*sub));
        search.directories.push_back(exeDir);
    }
    // Development and tests: the pinned install of tools/bootstrap.ps1 or tools/bootstrap-wsl.sh,
    // named by FROST_SDK (tools/env.ps1, ~/sdk/env.sh). No machine-specific default root (FND-10).
    addPinnedBin(search.directories, environment("FROST_SDK"));
    search.pathVariable = environment("PATH");
    return search;
}
Result<std::vector<std::string>> luaLanguageServerArguments(std::string_view stateDirectory) {
    if (!io::isAbsolutePath(stateDirectory))
        return fail(ErrorCode::InvalidArgument, "the LuaLS state directory must be absolute");
    auto log = io::joinPath(stateDirectory, "log");
    if (!log)
        return std::unexpected(std::move(log.error()));
    auto meta = io::joinPath(stateDirectory, "meta");
    if (!meta)
        return std::unexpected(std::move(meta.error()));
    return std::vector<std::string>{"--logpath=" + *log, "--metapath=" + *meta};
}
Json luaDefaults(std::string runtimeVersion) {
    Json runtime = Json::object().set("version", std::move(runtimeVersion));
    Json workspace = Json::object().set("checkThirdParty", false);
    Json hint = Json::object().set("enable", true);
    return Json::object()
        .set("runtime", std::move(runtime))
        .set("workspace", std::move(workspace))
        .set("hint", std::move(hint));
}
Json luaConfigurationSection(const Json& configuration, std::string_view section) {
    if (section == "Lua")
        return configuration;
    if (!section.starts_with("Lua."))
        return Json();
    section.remove_prefix(4);
    const Json* value = &configuration;
    while (!section.empty()) {
        auto dot = section.find('.');
        value = value->find(section.substr(0, dot));
        if (!value)
            return Json();
        if (dot == std::string_view::npos)
            return *value;
        section.remove_prefix(dot + 1);
    }
    return Json();
}
namespace {
std::string_view checkLevelName(DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::Error: return "Error";
    case DiagnosticSeverity::Warning: return "Warning";
    case DiagnosticSeverity::Information: return "Information";
    case DiagnosticSeverity::Hint: return "Hint";
    }
    return "Hint";
}
std::string_view trimAscii(std::string_view text) noexcept {
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    return text;
}
} // namespace
Result<std::vector<FileDiagnostics>> parseLuaCheckReport(std::string_view jsonText, io::IFileSystem* fs) {
    const std::string_view trimmed = trimAscii(jsonText);
    if (trimmed.empty())
        return std::vector<FileDiagnostics>{};
    auto parsed = parseJson(trimmed);
    if (!parsed)
        return std::unexpected(std::move(parsed.error()));
    if (parsed->isArray()) {
        if (parsed->size() == 0)
            return std::vector<FileDiagnostics>{};
        return fail(ErrorCode::Corrupt, "LuaLS --check report array is not empty");
    }
    if (!parsed->isObject())
        return fail(ErrorCode::Corrupt, "LuaLS --check report must be a JSON object or empty array");
    std::vector<FileDiagnostics> files;
    const auto& keys = parsed->keys();
    const auto& values = parsed->items();
    for (std::size_t i = 0; i < keys.size() && i < values.size(); ++i) {
        Json params = Json::object().set("uri", keys[i]).set("diagnostics", values[i]);
        auto decoded = decodePublishDiagnostics(params);
        if (!decoded)
            return std::unexpected(std::move(decoded.error()));
        if (decoded->diagnostics.empty())
            continue;
        if (fs != nullptr) {
            if (auto content = fs->readFile(decoded->file)) {
                if (auto doc = text::Document::fromUtf8(*content))
                    mapToSnapshot(*decoded, doc->text(), PositionEncoding::Utf16);
            }
        }
        std::stable_sort(decoded->diagnostics.begin(), decoded->diagnostics.end(),
                         [](const LiveDiagnostic& a, const LiveDiagnostic& b) {
                             return std::tie(a.line, a.column, a.code, a.message) <
                                    std::tie(b.line, b.column, b.code, b.message);
                         });
        files.push_back(std::move(*decoded));
    }
    std::ranges::sort(files, [](const FileDiagnostics& a, const FileDiagnostics& b) {
        return documentKey(a.file) < documentKey(b.file);
    });
    return files;
}
Result<std::vector<FileDiagnostics>> inspectLuaDirectory(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                         const LuaInspectOptions& options) {
    if (!options.trusted)
        return fail(ErrorCode::Untrusted, "refusing to launch LuaLS for an untrusted workspace");
    if (!io::isAbsolutePath(options.directory))
        return fail(ErrorCode::InvalidArgument, "the Lua inspect directory must be absolute");
    auto dirStatus = fs.status(options.directory);
    if (!dirStatus || (dirStatus->type != io::FileType::Directory && dirStatus->type != io::FileType::Regular))
        return fail(ErrorCode::NotFound, "the Lua inspect target does not exist");
    auto arguments = luaLanguageServerArguments(options.stateDirectory);
    if (!arguments)
        return std::unexpected(std::move(arguments.error()));
    auto server = findLuaLanguageServer(fs, options.search);
    if (!server)
        return std::unexpected(std::move(server.error()));
    if (auto created = fs.createDirectories(options.stateDirectory); !created)
        return std::unexpected(std::move(created.error()));
    static std::atomic<std::uint64_t> nextCheckId{0};
    auto outPath = io::joinPath(options.stateDirectory, std::format("check-{}.json", ++nextCheckId));
    if (!outPath)
        return std::unexpected(std::move(outPath.error()));
    (void)fs.remove(*outPath);
    io::ProcessSpec spec;
    spec.executable = *server;
    spec.workingDirectory =
        dirStatus->type == io::FileType::Directory ? options.directory : std::string(io::parentPath(options.directory));
    spec.arguments = std::move(*arguments);
    spec.arguments.push_back("--check=" + options.directory);
    spec.arguments.push_back("--checklevel=" + std::string(checkLevelName(options.minimumSeverity)));
    spec.arguments.emplace_back("--check_format=json");
    spec.arguments.push_back("--check_out_path=" + *outPath);
    if (!options.configPath.empty())
        spec.arguments.push_back("--configpath=" + options.configPath);
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::MergeIntoStdout;
    spec.killOnClose = true;
    auto completed = io::runToCompletion(launcher, spec, {}, options.timeout);
    if (!completed) {
        (void)fs.remove(*outPath);
        return std::unexpected(std::move(completed.error()));
    }
    auto reportText = fs.readFile(*outPath);
    (void)fs.remove(*outPath);
    if (!reportText) {
        if (reportText.error().code() == ErrorCode::NotFound && completed->exitCode == 0)
            return std::vector<FileDiagnostics>{};
        if (completed->exitCode != 0) {
            return fail(ErrorCode::Internal, std::format("LuaLS --check exited with code {}: {}", completed->exitCode,
                                                         completed->stdoutBytes));
        }
        return std::unexpected(std::move(reportText.error()));
    }
    auto parsed = parseLuaCheckReport(*reportText, &fs);
    if (!parsed)
        return std::unexpected(std::move(parsed.error()));
    for (FileDiagnostics& file : *parsed) {
        std::erase_if(file.diagnostics, [&](const LiveDiagnostic& d) {
            return static_cast<std::uint8_t>(d.severity) > static_cast<std::uint8_t>(options.minimumSeverity);
        });
    }
    std::erase_if(*parsed, [](const FileDiagnostics& file) { return file.diagnostics.empty(); });
    return parsed;
}
} // namespace frost::lang
