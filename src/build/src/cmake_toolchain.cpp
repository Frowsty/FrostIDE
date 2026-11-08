// SPDX-License-Identifier: Apache-2.0
//
// The CMake toolchain (frost/build/cmake_toolchain.hpp): cmake and ninja discovery, the cmake
// version probe, and the vcvarsall.bat environment capture with its cache.

#include "frost/build/cmake_toolchain.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::build {

namespace {

constexpr bool kWindows = io::kNativePathStyle == io::PathStyle::Windows;
constexpr std::string_view kEnvironmentHeader = "# FrostIDE MSVC environment v1";

constexpr char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), lowerAscii);
    return out;
}

bool isRegularFile(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    return status && status->type == io::FileType::Regular;
}

Result<std::string> underInstallation(std::string_view installationPath, std::string_view relative) {
    auto base = io::normalizePath(installationPath, io::PathStyle::Windows);
    if (!base) {
        return std::unexpected(std::move(base.error()));
    }
    return io::joinPath(*base, relative, io::PathStyle::Windows);
}

// A settings tool path: absolute and a regular file.
Result<std::string> configuredTool(io::IFileSystem& fs, std::string_view path, std::string_view key) {
    auto normalized = io::normalizePath(path);
    if (!normalized) {
        return fail(ErrorCode::InvalidArgument, std::format("{} '{}' is not an absolute path", key, path));
    }
    if (!isRegularFile(fs, *normalized)) {
        return fail(ErrorCode::NotFound, std::format("{} '{}' does not exist", key, *normalized));
    }
    return normalized;
}

std::string onPath(io::IFileSystem& fs, std::span<const std::string> directories, std::string_view name) {
    const std::string file = kWindows ? std::string(name) + ".exe" : std::string(name);
    for (const std::string& directory : directories) {
        auto candidate = io::joinPath(directory, file);
        if (candidate && isRegularFile(fs, *candidate)) {
            return *candidate;
        }
    }
    return {};
}

// Splits "NAME=VALUE" lines into a name-keyed map (lower-cased keys on Windows) keeping the spelling.
std::map<std::string, std::pair<std::string, std::string>> readSetListing(std::string_view text) {
    std::map<std::string, std::pair<std::string, std::string>> out;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos || equals == 0) {
            continue;
        }
        const std::string name(line.substr(0, equals));
        out[kWindows ? lowered(name) : name] = {name, std::string(line.substr(equals + 1))};
    }
    return out;
}

bool hasUnsafeCharacter(std::string_view text) noexcept {
    return text.find_first_of("\"%^!") != std::string_view::npos;
}

} // namespace

std::string_view toString(ToolOrigin origin) noexcept {
    switch (origin) {
    case ToolOrigin::NotFound: return "not found";
    case ToolOrigin::Settings: return "settings";
    case ToolOrigin::VisualStudio: return "Visual Studio";
    case ToolOrigin::Path: return "PATH";
    case ToolOrigin::Sdk: return "pinned SDK";
    }
    return "not found";
}

std::string_view pinnedCMakeVersion() noexcept { return FROST_PINNED_CMAKE_VERSION; }

std::string_view pinnedNinjaVersion() noexcept { return FROST_PINNED_NINJA_VERSION; }

namespace {

// <sdkDirectory>/<relative>, normalised (native separators).
Result<std::string> underSdk(std::string_view sdkDirectory, std::string_view relative) {
    if (!io::isAbsolutePath(sdkDirectory)) {
        return fail(ErrorCode::InvalidArgument, std::format("the SDK directory '{}' is not absolute", sdkDirectory));
    }
    auto joined = io::joinPath(sdkDirectory, relative);
    if (!joined) {
        return std::unexpected(std::move(joined.error()));
    }
    return io::normalizePath(*joined);
}

} // namespace

Result<std::string> sdkCMakePath(std::string_view sdkDirectory) {
    return underSdk(sdkDirectory,
                    std::format("cmake-{}/bin/{}", pinnedCMakeVersion(), kWindows ? "cmake.exe" : "cmake"));
}

Result<std::string> sdkNinjaPath(std::string_view sdkDirectory) {
    return underSdk(sdkDirectory, std::format("ninja-{}/{}", pinnedNinjaVersion(), kWindows ? "ninja.exe" : "ninja"));
}

Result<std::string> vsBundledCMakePath(std::string_view installationPath) {
    return underInstallation(installationPath,
                             "Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe");
}

Result<std::string> vsBundledNinjaPath(std::string_view installationPath) {
    return underInstallation(installationPath, "Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\Ninja\\ninja.exe");
}

std::vector<std::string> splitPathVariable(std::string_view value) {
    const char separator = kWindows ? ';' : ':';
    std::vector<std::string> out;
    std::vector<std::string> keys;
    while (true) {
        const std::size_t end = value.find(separator);
        std::string_view entry = value.substr(0, end);
        if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') {
            entry = entry.substr(1, entry.size() - 2);
        }
        if (!entry.empty() && io::isAbsolutePath(entry)) {
            if (auto normalized = io::normalizePath(entry)) {
                const std::string key = kWindows ? lowered(*normalized) : *normalized;
                if (std::ranges::find(keys, key) == keys.end()) {
                    keys.push_back(key);
                    out.push_back(std::move(*normalized));
                }
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        value.remove_prefix(end + 1);
    }
    return out;
}

std::string processPathVariable() {
#ifdef _WIN32
    const DWORD needed = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"PATH", value.data(), needed);
    if (length == 0 || length >= needed) {
        return {};
    }
    value.resize(length);
    return io::fromWide(value).value_or(std::string());
#else
    const char* value = std::getenv("PATH"); // NOLINT(concurrency-mt-unsafe): nobody sets it after start-up.
    return value != nullptr ? std::string(value) : std::string();
#endif
}

Result<CMakeTools> findCMakeTools(io::IFileSystem& fs, const CMakeToolOptions& options) {
    CMakeTools tools;
    const std::vector<std::string> path = splitPathVariable(options.pathVariable);
    if (!options.cmakePath.empty()) {
        auto configured = configuredTool(fs, options.cmakePath, "tools.cmake.path");
        if (!configured) {
            return std::unexpected(std::move(configured.error()));
        }
        tools.cmake = std::move(*configured);
        tools.cmakeOrigin = ToolOrigin::Settings;
    }
    if (!options.ninjaPath.empty()) {
        auto configured = configuredTool(fs, options.ninjaPath, "tools.ninja.path");
        if (!configured) {
            return std::unexpected(std::move(configured.error()));
        }
        tools.ninja = std::move(*configured);
        tools.ninjaOrigin = ToolOrigin::Settings;
    }
    // The pinned SDK (header comment, step 2).
    if (!options.sdkDirectory.empty()) {
        if (auto pinned = sdkCMakePath(options.sdkDirectory);
            tools.cmake.empty() && pinned && isRegularFile(fs, *pinned)) {
            tools.cmake = std::move(*pinned);
            tools.cmakeOrigin = ToolOrigin::Sdk;
        }
        if (auto pinned = sdkNinjaPath(options.sdkDirectory);
            tools.ninja.empty() && pinned && isRegularFile(fs, *pinned)) {
            tools.ninja = std::move(*pinned);
            tools.ninjaOrigin = ToolOrigin::Sdk;
        }
    }
    if constexpr (kWindows) {
        for (const VsInstance& instance : options.vsInstances) {
            if (tools.cmake.empty()) {
                if (auto bundled = vsBundledCMakePath(instance.installationPath);
                    bundled && isRegularFile(fs, *bundled)) {
                    tools.cmake = *bundled;
                    tools.cmakeOrigin = ToolOrigin::VisualStudio;
                }
            }
            if (tools.ninja.empty()) {
                if (auto bundled = vsBundledNinjaPath(instance.installationPath);
                    bundled && isRegularFile(fs, *bundled)) {
                    tools.ninja = *bundled;
                    tools.ninjaOrigin = ToolOrigin::VisualStudio;
                }
            }
        }
    }
    if (tools.cmake.empty()) {
        tools.cmake = onPath(fs, path, "cmake");
        tools.cmakeOrigin = tools.cmake.empty() ? ToolOrigin::NotFound : ToolOrigin::Path;
    }
    if (tools.ninja.empty()) {
        tools.ninja = onPath(fs, path, "ninja");
        tools.ninjaOrigin = tools.ninja.empty() ? ToolOrigin::NotFound : ToolOrigin::Path;
    }
    if (tools.cmake.empty()) {
        return fail(ErrorCode::NotFound, "no cmake was found: set tools.cmake.path, install Visual Studio's C++ CMake "
                                         "tools, or put cmake on PATH");
    }
    return tools;
}

Result<std::string> cmakeVersion(io::IProcessLauncher& launcher, std::string_view cmakePath) {
    io::ProcessSpec spec;
    spec.executable = std::string(cmakePath);
    spec.arguments = {"--version"};
    auto result = io::runToCompletion(launcher, spec, {}, std::chrono::seconds(30));
    if (!result) {
        return std::unexpected(std::move(result.error()));
    }
    if (result->exitCode != 0) {
        return fail(ErrorCode::IoError, std::format("'{} --version' exited with code {}", cmakePath, result->exitCode));
    }
    constexpr std::string_view kPrefix = "cmake version ";
    std::string_view text = result->stdoutBytes;
    const std::size_t at = text.find(kPrefix);
    if (at == std::string_view::npos) {
        return fail(ErrorCode::Corrupt, std::format("'{} --version' printed no version", cmakePath));
    }
    std::string_view version = text.substr(at + kPrefix.size());
    version = version.substr(0, version.find_first_of(" \r\n"));
    return std::string(version);
}

bool versionAtLeast(std::string_view version, int major, int minor) noexcept {
    int parts[2] = {0, 0};
    for (int& part : parts) {
        const auto [end, error] = std::from_chars(version.data(), version.data() + version.size(), part);
        if (error != std::errc{}) {
            return false;
        }
        version.remove_prefix(static_cast<std::size_t>(end - version.data()));
        if (version.starts_with('.')) {
            version.remove_prefix(1);
        }
    }
    return parts[0] > major || (parts[0] == major && parts[1] >= minor);
}

bool isVcvarsArchitecture(std::string_view architecture) noexcept {
    static constexpr std::string_view kKnown[] = {"x86",         "x64",       "amd64",       "arm64",     "x86_amd64",
                                                  "x86_x64",     "x86_arm",   "x86_arm64",   "amd64_x86", "x64_x86",
                                                  "amd64_arm",   "x64_arm",   "amd64_arm64", "x64_arm64", "arm64_x64",
                                                  "arm64_amd64", "arm64_x86", "arm64_arm"};
    return std::ranges::find(kKnown, lowered(architecture)) != std::end(kKnown);
}

Result<std::string> vcvarsallPath(std::string_view installationPath) {
    return underInstallation(installationPath, "VC\\Auxiliary\\Build\\vcvarsall.bat");
}

Result<std::string> systemCmdPath() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return fail(ErrorCode::NotFound, "the system directory is unknown");
    }
    auto directory = io::fromWide(std::wstring_view(buffer, length));
    if (!directory) {
        return std::unexpected(std::move(directory.error()));
    }
    return io::joinPath(*directory, "cmd.exe");
#else
    return fail(ErrorCode::Unsupported, "cmd.exe exists on Windows only");
#endif
}

Result<io::ProcessSpec> vcvarsProcessSpec(std::string_view cmdPath, std::string_view vcvarsall,
                                          std::string_view architecture) {
    if (!io::isAbsolutePath(cmdPath, io::PathStyle::Windows) ||
        !io::isAbsolutePath(vcvarsall, io::PathStyle::Windows)) {
        return fail(ErrorCode::InvalidArgument, "cmd.exe and vcvarsall.bat must be absolute paths");
    }
    if (hasUnsafeCharacter(vcvarsall) || hasUnsafeCharacter(cmdPath)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("'{}' contains a character cmd.exe would interpret (\" % ^ !)", vcvarsall));
    }
    if (!isVcvarsArchitecture(architecture)) {
        return fail(ErrorCode::InvalidArgument, std::format("'{}' is not a vcvarsall.bat architecture", architecture));
    }
    io::ProcessSpec spec;
    spec.executable = std::string(cmdPath);
    spec.arguments = {"/d",
                      "/u",
                      "/c",
                      "set",
                      "&&",
                      "echo",
                      std::string(kVcvarsMarker),
                      "&&",
                      "call",
                      std::string(vcvarsall),
                      lowered(architecture),
                      ">nul",
                      "&&",
                      "set"};
    spec.workingDirectory = std::string(io::parentPath(vcvarsall, io::PathStyle::Windows));
    spec.environment = {{"VSCMD_SKIP_SENDTELEMETRY", std::string("1")}};
    spec.stdinMode = io::Stdio::Null;
    return spec;
}

Result<project::CMakeEnvironment> parseVcvarsCapture(std::string_view output) {
    const std::size_t marker = output.find(kVcvarsMarker);
    if (marker == std::string_view::npos) {
        return fail(ErrorCode::Corrupt, "the vcvarsall capture has no marker line");
    }
    const std::string_view before = output.substr(0, marker);
    std::string_view after = output.substr(marker + kVcvarsMarker.size());
    const auto oldValues = readSetListing(before);
    const auto newValues = readSetListing(after);
    if (newValues.empty()) {
        return fail(ErrorCode::Corrupt, "the vcvarsall capture lists no environment after the marker");
    }
    project::CMakeEnvironment changes;
    for (const auto& [key, entry] : newValues) {
        const auto old = oldValues.find(key);
        if (old == oldValues.end() || old->second.second != entry.second) {
            changes.emplace_back(entry.first, entry.second);
        }
    }
    for (const auto& [key, entry] : oldValues) {
        if (!newValues.contains(key)) {
            changes.emplace_back(entry.first, std::nullopt);
        }
    }
    return changes;
}

std::string decodeUtf16Le(std::string_view bytes) {
    std::u16string units(bytes.size() / 2, u'\0');
    if (!units.empty()) {
        std::memcpy(units.data(), bytes.data(), units.size() * 2);
    }
    // Little-endian bytes on every supported platform (x64).
    std::string out;
    out.reserve(units.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        char32_t code = units[i];
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            code = 0x10000 + ((code - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            ++i;
        } else if (code >= 0xD800 && code <= 0xDFFF) {
            code = 0xFFFD;
        }
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    return out;
}

Result<MsvcDevEnvironment> captureMsvcEnvironment(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                  const VsInstance& instance, std::string_view architecture) {
    if constexpr (!kWindows) {
        static_cast<void>(launcher);
        static_cast<void>(fs);
        static_cast<void>(instance);
        static_cast<void>(architecture);
        return fail(ErrorCode::Unsupported, "the MSVC environment exists on Windows only");
    } else {
        auto vcvarsall = vcvarsallPath(instance.installationPath);
        if (!vcvarsall) {
            return std::unexpected(std::move(vcvarsall.error()));
        }
        if (!isRegularFile(fs, *vcvarsall)) {
            return fail(ErrorCode::NotFound,
                        std::format("{} has no C++ build tools ({} is missing)", instance.displayName, *vcvarsall));
        }
        auto cmd = systemCmdPath();
        if (!cmd) {
            return std::unexpected(std::move(cmd.error()));
        }
        auto spec = vcvarsProcessSpec(*cmd, *vcvarsall, architecture);
        if (!spec) {
            return std::unexpected(std::move(spec.error()));
        }
        auto result = io::runToCompletion(launcher, *spec, {}, std::chrono::seconds(120));
        if (!result) {
            return std::unexpected(std::move(result.error()).withContext("while running vcvarsall.bat"));
        }
        if (result->exitCode != 0) {
            return fail(ErrorCode::IoError,
                        std::format("vcvarsall.bat {} failed with exit code {}", architecture, result->exitCode));
        }
        auto changes = parseVcvarsCapture(decodeUtf16Le(result->stdoutBytes));
        if (!changes) {
            return std::unexpected(std::move(changes.error()));
        }
        return MsvcDevEnvironment{.instanceId = instance.instanceId,
                                  .installationVersion = instance.installationVersion,
                                  .architecture = lowered(architecture),
                                  .changes = std::move(*changes)};
    }
}

std::string formatMsvcEnvironmentFile(const MsvcDevEnvironment& environment) {
    std::string text = std::format("{}\nversion\t{}\n", kEnvironmentHeader, environment.installationVersion);
    for (const auto& [name, value] : environment.changes) {
        if (value && value->find('\n') != std::string::npos) {
            continue; // Not representable in one line (never set by vcvarsall).
        }
        text += value ? std::format("set\t{}={}\n", name, *value) : std::format("unset\t{}\n", name);
    }
    return text;
}

std::optional<MsvcDevEnvironment> parseMsvcEnvironmentFile(std::string_view text) {
    MsvcDevEnvironment environment;
    bool header = false;
    bool version = false;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (!header) {
            if (line != kEnvironmentHeader) {
                return std::nullopt;
            }
            header = true;
            continue;
        }
        if (line.starts_with("version\t")) {
            environment.installationVersion = std::string(line.substr(8));
            version = true;
        } else if (line.starts_with("set\t")) {
            const std::string_view entry = line.substr(4);
            const std::size_t equals = entry.find('=');
            if (equals == std::string_view::npos || equals == 0) {
                return std::nullopt;
            }
            environment.changes.emplace_back(std::string(entry.substr(0, equals)),
                                             std::string(entry.substr(equals + 1)));
        } else if (line.starts_with("unset\t")) {
            environment.changes.emplace_back(std::string(line.substr(6)), std::nullopt);
        } else if (!line.empty()) {
            return std::nullopt;
        }
    }
    if (!header || !version) {
        return std::nullopt;
    }
    return environment;
}

MsvcEnvironmentCache::MsvcEnvironmentCache(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                           std::string cacheDirectory)
    : m_fs(fs), m_launcher(launcher), m_directory(std::move(cacheDirectory)) {}

std::string MsvcEnvironmentCache::cacheFile(std::string_view instanceId, std::string_view architecture) const {
    if (m_directory.empty()) {
        return {};
    }
    std::string name = std::format("msvc-env-{}-{}.txt", instanceId, lowered(architecture));
    std::ranges::replace_if(name, [](char c) { return c == '\\' || c == '/' || c == ':'; }, '_');
    return io::joinPath(m_directory, name).value_or(std::string());
}

std::size_t MsvcEnvironmentCache::captures() const {
    LockGuard lock(m_mutex);
    return m_captures;
}

Result<MsvcDevEnvironment> MsvcEnvironmentCache::get(const VsInstance& instance, std::string_view architecture) {
    const std::string key = instance.instanceId + '\t' + lowered(architecture);
    {
        LockGuard lock(m_mutex);
        if (const auto found = m_memory.find(key);
            found != m_memory.end() && found->second.installationVersion == instance.installationVersion) {
            return found->second;
        }
    }
    const std::string file = cacheFile(instance.instanceId, architecture);
    if (!file.empty()) {
        if (auto text = m_fs.readFile(file)) {
            if (auto cached = parseMsvcEnvironmentFile(*text);
                cached && cached->installationVersion == instance.installationVersion) {
                cached->instanceId = instance.instanceId;
                cached->architecture = lowered(architecture);
                LockGuard lock(m_mutex);
                m_memory[key] = *cached;
                return *cached;
            }
        }
    }
    {
        LockGuard lock(m_mutex);
        ++m_captures;
    }
    auto captured = captureMsvcEnvironment(m_launcher, m_fs, instance, architecture);
    if (!captured) {
        return captured;
    }
    if (!file.empty()) {
        if (m_fs.createDirectories(m_directory)) {
            static_cast<void>(m_fs.writeFileAtomic(file, formatMsvcEnvironmentFile(*captured)));
        }
    }
    LockGuard lock(m_mutex);
    m_memory[key] = *captured;
    return captured;
}

} // namespace frost::build
