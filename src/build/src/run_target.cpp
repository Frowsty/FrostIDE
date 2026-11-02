// SPDX-License-Identifier: Apache-2.0
//
// What Run starts (see frost/build/run_target.hpp): the MSBuild property query and its JSON, the
// static fallback, macro and environment expansion, argument splitting, the PE subsystem and the
// process spec of each console mode.

#include "frost/build/run_target.hpp"

#include "frost/build/msbuild_driver.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include "json.hpp"

#include <array>
#include <cstring>
#include <format>
#include <optional>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#endif

namespace frost::build {

namespace {

constexpr io::PathStyle kWindows = io::PathStyle::Windows;
constexpr io::PathStyle kNative = io::kNativePathStyle;

constexpr std::array<std::string_view, 10> kRunPropertyNames = {
    "TargetPath",
    "TargetDir",
    "ProjectDir",
    "SolutionDir",
    "ConfigurationType",
    "LocalDebuggerCommand",
    "LocalDebuggerCommandArguments",
    "LocalDebuggerWorkingDirectory",
    "LocalDebuggerEnvironment",
    "LocalDebuggerMergeEnvironment",
};

constexpr char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = lowerAscii(c);
    }
    return out;
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

std::string_view trimmed(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<std::string> lookupEnvironment(const project::EnvironmentLookup& environment, std::string_view name) {
    return environment ? environment(name) : project::processEnvironmentVariable(name);
}

// `directory` with a trailing separator of `style`, as MSBuild spells $(ProjectDir).
std::string withTrailingSeparator(std::string directory, io::PathStyle style) {
    const char separator = style == kWindows ? '\\' : '/';
    if (!directory.empty() && directory.back() != '\\' && directory.back() != '/') {
        directory.push_back(separator);
    }
    return directory;
}

// `path` made absolute against `base` (a directory) and normalised; a path that already is
// absolute is only normalised. Errors: InvalidArgument (relative with no base, or malformed).
Result<std::string> absoluteAgainst(std::string_view path, std::string_view base) {
    if (io::isAbsolutePath(path, kNative)) {
        return io::normalizePath(path, kNative);
    }
    if (base.empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("'{}' is not an absolute path", path));
    }
    auto normalizedBase = io::normalizePath(base, kNative);
    if (!normalizedBase) {
        return std::unexpected(std::move(normalizedBase.error()));
    }
    if (path.empty()) {
        return normalizedBase;
    }
    auto joined = io::joinPath(*normalizedBase, path, kNative);
    if (!joined) {
        return std::unexpected(std::move(joined.error()));
    }
    return io::normalizePath(*joined, kNative);
}

std::string property(std::string_view name, std::string_view value) {
    return std::format("-p:{}={}", name, escapeMsBuildValue(value));
}

// The stem and the extension of a file name ("App.exe" -> "App", ".exe").
std::pair<std::string_view, std::string_view> splitExtension(std::string_view name) noexcept {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {name, {}};
    }
    return {name.substr(0, dot), name.substr(dot)};
}

// A little-endian integer of `bytes` at `offset`; nullopt when it does not fit.
template<typename T>
std::optional<T> readLittleEndian(std::string_view bytes, std::size_t offset) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
        return std::nullopt;
    }
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value = static_cast<T>(value | (static_cast<T>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i)));
    }
    return value;
}

} // namespace

std::span<const std::string_view> runPropertyNames() noexcept { return kRunPropertyNames; }

Result<io::ProcessSpec> runPropertyQuerySpec(const RunPropertyRequest& request) {
    auto msbuild = io::normalizePath(request.msbuildPath, kWindows);
    if (!msbuild) {
        return std::unexpected(std::move(msbuild.error()).withContext("MSBuild.exe path"));
    }
    auto project = io::normalizePath(request.projectPath, kWindows);
    if (!project) {
        return std::unexpected(std::move(project.error()).withContext("project path"));
    }
    if (request.configuration.empty() || request.platform.empty()) {
        return fail(ErrorCode::InvalidArgument, "a property query needs a configuration and a platform");
    }
    io::ProcessSpec spec;
    spec.executable = std::move(*msbuild);
    spec.arguments.push_back(*project);
    for (const std::string_view name : kRunPropertyNames) {
        spec.arguments.push_back(std::format("-getProperty:{}", name));
    }
    spec.arguments.push_back(property("Configuration", request.configuration));
    spec.arguments.push_back(property("Platform", request.platform));
    if (!request.solutionPath.empty()) {
        auto solution = io::normalizePath(request.solutionPath, kWindows);
        if (!solution) {
            return std::unexpected(std::move(solution.error()).withContext("solution path"));
        }
        // What Visual Studio passes for a project of a solution (as msbuildProcessSpec does).
        const std::string_view fileName = io::fileName(*solution, kWindows);
        const auto [stem, extension] = splitExtension(fileName);
        spec.arguments.push_back(
            property("SolutionDir", withTrailingSeparator(std::string(io::parentPath(*solution, kWindows)), kWindows)));
        spec.arguments.push_back(property("SolutionPath", *solution));
        spec.arguments.push_back(property("SolutionName", stem));
        spec.arguments.push_back(property("SolutionFileName", fileName));
        spec.arguments.push_back(property("SolutionExt", extension));
    }
    spec.arguments.emplace_back("-nologo");
    spec.workingDirectory = std::string(io::parentPath(*project, kWindows));
    spec.environment = {
        {"DOTNET_CLI_UI_LANGUAGE", "en-US"},
        {"MSBUILDTERMINALLOGGER", "off"},
        {"MSBUILDDISABLENODEREUSE", "1"},
        {"VSLANG", "1033"},
    };
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    return spec;
}

Result<RunProperties> parseRunProperties(std::string_view json) {
    auto document = detail::parseJson(json);
    if (!document) {
        return std::unexpected(std::move(document.error()).withContext("MSBuild's -getProperty output"));
    }
    const detail::JsonValue* properties = document->find("Properties");
    if (properties == nullptr || properties->kind != detail::JsonValue::Kind::Object) {
        return fail(ErrorCode::Corrupt, "MSBuild's -getProperty output has no \"Properties\" object");
    }
    RunProperties result;
    result.targetPath = properties->stringMember("TargetPath");
    result.targetDir = properties->stringMember("TargetDir");
    result.projectDir = properties->stringMember("ProjectDir");
    result.solutionDir = properties->stringMember("SolutionDir");
    result.configurationType = properties->stringMember("ConfigurationType");
    result.command = properties->stringMember("LocalDebuggerCommand");
    result.commandArguments = properties->stringMember("LocalDebuggerCommandArguments");
    result.workingDirectory = properties->stringMember("LocalDebuggerWorkingDirectory");
    result.environment = properties->stringMember("LocalDebuggerEnvironment");
    result.mergeEnvironment =
        !equalsIgnoringCase(trimmed(properties->stringMember("LocalDebuggerMergeEnvironment")), "false");
    result.fromMsBuild = true;
    return result;
}

Result<RunProperties> queryRunProperties(io::IProcessLauncher& launcher, const RunPropertyRequest& request,
                                         IClock::Duration timeout) {
#ifdef _WIN32
    auto spec = runPropertyQuerySpec(request);
    if (!spec) {
        return std::unexpected(std::move(spec.error()));
    }
    auto ran = io::runToCompletion(launcher, *spec, {}, timeout);
    if (!ran) {
        return std::unexpected(std::move(ran.error()));
    }
    if (ran->exitCode != 0) {
        const std::string output = ran->stdoutBytes + ran->stderrBytes;
        if (output.find("MSB1001") != std::string::npos) {
            return fail(
                ErrorCode::Unsupported,
                std::format("{} does not know -getProperty (MSBuild 17.8 or newer is needed)", spec->executable));
        }
        std::string_view shown = output;
        if (shown.size() > 2000) {
            shown = shown.substr(0, 2000);
        }
        return fail(ErrorCode::IoError,
                    std::format("MSBuild could not evaluate {} ({}|{}), exit code {}: {}", request.projectPath,
                                request.configuration, request.platform, ran->exitCode, trimmed(shown)));
    }
    return parseRunProperties(ran->stdoutBytes);
#else
    static_cast<void>(launcher);
    static_cast<void>(request);
    static_cast<void>(timeout);
    return fail(ErrorCode::Unsupported, "asking MSBuild for properties needs Windows");
#endif
}

Result<RunProperties> staticRunProperties(io::IFileSystem& fs, const RunPropertyRequest& request,
                                          const project::EnvironmentLookup& environment) {
    auto project = io::normalizePath(request.projectPath, kNative);
    if (!project) {
        return std::unexpected(std::move(project.error()).withContext("project path"));
    }
    auto content = fs.readFile(*project);
    if (!content) {
        return std::unexpected(std::move(content.error()));
    }
    auto values = project::parseConfigurationProperties(*content, *project, request.configuration, request.platform);
    if (!values) {
        return std::unexpected(std::move(values.error()));
    }
    // The per-user settings override the project's, as their import comes later.
    const std::string userPath = *project + ".user";
    if (auto user = fs.readFile(userPath)) {
        auto userValues =
            project::parseConfigurationProperties(*user, userPath, request.configuration, request.platform);
        if (!userValues) {
            return std::unexpected(std::move(userValues.error()));
        }
        for (auto& [name, value] : *userValues) {
            (*values)[name] = std::move(value);
        }
    } else if (user.error().code() != ErrorCode::NotFound) {
        return std::unexpected(std::move(user.error()));
    }
    const auto value = [&](std::string_view name) -> std::string {
        const auto it = values->find(lowered(name));
        return it == values->end() ? std::string() : it->second;
    };

    RunProperties result;
    result.fromMsBuild = false;
    result.warnings.push_back(
        std::format("MSBuild could not report the properties of {}, so they were read from the project file without "
                    "evaluation: settings from imported .props files are missing",
                    io::fileName(*project, kNative)));
    result.projectDir = withTrailingSeparator(std::string(io::parentPath(*project, kNative)), kNative);
    if (!request.solutionPath.empty()) {
        if (auto solution = io::normalizePath(request.solutionPath, kNative)) {
            result.solutionDir = withTrailingSeparator(std::string(io::parentPath(*solution, kNative)), kNative);
        }
    }
    if (result.solutionDir.empty()) {
        result.solutionDir = result.projectDir;
    }
    result.configurationType = value("ConfigurationType");

    // Expansion sees the names known so far; OutDir and TargetName may use them.
    RunMacros macros = runMacros(result, request);
    std::string outDir = value("OutDir");
    if (outDir.empty()) {
        outDir = equalsIgnoringCase(request.platform, "Win32") ? "$(SolutionDir)$(Configuration)\\"
                                                               : "$(SolutionDir)$(Platform)\\$(Configuration)\\";
    }
    outDir = expandRunMacros(outDir, macros, environment);
    for (char& c : outDir) {
        if constexpr (kNative == kWindows) {
            c = c == '/' ? '\\' : c;
        } else {
            c = c == '\\' ? '/' : c;
        }
    }
    auto targetDir = absoluteAgainst(outDir, result.projectDir);
    if (!targetDir) {
        return std::unexpected(std::move(targetDir.error()).withContext("OutDir"));
    }
    result.targetDir = withTrailingSeparator(*targetDir, kNative);
    std::string targetName = expandRunMacros(value("TargetName"), macros, environment);
    if (targetName.empty()) {
        targetName = macros["projectname"];
    }
    std::string targetExt = expandRunMacros(value("TargetExt"), macros, environment);
    if (targetExt.empty()) {
        targetExt = equalsIgnoringCase(result.configurationType, "DynamicLibrary") ? ".dll" : ".exe";
    }
    result.targetPath = result.targetDir + targetName + targetExt;
    result.command = value("LocalDebuggerCommand");
    result.commandArguments = value("LocalDebuggerCommandArguments");
    result.workingDirectory = value("LocalDebuggerWorkingDirectory");
    result.environment = value("LocalDebuggerEnvironment");
    result.mergeEnvironment = !equalsIgnoringCase(trimmed(value("LocalDebuggerMergeEnvironment")), "false");
    return result;
}

RunMacros runMacros(const RunProperties& properties, const RunPropertyRequest& request) {
    RunMacros macros;
    macros["projectdir"] = properties.projectDir;
    macros["targetdir"] = properties.targetDir;
    macros["targetpath"] = properties.targetPath;
    macros["solutiondir"] = properties.solutionDir;
    macros["configuration"] = request.configuration;
    macros["platform"] = request.platform;
    const std::string_view projectFile = io::fileName(request.projectPath, kNative);
    macros["projectname"] = std::string(splitExtension(projectFile).first);
    if (!properties.targetPath.empty()) {
        const auto [stem, extension] = splitExtension(io::fileName(properties.targetPath, kNative));
        macros["targetname"] = std::string(stem);
        macros["targetext"] = std::string(extension);
    }
    return macros;
}

std::string expandRunMacros(std::string_view text, const RunMacros& macros,
                            const project::EnvironmentLookup& environment) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '(') {
            const std::size_t close = text.find(')', i + 2);
            if (close == std::string_view::npos) {
                out.append(text.substr(i));
                break;
            }
            const std::string_view name = trimmed(text.substr(i + 2, close - i - 2));
            if (const auto it = macros.find(lowered(name)); it != macros.end()) {
                out += it->second;
            } else if (auto value = lookupEnvironment(environment, name)) {
                out += *value;
            }
            i = close + 1;
            continue;
        }
        out.push_back(text[i++]);
    }
    return out;
}

std::string expandPercentVariables(std::string_view text, const project::EnvironmentLookup& environment) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '%') {
            const std::size_t close = text.find('%', i + 1);
            if (close != std::string_view::npos && close > i + 1) {
                if (auto value = lookupEnvironment(environment, text.substr(i + 1, close - i - 1))) {
                    out += *value;
                    i = close + 1;
                    continue;
                }
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> parseDebuggerEnvironment(std::string_view text,
                                                                          const project::EnvironmentLookup& environment,
                                                                          std::vector<std::string>* warnings) {
    std::vector<std::pair<std::string, std::string>> variables;
    while (!text.empty()) {
        const std::size_t end = text.find_first_of("\r\n");
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (trimmed(line).empty()) {
            continue;
        }
        const std::size_t equals = line.find('=');
        const std::string_view name =
            equals == std::string_view::npos ? std::string_view() : trimmed(line.substr(0, equals));
        if (name.empty()) {
            if (warnings != nullptr) {
                warnings->push_back(std::format("environment line '{}' is not NAME=VALUE; it was skipped", line));
            }
            continue;
        }
        variables.emplace_back(std::string(name), expandPercentVariables(line.substr(equals + 1), environment));
    }
    return variables;
}

std::vector<std::string> splitWindowsArguments(std::string_view text) {
    std::vector<std::string> arguments;
    std::size_t i = 0;
    while (true) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        std::string argument;
        bool quoted = false;
        while (i < text.size()) {
            const char c = text[i];
            if (!quoted && (c == ' ' || c == '\t')) {
                break;
            }
            if (c == '\\') {
                std::size_t backslashes = 0;
                while (i < text.size() && text[i] == '\\') {
                    ++backslashes;
                    ++i;
                }
                if (i < text.size() && text[i] == '"') {
                    argument.append(backslashes / 2, '\\');
                    if (backslashes % 2 == 1) {
                        argument.push_back('"'); // 2n+1: a literal quote.
                        ++i;
                    }
                    // 2n: the quote is handled by the next iteration (a toggle).
                } else {
                    argument.append(backslashes, '\\');
                }
                continue;
            }
            if (c == '"') {
                if (quoted && i + 1 < text.size() && text[i + 1] == '"') {
                    argument.push_back('"'); // "" inside quotes: a literal quote.
                    i += 2;
                    continue;
                }
                quoted = !quoted;
                ++i;
                continue;
            }
            argument.push_back(c);
            ++i;
        }
        arguments.push_back(std::move(argument));
    }
    return arguments;
}

std::string_view toString(Subsystem subsystem) noexcept {
    switch (subsystem) {
    case Subsystem::Unknown: return "Unknown";
    case Subsystem::Console: return "Console";
    case Subsystem::Gui: return "GUI";
    }
    return "Unknown";
}

Result<Subsystem> peSubsystem(std::string_view image) {
    // IMAGE_DOS_HEADER: "MZ", e_lfanew at 0x3C. IMAGE_NT_HEADERS: "PE\0\0", then the 20-byte
    // IMAGE_FILE_HEADER, then the optional header, whose Subsystem field sits at offset 68 for both
    // PE32 and PE32+ (the fields before it have the same sizes in both).
    if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') {
        return fail(ErrorCode::Corrupt, "not a PE image: no MZ header");
    }
    const auto ntOffset = readLittleEndian<std::uint32_t>(image, 0x3C);
    if (!ntOffset || *ntOffset > image.size() || image.size() - *ntOffset < 4 ||
        std::memcmp(image.data() + *ntOffset, "PE\0\0", 4) != 0) {
        return fail(ErrorCode::Corrupt, "not a PE image: no PE signature");
    }
    const auto subsystem = readLittleEndian<std::uint16_t>(image, std::size_t{*ntOffset} + 4 + 20 + 68);
    if (!subsystem) {
        return fail(ErrorCode::Corrupt, "the PE optional header is truncated");
    }
    switch (*subsystem) {
    case 2: return Subsystem::Gui;     // IMAGE_SUBSYSTEM_WINDOWS_GUI
    case 3: return Subsystem::Console; // IMAGE_SUBSYSTEM_WINDOWS_CUI
    default: return Subsystem::Unknown;
    }
}

std::string_view toString(ConsoleMode mode) noexcept {
    switch (mode) {
    case ConsoleMode::RunConsole: return "Run console";
    case ConsoleMode::ExternalConsole: return "external console";
    case ConsoleMode::NoConsole: return "no console";
    }
    return "Run console";
}

std::string RunCommand::displayCommandLine() const {
    std::string line = std::format("\"{}\"", executable);
    if (!arguments.empty()) {
        line += ' ';
        line += arguments;
    }
    return line;
}

Result<RunCommand> resolveRunCommand(const RunProperties& properties, const RunMacros& macros,
                                     const project::EnvironmentLookup& environment) {
    RunCommand command;
    // trimmed() views the temporary, which lives until the end of the full expression.
    std::string executable(trimmed(expandRunMacros(properties.command, macros, environment)));
    if (executable.empty()) {
        executable = properties.targetPath;
    }
    // A command written with quotes ("C:\Tools\host.exe") names the same file.
    if (executable.size() >= 2 && executable.front() == '"' && executable.back() == '"') {
        executable = executable.substr(1, executable.size() - 2);
    }
    if (executable.empty()) {
        return fail(ErrorCode::InvalidArgument, "the project names no program to run (TargetPath is empty)");
    }
    auto absolute = absoluteAgainst(executable, {});
    if (!absolute) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the program to run, '{}', is not an absolute path", executable));
    }
    command.executable = std::move(*absolute);
    command.arguments = std::string(trimmed(expandRunMacros(properties.commandArguments, macros, environment)));
    std::string directory = std::string(trimmed(expandRunMacros(properties.workingDirectory, macros, environment)));
    if (directory.size() >= 2 && directory.front() == '"' && directory.back() == '"') {
        directory = directory.substr(1, directory.size() - 2);
    }
    auto workingDirectory = absoluteAgainst(directory, properties.projectDir);
    if (!workingDirectory) {
        return fail(ErrorCode::InvalidArgument, std::format("the working directory '{}' cannot be resolved: {}",
                                                            directory, workingDirectory.error().message()));
    }
    command.workingDirectory = std::move(*workingDirectory);
    command.environment = parseDebuggerEnvironment(expandRunMacros(properties.environment, macros, environment),
                                                   environment, &command.warnings);
    command.mergeEnvironment = properties.mergeEnvironment;
    return command;
}

Result<io::ProcessSpec> runProcessSpec(const RunCommand& command, ConsoleMode mode, std::string_view cmdPath) {
    if (command.executable.find('"') != std::string::npos) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the program path '{}' contains a quote", command.executable));
    }
    io::ProcessSpec spec;
    spec.workingDirectory = command.workingDirectory;
    spec.cleanEnvironment = !command.mergeEnvironment;
    if (command.mergeEnvironment) {
        spec.environment.emplace_back("NoDefaultCurrentDirectoryInExePath", std::nullopt);
    }
    for (const auto& [name, value] : command.environment) {
        spec.environment.emplace_back(name, value);
    }
    spec.killOnClose = true;
    switch (mode) {
    case ConsoleMode::RunConsole:
    case ConsoleMode::NoConsole:
        spec.executable = command.executable;
        spec.arguments = splitWindowsArguments(command.arguments);
        spec.stdinMode = mode == ConsoleMode::RunConsole ? io::Stdio::Pipe : io::Stdio::Null;
        spec.stdoutMode = io::Stdio::Pipe;
        spec.stderrMode = io::Stdio::Pipe;
        return spec;
    case ConsoleMode::ExternalConsole: {
        if (cmdPath.empty()) {
            return fail(ErrorCode::InvalidArgument, "an external console needs the path of cmd.exe");
        }
        spec.executable = std::string(cmdPath);
        /*
         * /d skips the AutoRun commands, /s strips exactly the outer quotes. In cmd's command-line
         * mode "%^errorlevel%" survives the first percent pass (no variable is named "^errorlevel"),
         * loses its caret, and `call` expands it at that point, after the program ran; pause does
         * not change ERRORLEVEL, so `exit` returns the program's code (measured on REF-W1,
         * 2026-11-02: 3, 0, 255 and 0xC0000005 came back unchanged).
         */
        std::string tail = std::format("/d /s /c \"\"{}\"", command.executable);
        if (!command.arguments.empty()) {
            tail += ' ';
            tail += command.arguments;
        }
        tail += " & call set FROST_RUN_EXIT=%^errorlevel%& echo.& call echo Process finished with exit code "
                "%^FROST_RUN_EXIT%.& pause & call exit %^FROST_RUN_EXIT%\"";
        spec.rawArguments = std::move(tail);
        spec.newConsole = true;
        spec.stdinMode = io::Stdio::Inherit;
        spec.stdoutMode = io::Stdio::Inherit;
        spec.stderrMode = io::Stdio::Inherit;
        return spec;
    }
    }
    return fail(ErrorCode::InvalidArgument, "unknown console mode");
}

Result<std::string> systemCommandProcessor() {
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return fail(ErrorCode::IoError, "GetSystemDirectoryW failed");
    }
    auto directory = io::fromWide(std::wstring_view(buffer.data(), length));
    if (!directory) {
        return std::unexpected(std::move(directory.error()));
    }
    return withTrailingSeparator(std::move(*directory), kWindows) + "cmd.exe";
#else
    return fail(ErrorCode::Unsupported, "cmd.exe exists on Windows only");
#endif
}

} // namespace frost::build
